/**
 * @file registry_admission.cpp
 * @brief Registers agents and admits idempotent dispatch requests.
 */

#include "include/dispatch_task_identity.h"
#include "include/manifest_validation.h"

#include <algorithm>
#include <utility>

namespace master_agent::agent_dispatch {

AgentDispatch::AgentDispatch(std::shared_ptr<IRuntimeClock> clock,
                                   std::shared_ptr<IdGenerator> ids)
    : clock_(std::move(clock)), ids_(std::move(ids)) {}

Status AgentDispatch::registerAgent(
    std::shared_ptr<sub_agents::ISubAgent> agent,
    const CallContext& call) {

    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        return Status::Error("agent_dispatch",
                             "AGENT_REGISTER_CALLER_NOT_ALLOWED",
                             "only AgentService may register agents");
    }
    if (!agent || !clock_ || !ids_ ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error("agent_dispatch",
                             "AGENT_REGISTRATION_INVALID",
                             "agent, clock and ids are required");
    }
    sub_agents::AgentManifest manifest;
    try {
        manifest = agent->getManifest();
    } catch (...) {
        return Status::Error(
            "agent_dispatch", "AGENT_MANIFEST_PROVIDER_EXCEPTION",
            "agent provider threw while reading its manifest", true,
            SideEffectState::NotApplicable);
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "agent_dispatch",
            "AGENT_MANIFEST_RESULT_AFTER_DEADLINE",
            "late manifest evidence cannot mutate the Agent registry",
            false, SideEffectState::NotApplicable);
    }
    // Complete the side-effect-free registration handshake before publishing
    // the Provider in the registry. The Provider sees Agent Dispatch as the
    // caller; Agent Service remains the only module allowed to request the
    // registration itself.
    bool heartbeat_ok = false;
    try {
        CallContext health_call{
            CallerModuleId::AgentDispatch,
            call.request_id + ":registration-health",
            call.trace_id,
            call.principal_id_hash,
            call.priority,
            call.deadline_mono_ns,
            {},
            0,
            call.authorization_ref};
        heartbeat_ok = agent->heartbeat(health_call);
    } catch (...) {
        return Status::Error(
            "agent_dispatch", "AGENT_HEARTBEAT_PROVIDER_EXCEPTION",
            "agent provider threw during the registration heartbeat",
            true, SideEffectState::NotApplicable);
    }
    if (!heartbeat_ok ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "agent_dispatch", "AGENT_HEARTBEAT_FAILED",
            "agent heartbeat failed during the registration handshake",
            true, SideEffectState::NotApplicable);
    }
    if (manifest.agent_id.empty() ||
        manifest.agent_id.size() > 128 ||
        manifest.agent_epoch == 0 ||
        manifest.manifest_digest.empty() ||
        manifest.manifest_digest.size() > 256 ||
        manifest.capability_version.empty() ||
        manifest.capability_version.size() > 64 ||
        manifest.capabilities.empty() ||
        !validBoundedUniqueClaims(manifest.capabilities) ||
        manifest.prompt_profile_id.empty() ||
        manifest.prompt_profile_id.size() > 128 ||
        manifest.model_profile_id.empty() ||
        manifest.model_profile_id.size() > 128 ||
        manifest.max_concurrency == 0 ||
        manifest.max_concurrency > 1024 ||
        manifest.reserved_p0_slots > manifest.max_concurrency) {
        return Status::Error("agent_dispatch",
                             "AGENT_MANIFEST_INVALID",
                             "agent manifest is incomplete");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto recovered_epoch =
        recovered_agent_epochs_.find(manifest.agent_id);
    if (recovered_epoch != recovered_agent_epochs_.end() &&
        manifest.agent_epoch < recovered_epoch->second) {
        return Status::Error(
            "agent_dispatch", "AGENT_EPOCH_STALE",
            "agent epoch is older than the durable registry fence");
    }
    const auto recovered_manifest =
        recovered_manifests_.find(manifest.agent_id);
    if (recovered_manifest != recovered_manifests_.end() &&
        manifest.agent_epoch ==
            recovered_manifest->second.agent_epoch &&
        (manifest.manifest_digest !=
             recovered_manifest->second.manifest_digest ||
         manifest.capability_version !=
             recovered_manifest->second.capability_version ||
         manifest.capabilities !=
             recovered_manifest->second.capabilities)) {
        return Status::Error(
            "agent_dispatch", "AGENT_RECOVERED_MANIFEST_CONFLICT",
            "same-epoch Provider does not match the durable manifest");
    }
    const auto existing = agents_.find(manifest.agent_id);
    if (existing != agents_.end() &&
        manifest.agent_epoch <= existing->second.manifest.agent_epoch) {
        return Status::Error("agent_dispatch", "AGENT_EPOCH_STALE",
                             "agent epoch must increase on replacement");
    }
    if (existing != agents_.end()) {
        const bool has_active_lease = std::any_of(
            dispatches_.begin(), dispatches_.end(),
            [&manifest](const auto& dispatch) {
                const auto state = dispatch.second.state;
                return dispatch.second.route.agent_id ==
                           manifest.agent_id &&
                       state != DispatchState::Succeeded &&
                       state != DispatchState::Failed &&
                       state != DispatchState::Cancelled;
            });
        if (has_active_lease) {
            return Status::Error(
                "agent_dispatch", "AGENT_REPLACEMENT_ACTIVE_LEASE",
                "agent epoch cannot change while a dispatch lease is active",
                true);
        }
    }
    const auto provider = agent;
    agents_[manifest.agent_id] = {manifest, std::move(agent), true};
    recovered_agent_epochs_[manifest.agent_id] = manifest.agent_epoch;
    recovered_manifests_[manifest.agent_id] = manifest;
    for (const auto& [dispatch_id, snapshot] : dispatches_) {
        if (snapshot.route.agent_id == manifest.agent_id &&
            snapshot.route.agent_epoch == manifest.agent_epoch &&
            snapshot.state != DispatchState::Succeeded &&
            snapshot.state != DispatchState::Failed &&
            snapshot.state != DispatchState::Cancelled) {
            dispatch_providers_[dispatch_id] = provider;
        }
    }
    ++capacity_epoch_;
    recordCapacityUnlocked();
    const auto persisted = persistStateUnlocked();
    durable_ = persisted.ok;
    return persisted;
}

DispatchAcceptance AgentDispatch::submitDispatch(
    const DispatchTask& task, const CallContext& call) {

    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_DRAINING"};
    }
    if (!hasHostModuleIdentity(
            call, CallerModuleId::TaskOrchestrationEngine) ||
        task.caller_module_id !=
            CallerModuleId::TaskOrchestrationEngine) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_CALLER_MODULE_NOT_ALLOWED"};
    }
    if (task.idempotency_key.empty() ||
        task.principal_id_hash.empty() ||
        call.request_id != task.request_id ||
        call.trace_id != task.trace_id ||
        call.principal_id_hash != task.principal_id_hash ||
        call.priority != task.priority ||
        call.authorization_ref != task.authorization_ref ||
        !isValidTaskPriority(call.priority) ||
        !isValidTaskPriority(task.priority) ||
        call.deadline_mono_ns != task.deadline_mono_ns) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_CALL_IDENTITY_MISMATCH"};
    }
    if (!clock_ || !ids_ || task.request_id.empty() ||
        task.plan_id.empty() || task.pid.empty() ||
        task.activation_id.empty() ||
        task.execution_id.empty() || task.operation_id.empty() ||
        task.task_id.empty() || task.action.empty() ||
        task.idempotency_key.empty() || task.fencing_token == 0 ||
        task.attempt_no == 0 || task.deadline_mono_ns <= 0 ||
        task.capability_digest.empty() || task.capacity_epoch == 0 ||
        task.principal_id_hash.empty() ||
        task.authorization_ref.empty() || task.trace_id.empty() ||
        !validBoundedUniqueClaims(task.granted_permissions) ||
        !validBoundedUniqueClaims(
            task.allowed_child_capabilities) ||
        deadlineExpired(task.deadline_mono_ns, *clock_)) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_TASK_INVALID"};
    }
    if (task.priority == TaskPriority::P0 &&
        task.authorization_ref.rfind("trusted-safety:", 0) != 0) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_P0_AUTHORIZATION_REQUIRED"};
    }
    const auto digest = taskDigest(task);
    const auto ledger_key = scopedIdempotencyLedgerKey(
        task.principal_id_hash, task.idempotency_key);
    const auto replay =
        idempotency_to_dispatch_.find(ledger_key);
    if (replay != idempotency_to_dispatch_.end()) {
        if (idempotency_digest_.at(ledger_key) != digest) {
            return {false, false, {}, task.operation_id,
                    "DISPATCH_IDEMPOTENCY_CONFLICT"};
        }
        const auto& existing = dispatches_.at(replay->second);
        return {true, true, existing.dispatch_id,
                existing.task.operation_id, {}};
    }
    if (task.capacity_epoch != capacity_epoch_) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_CAPACITY_EPOCH_STALE"};
    }
    if (task.capability_digest !=
        dispatchCapabilityDigest(
            task.action, task.input_schema_version,
            task.expected_output_schema_version)) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_CAPABILITY_DIGEST_MISMATCH"};
    }
    if (operation_to_dispatch_.count(task.operation_id) != 0) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_OPERATION_ID_CONFLICT"};
    }
    if (execution_to_dispatch_.count(task.execution_id) != 0) {
        return {false, false, {}, task.operation_id,
                "DISPATCH_EXECUTION_ID_CONFLICT"};
    }
    const auto decision = route(task);
    if (!decision.routed) {
        return {false, false, {}, task.operation_id,
                decision.reason_code};
    }

    DispatchSnapshot snapshot;
    snapshot.dispatch_id = ids_->next("dispatch");
    snapshot.task = task;
    snapshot.route = decision;
    snapshot.route.lease_id = ids_->next("agent-lease");
    snapshot.state = DispatchState::Accepted;
    snapshot.enqueue_sequence = ++enqueue_sequence_;
    const auto dispatch_id = snapshot.dispatch_id;
    dispatches_[dispatch_id] = std::move(snapshot);
    dispatch_providers_[dispatch_id] =
        agents_.at(decision.agent_id).provider;
    operation_to_dispatch_[task.operation_id] = dispatch_id;
    execution_to_dispatch_[task.execution_id] = dispatch_id;
    idempotency_to_dispatch_[ledger_key] = dispatch_id;
    idempotency_digest_[ledger_key] = digest;
    emit(dispatches_.at(dispatch_id), "ACCEPTED");
    setStateUnlocked(dispatches_.at(dispatch_id),
                     DispatchState::Queued);
    emit(dispatches_.at(dispatch_id), "QUEUED");
    if (!durable_) {
        return {false, false, dispatch_id, task.operation_id,
                "DISPATCH_DURABILITY_FAILED"};
    }
    return {true, false, dispatch_id, task.operation_id, {}};
}


}  // namespace master_agent::agent_dispatch
