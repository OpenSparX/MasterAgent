/**
 * @file sub_agent.cpp
 * @brief Implements the deterministic sub-agent lifecycle test double.
 */

#include "master_agent/sub_agents/sub_agent.h"

#include <algorithm>
#include <utility>

namespace master_agent::sub_agents {
namespace {

Status validateDispatchCaller(const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentDispatch) ||
        !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0) {
        return Status::Error("sub_agent", "SUB_AGENT_CALLER_NOT_ALLOWED",
                             "only AgentDispatch may call a SubAgent");
    }
    return Status::Ok();
}

bool isTerminal(SubAgentState state) {
    return state == SubAgentState::Succeeded ||
           state == SubAgentState::Failed ||
           state == SubAgentState::Cancelled;
}

std::string executionDigest(const SubAgentExecutionRequest& request) {
    const nlohmann::json encoded{
        {"dispatch_id", request.dispatch_id},
        {"request_id", request.request_id},
        {"pid", request.pid},
        {"activation_id", request.activation_id},
        {"attempt_no", request.attempt_no},
        {"operation_id", request.operation_id},
        {"execution_id", request.execution_id},
        {"agent_id", request.agent_id},
        {"agent_epoch", request.agent_epoch},
        {"manifest_digest", request.manifest_digest},
        {"lease_id", request.lease_id},
        {"action", request.action},
        {"params", request.params},
        {"capability_digest", request.capability_digest},
        {"expected_output_schema_version",
         request.expected_output_schema_version},
        {"priority", toString(request.priority)},
        {"deadline_mono_ns", request.deadline_mono_ns},
        {"fencing_token", request.fencing_token},
        {"trace_id", request.trace_id},
        {"principal_id_hash", request.principal_id_hash},
        {"authorization_ref", request.authorization_ref}};
    return secureDigest(encoded.dump());
}

}  // namespace

DeterministicSubAgent::DeterministicSubAgent(
    AgentManifest manifest, std::shared_ptr<IRuntimeClock> clock,
    std::shared_ptr<IdGenerator> ids, std::uint32_t work_units)
    : manifest_(std::move(manifest)),
      clock_(std::move(clock)),
      ids_(std::move(ids)),
      work_units_(std::max<std::uint32_t>(1, work_units)) {
    if (manifest_.manifest_digest.empty()) {
        // The generated reference-runtime manifest seal includes child authorization
        // requirements so a changed grant contract produces a new immutable
        // snapshot identity.
        const nlohmann::json contract{
            {"agent_id", manifest_.agent_id},
            {"agent_epoch", manifest_.agent_epoch},
            {"capability_version", manifest_.capability_version},
            {"capabilities", manifest_.capabilities},
            {"required_permissions",
             manifest_.required_permissions},
            {"required_atomic_tools",
             manifest_.required_atomic_tools},
            {"max_concurrency", manifest_.max_concurrency},
            {"reserved_p0_slots", manifest_.reserved_p0_slots},
            {"supports_safe_point_preemption",
             manifest_.supports_safe_point_preemption},
            {"prompt_profile_id", manifest_.prompt_profile_id},
            {"model_profile_id", manifest_.model_profile_id}};
        manifest_.manifest_digest =
            secureDigest(contract.dump());
    }
}

AgentManifest DeterministicSubAgent::getManifest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return manifest_;
}

SubAgentAcceptance DeterministicSubAgent::submit(
    const SubAgentExecutionRequest& request, const CallContext& call) {

    std::lock_guard<std::mutex> lock(mutex_);
    const auto caller = validateDispatchCaller(call);
    if (!caller.ok) {
        return {false, request.dispatch_id, caller.error.code};
    }
    if (request.dispatch_id.empty() || request.operation_id.empty() ||
        request.request_id.empty() || request.pid.empty() ||
        request.activation_id.empty() || request.attempt_no == 0 ||
        request.execution_id.empty() || request.agent_id.empty() ||
        request.agent_epoch == 0 || request.manifest_digest.empty() ||
        request.lease_id.empty() || request.action.empty() ||
        request.capability_digest.empty() ||
        request.expected_output_schema_version == 0 ||
        request.trace_id.empty() ||
        request.principal_id_hash.empty() ||
        request.authorization_ref.empty() ||
        request.fencing_token == 0 || request.deadline_mono_ns <= 0 ||
        deadlineExpired(request.deadline_mono_ns, *clock_)) {
        return {false, request.dispatch_id, "SUB_AGENT_REQUEST_INVALID"};
    }
    if (request.agent_id != manifest_.agent_id ||
        request.agent_epoch != manifest_.agent_epoch ||
        request.manifest_digest != manifest_.manifest_digest) {
        return {false, request.dispatch_id,
                "SUB_AGENT_MANIFEST_IDENTITY_MISMATCH"};
    }
    if (call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash != request.principal_id_hash ||
        call.priority != request.priority ||
        call.deadline_mono_ns != request.deadline_mono_ns ||
        call.authorization_ref != request.authorization_ref) {
        return {false, request.dispatch_id,
                "SUB_AGENT_CALL_IDENTITY_MISMATCH"};
    }
    const auto digest = executionDigest(request);
    const auto existing = runtimes_.find(request.dispatch_id);
    if (existing != runtimes_.end()) {
        if (existing->second.request_digest != digest) {
            return {false, request.dispatch_id,
                    "SUB_AGENT_IDEMPOTENCY_CONFLICT"};
        }
        return {true, request.dispatch_id, {}};
    }
    for (const auto& pair : runtimes_) {
        if (pair.second.snapshot.request.operation_id ==
                request.operation_id ||
            pair.second.snapshot.request.execution_id ==
                request.execution_id) {
            return {false, request.dispatch_id,
                    "SUB_AGENT_EXECUTION_ID_CONFLICT"};
        }
    }
    std::size_t active = 0;
    for (const auto& pair : runtimes_) {
        if (!isTerminal(pair.second.snapshot.state) &&
            pair.second.snapshot.state != SubAgentState::Suspended) {
            ++active;
        }
    }
    if (active >= manifest_.max_concurrency) {
        return {false, request.dispatch_id, "SUB_AGENT_CAPACITY_EXHAUSTED"};
    }
    Runtime runtime;
    runtime.snapshot.request = request;
    runtime.snapshot.output_schema_version =
        request.expected_output_schema_version;
    runtime.snapshot.state = SubAgentState::Running;
    runtime.remaining_work_units = work_units_;
    runtime.request_digest = digest;
    runtimes_[request.dispatch_id] = std::move(runtime);
    return {true, request.dispatch_id, {}};
}

Result<SubAgentSnapshot> DeterministicSubAgent::query(
    const std::string& dispatch_id, const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto caller = validateDispatchCaller(call);
    if (!caller.ok) {
        return Result<SubAgentSnapshot>::Failure(caller);
    }
    const auto found = runtimes_.find(dispatch_id);
    if (found == runtimes_.end()) {
        return Result<SubAgentSnapshot>::Failure(Status::Error(
            "sub_agent", "SUB_AGENT_EXECUTION_NOT_FOUND",
            "sub agent execution was not found"));
    }
    const auto& request = found->second.snapshot.request;
    if (deadlineExpired(call.deadline_mono_ns, *clock_) ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash != request.principal_id_hash ||
        call.priority != request.priority ||
        call.authorization_ref != request.authorization_ref) {
        return Result<SubAgentSnapshot>::Failure(Status::Error(
            "sub_agent", "SUB_AGENT_CALL_IDENTITY_MISMATCH",
            "query is not bound to the active dispatch identity"));
    }
    return Result<SubAgentSnapshot>::Success(found->second.snapshot);
}

Status DeterministicSubAgent::requestPreempt(
    const std::string& dispatch_id, std::uint64_t control_epoch,
    const CallContext& call) {

    std::lock_guard<std::mutex> lock(mutex_);
    const auto caller = validateDispatchCaller(call);
    if (!caller.ok) return caller;
    auto found = runtimes_.find(dispatch_id);
    if (found == runtimes_.end()) {
        return Status::Error("sub_agent",
                             "SUB_AGENT_EXECUTION_NOT_FOUND",
                             "execution was not found");
    }
    const auto& request = found->second.snapshot.request;
    if (control_epoch == 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash != request.principal_id_hash ||
        call.priority != request.priority ||
        call.authorization_ref != request.authorization_ref) {
        return Status::Error(
            "sub_agent", "SUB_AGENT_CALL_IDENTITY_MISMATCH",
            "preempt is not bound to the active dispatch identity");
    }
    if (control_epoch < found->second.control_epoch) {
        return Status::Error("sub_agent", "SUB_AGENT_CONTROL_EPOCH_STALE",
                             "control epoch is stale");
    }
    if (control_epoch == found->second.control_epoch &&
        control_epoch != 0) {
        return Status::Ok();
    }
    if (found->second.snapshot.state != SubAgentState::Running ||
        !manifest_.supports_safe_point_preemption) {
        return Status::Error("sub_agent", "SUB_AGENT_PREEMPT_NOT_APPLICABLE",
                             "execution cannot be preempted");
    }
    found->second.control_epoch = control_epoch;
    found->second.snapshot.control_epoch = control_epoch;
    found->second.snapshot.checkpoint_ref =
        "sub-checkpoint:" + dispatch_id + ":" +
        std::to_string(found->second.remaining_work_units);
    found->second.snapshot.state = SubAgentState::Suspended;
    found->second.snapshot.resource_released = true;
    return Status::Ok();
}

Status DeterministicSubAgent::restore(
    const std::string& dispatch_id, const std::string& checkpoint_ref,
    std::uint64_t control_epoch, const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto caller = validateDispatchCaller(call);
    if (!caller.ok) return caller;
    auto found = runtimes_.find(dispatch_id);
    if (found == runtimes_.end()) {
        return Status::Error("sub_agent",
                             "SUB_AGENT_EXECUTION_NOT_FOUND",
                             "execution was not found");
    }
    const auto& request = found->second.snapshot.request;
    if (control_epoch == 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash != request.principal_id_hash ||
        call.priority != request.priority ||
        call.authorization_ref != request.authorization_ref) {
        return Status::Error(
            "sub_agent", "SUB_AGENT_CALL_IDENTITY_MISMATCH",
            "restore is not bound to the active dispatch identity");
    }
    if (control_epoch < found->second.control_epoch) {
        return Status::Error("sub_agent", "SUB_AGENT_CONTROL_EPOCH_STALE",
                             "control epoch is stale");
    }
    if (control_epoch == found->second.control_epoch &&
        control_epoch != 0) {
        return Status::Ok();
    }
    if (found->second.snapshot.state != SubAgentState::Suspended ||
        found->second.snapshot.checkpoint_ref != checkpoint_ref) {
        return Status::Error("sub_agent", "SUB_AGENT_RESTORE_INVALID",
                             "checkpoint cannot restore execution");
    }
    found->second.control_epoch = control_epoch;
    found->second.snapshot.control_epoch = control_epoch;
    found->second.snapshot.state = SubAgentState::Running;
    found->second.snapshot.resource_released = false;
    return Status::Ok();
}

Status DeterministicSubAgent::cancel(
    const std::string& dispatch_id, std::uint64_t control_epoch,
    const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto caller = validateDispatchCaller(call);
    if (!caller.ok) return caller;
    auto found = runtimes_.find(dispatch_id);
    if (found == runtimes_.end()) {
        return Status::Error("sub_agent", "SUB_AGENT_EXECUTION_NOT_FOUND",
                             "execution was not found");
    }
    const auto& request = found->second.snapshot.request;
    if (control_epoch == 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash != request.principal_id_hash ||
        call.priority != request.priority ||
        call.authorization_ref != request.authorization_ref) {
        return Status::Error(
            "sub_agent", "SUB_AGENT_CALL_IDENTITY_MISMATCH",
            "cancel is not bound to the active dispatch identity");
    }
    if (control_epoch < found->second.control_epoch) {
        return Status::Error("sub_agent", "SUB_AGENT_CONTROL_EPOCH_STALE",
                             "control epoch is stale");
    }
    if (control_epoch == found->second.control_epoch &&
        control_epoch != 0) {
        return Status::Ok();
    }
    found->second.control_epoch = control_epoch;
    found->second.snapshot.control_epoch = control_epoch;
    if (!isTerminal(found->second.snapshot.state)) {
        found->second.snapshot.state = SubAgentState::Cancelled;
        found->second.snapshot.resource_released = true;
    }
    return Status::Ok();
}

bool DeterministicSubAgent::pumpOne() {

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& pair : runtimes_) {
        auto& runtime = pair.second;
        if (runtime.snapshot.state != SubAgentState::Running) continue;
        if (deadlineExpired(runtime.snapshot.request.deadline_mono_ns,
                            *clock_)) {
            runtime.snapshot.state = SubAgentState::Failed;
            runtime.snapshot.error_code = "SUB_AGENT_DEADLINE_EXPIRED";
            runtime.snapshot.resource_released = true;
            return true;
        }
        if (runtime.remaining_work_units > 0) {
            --runtime.remaining_work_units;
        }
        if (runtime.remaining_work_units == 0) {
            runtime.snapshot.state = SubAgentState::Succeeded;
            runtime.snapshot.side_effect_state = SideEffectState::Committed;
            runtime.snapshot.resource_released = true;
            runtime.snapshot.result =
                nlohmann::json{{"success", true},
                               {"agent_id", manifest_.agent_id},
                               {"output_schema_version",
                                runtime.snapshot
                                    .output_schema_version},
                               {"action", runtime.snapshot.request.action},
                               {"summary", u8"子 Agent 模拟任务已完成"}};
        }
        return true;
    }
    return false;
}

bool DeterministicSubAgent::heartbeat(const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return validateDispatchCaller(call).ok && !manifest_.agent_id.empty();
}

}  // namespace master_agent::sub_agents
