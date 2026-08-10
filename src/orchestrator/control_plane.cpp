/**
 * @file control_plane.cpp
 * @brief Implements the production orchestration control and event surfaces.
 *
 * Design trace: Task Orchestration Engine Detailed Design, sections 3.1, 3.4, 5.6,
 * 5.10, 5.11, and 5.12. The implementation remains in the same process as
 * Agent Service; module identity checks preserve the documented trust boundary.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {
namespace {

bool executionCallerAllowed(const CallContext& call) {
    return hasHostModuleIdentity(
               call, CallerModuleId::AtomicServiceManager) ||
           hasHostModuleIdentity(
               call, CallerModuleId::AgentDispatch);
}

const nlohmann::json* readBindingPath(
    const nlohmann::json& root, const std::string& path) {
    const nlohmann::json* current = &root;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto separator = path.find('.', begin);
        const auto name = path.substr(
            begin, separator == std::string::npos
                       ? std::string::npos
                       : separator - begin);
        if (name.empty() || !current->is_object()) return nullptr;
        const auto found = current->find(name);
        if (found == current->end()) return nullptr;
        current = &*found;
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return current;
}

bool writeBindingPath(
    nlohmann::json* root, const std::string& path,
    const nlohmann::json& value) {
    if (!root || !root->is_object() || path.empty()) return false;
    nlohmann::json* current = root;
    std::size_t begin = 0;
    while (begin < path.size()) {
        const auto separator = path.find('.', begin);
        const auto name = path.substr(
            begin, separator == std::string::npos
                       ? std::string::npos
                       : separator - begin);
        if (name.empty()) return false;
        if (separator == std::string::npos) {
            (*current)[name] = value;
            return true;
        }
        auto& child = (*current)[name];
        if (child.is_null()) child = nlohmann::json::object();
        if (!child.is_object()) return false;
        current = &child;
        begin = separator + 1;
    }
    return false;
}

bool executionIdentityMatches(
    const NodeRuntimeSnapshot& node,
    const std::string& pid,
    const std::string& activation_id,
    const std::string& execution_id,
    const std::string& operation_id,
    std::uint32_t attempt_no,
    std::uint64_t fencing_token) {
    return node.pid == pid &&
           node.activation_id == activation_id &&
           node.execution_id == execution_id &&
           node.operation_id == operation_id &&
           node.attempt_count == attempt_no &&
           node.fencing_token == fencing_token;
}

bool executorMatchesCaller(
    const NodeRuntimeSnapshot& node,
    const CallContext& call) {
    if (node.definition.executor == "atomic_service") {
        return hasHostModuleIdentity(
            call, CallerModuleId::AtomicServiceManager);
    }
    return node.definition.executor == "agent_dispatch" &&
           hasHostModuleIdentity(
               call, CallerModuleId::AgentDispatch);
}

}  // namespace

Status Orchestrator::validateControlRequest(
    const PlanControlRequest& request, const CallContext& call,
    const PlanRuntime& plan) const {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) return caller;
    if (!clock_ || request.plan_id != plan.snapshot.plan_id ||
        request.reason.empty() ||
        request.deadline_mono_ns <= 0 ||
        request.deadline_mono_ns != call.deadline_mono_ns ||
        deadlineExpired(request.deadline_mono_ns, *clock_) ||
        request.expected_plan_version != plan.snapshot.version ||
        request.control_epoch <= plan.snapshot.control_epoch ||
        call.request_id != plan.snapshot.request_id ||
        call.trace_id != plan.snapshot.trace_id ||
        call.principal_id_hash !=
            plan.admission.principal_id_hash) {
        return Status::Error(
            "orchestrator", "ORCHESTRATOR_CONTROL_CONFLICT",
            "control identity, epoch, version, or deadline is invalid");
    }
    return Status::Ok();
}

std::optional<std::pair<std::string, std::string>>
Orchestrator::findExecutionUnlocked(
    const std::string& execution_id) const {
    for (const auto& [plan_id, plan] : plans_) {
        for (const auto& [node_id, node] :
             plan.snapshot.nodes) {
            if (node.execution_id == execution_id) {
                return std::make_pair(plan_id, node_id);
            }
        }
    }
    return std::nullopt;
}

bool Orchestrator::bindResultsUnlocked(
    const PlanRuntime& plan,
    const NodeRuntimeSnapshot& node,
    nlohmann::json* params,
    std::string* error_code) const {
    if (!params || !error_code ||
        !node.definition.params.is_object()) {
        return false;
    }
    *params = node.definition.params;
    error_code->clear();
    for (const auto& binding :
         node.definition.result_bindings) {
        const auto source =
            plan.snapshot.nodes.find(
                binding.source_node_id);
        const nlohmann::json* value = nullptr;
        if (source != plan.snapshot.nodes.end() &&
            source->second.state ==
                ActivationState::Succeeded &&
            source->second.definition
                    .expected_output_schema_version ==
                binding.source_schema_version &&
            node.definition.input_schema_version ==
                binding.target_schema_version) {
            value = readBindingPath(
                source->second.result,
                binding.source_field_path);
        }
        nlohmann::json selected;
        if (!value) {
            if (binding.on_missing == "USE_DEFAULT") {
                selected = binding.default_value;
            } else {
                *error_code =
                    binding.on_missing == "SKIP_PATH"
                        ? "ORCHESTRATOR_BINDING_SKIP_PATH"
                        : "ORCHESTRATOR_RESULT_BINDING_FAILED";
                return false;
            }
        } else {
            // Only IDENTITY is registered by this delivery. Validation rejects
            // every other transform before the plan is committed.
            if (binding.transform_id != "IDENTITY") {
                *error_code =
                    "ORCHESTRATOR_RESULT_TRANSFORM_NOT_REGISTERED";
                return false;
            }
            selected = *value;
        }
        if (!writeBindingPath(
                params, binding.target_param, selected)) {
            *error_code =
                "ORCHESTRATOR_RESULT_BINDING_TARGET_INVALID";
            return false;
        }
    }
    return true;
}

CapacityAck Orchestrator::updateDispatchCapacity(
    const DispatchCapacity& capacity,
    const CallContext& call) {
    if (!executionCallerAllowed(call) || !clock_ ||
        capacity.target_id.empty() ||
        capacity.capacity_epoch == 0 ||
        capacity.available_credits >
            capacity.max_inflight ||
        capacity.valid_until_mono_ns <=
            clock_->monotonicNowNs()) {
        return {false, 0,
                "ORCHESTRATOR_CAPACITY_UPDATE_INVALID"};
    }
    const bool caller_target_match =
        (hasHostModuleIdentity(
             call, CallerModuleId::AtomicServiceManager) &&
         capacity.target_id == "atomic_service") ||
        (hasHostModuleIdentity(
             call, CallerModuleId::AgentDispatch) &&
         capacity.target_id == "agent_dispatch");
    if (!caller_target_match) {
        return {false, 0,
                "ORCHESTRATOR_CAPACITY_SOURCE_MISMATCH"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing =
        dispatch_capacity_.find(capacity.target_id);
    if (existing != dispatch_capacity_.end() &&
        capacity.capacity_epoch <
            existing->second.capacity_epoch) {
        return {false, existing->second.capacity_epoch,
                "ORCHESTRATOR_CAPACITY_EPOCH_STALE"};
    }
    dispatch_capacity_[capacity.target_id] = capacity;
    return {true, capacity.capacity_epoch, {}};
}

ExecutionEventAck Orchestrator::ackDispatch(
    const DispatchAcceptance& acceptance,
    const CallContext& call) {
    if (!executionCallerAllowed(call) || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, 0,
                "ORCHESTRATOR_EXECUTION_CALLER_INVALID"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto located =
        findExecutionUnlocked(acceptance.execution_id);
    if (!located) {
        return {false, false, 0,
                "ORCHESTRATOR_EXECUTION_NOT_FOUND"};
    }
    auto& plan = plans_.at(located->first);
    auto& node =
        plan.snapshot.nodes.at(located->second);
    if (!executorMatchesCaller(node, call) ||
        call.request_id != plan.snapshot.request_id ||
        call.trace_id != plan.snapshot.trace_id ||
        !executionIdentityMatches(
            node, acceptance.pid,
            acceptance.activation_id,
            acceptance.execution_id,
            acceptance.operation_id,
            acceptance.attempt_no,
            acceptance.fencing_token) ||
        acceptance.plan_id != plan.snapshot.plan_id) {
        return {false, false, plan.snapshot.version,
                "ORCHESTRATOR_EXECUTION_IDENTITY_MISMATCH"};
    }
    if (node.state == ActivationState::Queued ||
        node.state == ActivationState::Running ||
        nodeTerminal(node.state)) {
        return {true, true, plan.snapshot.version, {}};
    }
    if (node.state != ActivationState::DispatchPending) {
        return {false, false, plan.snapshot.version,
                "ORCHESTRATOR_EXECUTION_STATE_CONFLICT"};
    }
    node.state = acceptance.accepted
                     ? ActivationState::Queued
                     : ActivationState::Failed;
    node.error_code = acceptance.reject_code;
    ++plan.snapshot.version;
    emit(plan, &node,
         acceptance.accepted ? "DISPATCH_ACCEPTED"
                             : "DISPATCH_REJECTED");
    if (!acceptance.accepted) settlePlan(plan);
    return {true, false, plan.snapshot.version, {}};
}

ExecutionEventAck Orchestrator::ackExecutionStarted(
    const ExecutionStarted& started,
    const CallContext& call) {
    if (!executionCallerAllowed(call) || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, 0,
                "ORCHESTRATOR_EXECUTION_CALLER_INVALID"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto located =
        findExecutionUnlocked(started.execution_id);
    if (!located) {
        return {false, false, 0,
                "ORCHESTRATOR_EXECUTION_NOT_FOUND"};
    }
    auto& plan = plans_.at(located->first);
    auto& node =
        plan.snapshot.nodes.at(located->second);
    if (!executorMatchesCaller(node, call) ||
        started.plan_id != plan.snapshot.plan_id ||
        call.request_id != plan.snapshot.request_id ||
        call.trace_id != plan.snapshot.trace_id ||
        !executionIdentityMatches(
            node, started.pid, started.activation_id,
            started.execution_id, started.operation_id,
            started.attempt_no, started.fencing_token)) {
        return {false, false, plan.snapshot.version,
                "ORCHESTRATOR_EXECUTION_IDENTITY_MISMATCH"};
    }
    if (node.state == ActivationState::Running ||
        nodeTerminal(node.state)) {
        return {true, true, plan.snapshot.version, {}};
    }
    if (node.state != ActivationState::Queued) {
        return {false, false, plan.snapshot.version,
                "ORCHESTRATOR_EXECUTION_STATE_CONFLICT"};
    }
    node.state = ActivationState::Running;
    ++plan.snapshot.version;
    emit(plan, &node, "NODE_RUNNING");
    return {true, false, plan.snapshot.version, {}};
}

ExecutionCommitResult Orchestrator::completeExecution(
    const NodeExecutionResult& result,
    const CallContext& call) {
    if (!executionCallerAllowed(call) || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, 0,
                "ORCHESTRATOR_EXECUTION_CALLER_INVALID"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto located =
        findExecutionUnlocked(result.execution_id);
    if (!located) {
        return {false, false, 0,
                "ORCHESTRATOR_EXECUTION_NOT_FOUND"};
    }
    auto& plan = plans_.at(located->first);
    auto& node =
        plan.snapshot.nodes.at(located->second);
    if (!executorMatchesCaller(node, call) ||
        result.plan_id != plan.snapshot.plan_id ||
        call.request_id != plan.snapshot.request_id ||
        call.trace_id != plan.snapshot.trace_id ||
        !executionIdentityMatches(
            node, result.pid, result.activation_id,
            result.execution_id, result.operation_id,
            result.attempt_no, result.fencing_token) ||
        !result.result.is_object() ||
        !isValidSideEffectState(result.side_effect_state)) {
        return {false, false, plan.snapshot.version,
                "ORCHESTRATOR_EXECUTION_IDENTITY_MISMATCH"};
    }
    if (nodeTerminal(node.state)) {
        return {true, true, plan.snapshot.version, {}};
    }
    Observation observation;
    observation.plan_id = located->first;
    observation.node_id = located->second;
    observation.pid = result.pid;
    observation.activation_id = result.activation_id;
    observation.execution_id = result.execution_id;
    observation.operation_id = result.operation_id;
    observation.attempt_count = result.attempt_no;
    observation.fencing_token = result.fencing_token;
    observation.result = result.result;
    observation.error_code = result.error_code;
    observation.side_effect_state =
        result.side_effect_state;
    observation.retryable_hint = result.retryable_hint;
    switch (result.status) {
        case TerminalStatus::Succeeded:
            observation.state = ActivationState::Succeeded;
            break;
        case TerminalStatus::Failed:
            observation.state = ActivationState::Failed;
            break;
        case TerminalStatus::Cancelled:
            observation.state = ActivationState::Cancelled;
            break;
        case TerminalStatus::Unknown:
            observation.state = ActivationState::Unknown;
            break;
        default:
            return {false, false, plan.snapshot.version,
                    "ORCHESTRATOR_EXECUTION_TERMINAL_REQUIRED"};
    }
    applyObservation(observation);
    return {true, false, plan.snapshot.version, {}};
}

SignalCommitResult Orchestrator::publishSignal(
    const SignalEvent& signal, const CallContext& call) {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        signal.event_id.empty() || signal.source.empty() ||
        signal.event_name.empty() ||
        !signal.payload.is_object() ||
        signal.source_epoch == 0 || signal.cursor == 0 ||
        signal.occurred_at_utc_ms <= 0 ||
        signal.signature_ref.empty()) {
        return {false, false, 0,
                "ORCHESTRATOR_SIGNAL_INVALID"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (seen_signal_events_.count(signal.event_id) != 0) {
        return {true, true, 0, {}};
    }
    const auto source_key =
        signal.source + "|" + signal.event_name;
    const auto cursor = signal_cursors_.find(source_key);
    if (cursor != signal_cursors_.end() &&
        (signal.source_epoch < cursor->second.first ||
         (signal.source_epoch == cursor->second.first &&
          signal.cursor <= cursor->second.second))) {
        return {false, false, 0,
                "ORCHESTRATOR_SIGNAL_CURSOR_STALE"};
    }
    seen_signal_events_.insert(signal.event_id);
    signal_cursors_[source_key] =
        {signal.source_epoch, signal.cursor};
    std::size_t activated = 0;
    for (auto& [plan_id, plan] : plans_) {
        (void)plan_id;
        if (planTerminal(plan.snapshot.state) ||
            call.principal_id_hash !=
                plan.admission.principal_id_hash) {
            continue;
        }
        for (auto& [node_id, node] :
             plan.snapshot.nodes) {
            (void)node_id;
            if (node.definition.trigger.type !=
                    TriggerType::Signal ||
                node.definition.trigger.source !=
                    signal.source ||
                node.definition.trigger.expression.value(
                    "event_name", std::string{}) !=
                    signal.event_name) {
                continue;
            }
            if (node.state == ActivationState::Blocked &&
                !node.trigger_satisfied &&
                node.activation_count <
                    node.definition.lifecycle.max_activations) {
                node.trigger_satisfied = true;
                node.activation_id =
                    ids_->next("activation");
                ++node.activation_count;
                node.trigger_instance_id = signal.event_id;
                ++plan.snapshot.version;
                emit(plan, &node, "SIGNAL_TRIGGERED");
                ++activated;
                continue;
            }
            const bool active =
                node.state == ActivationState::Ready ||
                node.state == ActivationState::DispatchPending ||
                node.state == ActivationState::Queued ||
                node.state == ActivationState::Running ||
                node.state == ActivationState::Reconciling;
            const auto remaining =
                node.definition.lifecycle.max_activations >
                        node.activation_count +
                            node.pending_trigger_count
                    ? node.definition.lifecycle.max_activations -
                          node.activation_count -
                          node.pending_trigger_count
                    : 0;
            if (!active || remaining == 0 ||
                node.definition.trigger.repeat_policy ==
                    RepeatPolicy::Once) {
                continue;
            }
            if (node.definition.trigger.overlap_policy ==
                OverlapPolicy::DropIfRunning) {
                ++plan.snapshot.version;
                emit(plan, &node, "SIGNAL_TRIGGER_DROPPED_RUNNING");
            } else if (node.definition.trigger.overlap_policy ==
                       OverlapPolicy::CoalesceLatest) {
                node.pending_trigger_count = 1;
                node.pending_trigger_instance_id = signal.event_id;
                ++plan.snapshot.version;
                emit(plan, &node, "SIGNAL_TRIGGER_COALESCED");
            } else {
                // SERIAL is the production default. ParallelBounded is
                // rejected during admission until a capability explicitly
                // advertises same-PID parallel safety.
                ++node.pending_trigger_count;
                node.pending_trigger_instance_id = signal.event_id;
                ++plan.snapshot.version;
                emit(plan, &node, "SIGNAL_TRIGGER_QUEUED");
            }
        }
        refreshBlockers(plan);
    }
    return {true, false, activated, {}};
}

ControlCommitResult Orchestrator::suspend(
    const PlanControlRequest& request,
    const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = plans_.find(request.plan_id);
    if (found == plans_.end()) {
        return {false, false, 0,
                "ORCHESTRATOR_PLAN_NOT_FOUND"};
    }
    auto& plan = found->second;
    if (request.control_epoch == plan.snapshot.control_epoch &&
        plan.snapshot.state == PlanState::Suspended) {
        return {true, true, plan.snapshot.version, {}};
    }
    const auto valid =
        validateControlRequest(request, call, plan);
    if (!valid.ok || planTerminal(plan.snapshot.state)) {
        return {false, false, plan.snapshot.version,
                valid.ok ? "ORCHESTRATOR_PLAN_TERMINAL"
                         : valid.error.code};
    }
    plan.snapshot.state = PlanState::Suspended;
    plan.snapshot.control_epoch = request.control_epoch;
    ++plan.snapshot.version;
    emit(plan, nullptr, "PLAN_SUSPENDED");
    return {true, false, plan.snapshot.version, {}};
}

ControlCommitResult Orchestrator::resume(
    const PlanControlRequest& request,
    const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = plans_.find(request.plan_id);
    if (found == plans_.end()) {
        return {false, false, 0,
                "ORCHESTRATOR_PLAN_NOT_FOUND"};
    }
    auto& plan = found->second;
    if (request.control_epoch == plan.snapshot.control_epoch &&
        plan.snapshot.state == PlanState::Running) {
        return {true, true, plan.snapshot.version, {}};
    }
    const auto valid =
        validateControlRequest(request, call, plan);
    if (!valid.ok ||
        plan.snapshot.state != PlanState::Suspended) {
        return {
            false, false, plan.snapshot.version,
            valid.ok ? "ORCHESTRATOR_PLAN_NOT_SUSPENDED"
                     : valid.error.code};
    }
    plan.snapshot.state = PlanState::Running;
    plan.snapshot.control_epoch = request.control_epoch;
    ++plan.snapshot.version;
    emit(plan, nullptr, "PLAN_RESUMED");
    refreshBlockers(plan);
    return {true, false, plan.snapshot.version, {}};
}

ControlCommitResult Orchestrator::cancelPlan(
    const PlanCancelRequest& request,
    const CallContext& call) {
    struct Active {
        std::string executor;
        std::string execution_id;
        std::string operation_id;
        TaskPriority priority = TaskPriority::P1;
        std::uint64_t control_epoch = 0;
    };
    std::vector<Active> active;
    std::uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = plans_.find(request.plan_id);
        if (found == plans_.end()) {
            return {false, false, 0,
                    "ORCHESTRATOR_PLAN_NOT_FOUND"};
        }
        auto& plan = found->second;
        if (request.control_epoch ==
                plan.snapshot.control_epoch &&
            plan.snapshot.cancellation_requested) {
            return {true, true, plan.snapshot.version, {}};
        }
        const auto valid =
            validateControlRequest(request, call, plan);
        if (!valid.ok) {
            return {false, false, plan.snapshot.version,
                    valid.error.code};
        }
        if (planTerminal(plan.snapshot.state)) {
            return {false, false, plan.snapshot.version,
                    "ORCHESTRATOR_PLAN_TERMINAL"};
        }
        plan.snapshot.cancellation_requested = true;
        plan.snapshot.control_epoch = request.control_epoch;
        for (auto& [node_id, node] :
             plan.snapshot.nodes) {
            (void)node_id;
            if (node.state == ActivationState::Queued ||
                node.state == ActivationState::Running ||
                node.state ==
                    ActivationState::Reconciling) {
                active.push_back(
                    {node.definition.executor,
                     node.execution_id, node.operation_id,
                     node.effective_priority,
                     request.control_epoch});
            } else if (!nodeTerminal(node.state)) {
                node.state = ActivationState::Cancelled;
                node.error_code =
                    "ORCHESTRATOR_PLAN_CANCELLED";
                ++plan.snapshot.version;
                emit(plan, &node, "NODE_CANCELLED");
            }
        }
        ++plan.snapshot.version;
        emit(plan, nullptr, "PLAN_CANCEL_REQUESTED");
        settlePlan(plan);
        version = plan.snapshot.version;
    }
    auto downstream_call = makeChildCallContext(
        call, CallerModuleId::TaskOrchestrationEngine);
    for (const auto& item : active) {
        if (item.executor == "atomic_service") {
            (void)atomic_->requestPreempt(
                item.execution_id, TaskPriority::P0,
                item.control_epoch, downstream_call);
            continue;
        }
        const auto snapshot = dispatch_->queryDispatch(
            item.operation_id, downstream_call);
        if (snapshot.status.ok && snapshot.value) {
            (void)dispatch_->requestPreempt(
                snapshot.value->dispatch_id,
                TaskPriority::P0, item.control_epoch,
                downstream_call);
        }
    }
    return {true, false, version, {}};
}

ControlCommitResult Orchestrator::cancelActivation(
    const ActivationCancelRequest& request,
    const CallContext& call) {
    std::string executor;
    std::string execution_id;
    std::string operation_id;
    bool active = false;
    std::uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = plans_.find(request.plan_id);
        if (found == plans_.end()) {
            return {false, false, 0,
                    "ORCHESTRATOR_PLAN_NOT_FOUND"};
        }
        auto& plan = found->second;
        const auto valid =
            validateControlRequest(request, call, plan);
        if (!valid.ok) {
            return {false, false, plan.snapshot.version,
                    valid.error.code};
        }
        auto node = std::find_if(
            plan.snapshot.nodes.begin(),
            plan.snapshot.nodes.end(),
            [&request](const auto& item) {
                return item.second.pid == request.pid &&
                       item.second.activation_id ==
                           request.activation_id;
            });
        if (node == plan.snapshot.nodes.end()) {
            return {false, false, plan.snapshot.version,
                    "ORCHESTRATOR_ACTIVATION_NOT_FOUND"};
        }
        if (nodeTerminal(node->second.state)) {
            return {true, true, plan.snapshot.version, {}};
        }
        plan.snapshot.control_epoch = request.control_epoch;
        executor = node->second.definition.executor;
        execution_id = node->second.execution_id;
        operation_id = node->second.operation_id;
        active =
            node->second.state == ActivationState::Queued ||
            node->second.state == ActivationState::Running ||
            node->second.state ==
                ActivationState::Reconciling;
        if (!active) {
            node->second.state = ActivationState::Cancelled;
            node->second.error_code =
                "ORCHESTRATOR_ACTIVATION_CANCELLED";
        }
        ++plan.snapshot.version;
        emit(plan, &node->second,
             active ? "ACTIVATION_CANCEL_REQUESTED"
                    : "NODE_CANCELLED");
        settlePlan(plan);
        version = plan.snapshot.version;
    }
    if (active) {
        auto downstream_call = makeChildCallContext(
            call, CallerModuleId::TaskOrchestrationEngine);
        if (executor == "atomic_service") {
            (void)atomic_->requestPreempt(
                execution_id, TaskPriority::P0,
                request.control_epoch, downstream_call);
        } else {
            const auto snapshot = dispatch_->queryDispatch(
                operation_id, downstream_call);
            if (snapshot.status.ok && snapshot.value) {
                (void)dispatch_->requestPreempt(
                    snapshot.value->dispatch_id,
                    TaskPriority::P0,
                    request.control_epoch,
                    downstream_call);
            }
        }
    }
    return {true, false, version, {}};
}

CompensationCommitResult Orchestrator::rollback(
    const RollbackRequest& request,
    const CallContext& call) {
    PlanRuntime original;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = plans_.find(request.plan_id);
        if (found == plans_.end()) {
            return {false, false, 0,
                    "ORCHESTRATOR_PLAN_NOT_FOUND", {}};
        }
        const auto valid =
            validateControlRequest(request, call, found->second);
        if (!valid.ok || !planTerminal(found->second.snapshot.state) ||
            request.idempotency_key.empty()) {
            return {
                false, false, found->second.snapshot.version,
                valid.ok
                    ? "ORCHESTRATOR_ROLLBACK_REQUEST_INVALID"
                    : valid.error.code,
                {}};
        }
        original = found->second;
    }

    IntentDAG compensation;
    compensation.dag_id = ids_->next("rollback-dag");
    compensation.request_id = original.snapshot.request_id;
    compensation.priority = original.snapshot.summary_priority;
    compensation.deadline_mono_ns = request.deadline_mono_ns;
    compensation.schema_version = 2;
    compensation.idempotency_key = request.idempotency_key;
    std::string previous;
    std::vector<const NodeRuntimeSnapshot*> completed;
    for (const auto& [node_id, node] :
         original.snapshot.nodes) {
        (void)node_id;
        if (node.state == ActivationState::Succeeded &&
            node.definition.compensation.enabled) {
            completed.push_back(&node);
        }
    }
    std::reverse(completed.begin(), completed.end());
    for (const auto* source : completed) {
        DAGNode node;
        node.node_id =
            "COMP_" + source->definition.node_id;
        node.executor =
            source->definition.compensation.executor;
        node.action =
            source->definition.compensation.action;
        node.target_agent =
            source->definition.compensation.target_agent;
        node.params =
            source->definition.compensation.params;
        node.base_priority =
            source->definition.base_priority;
        node.deadline_mono_ns =
            request.deadline_mono_ns;
        node.input_schema_version = 1;
        node.expected_output_schema_version = 1;
        if (!previous.empty()) {
            node.dependencies.push_back(previous);
        }
        for (const auto& binding :
             source->definition.compensation
                 .result_bindings) {
            const auto result_source =
                original.snapshot.nodes.find(
                    binding.source_node_id);
            const auto* value =
                result_source ==
                        original.snapshot.nodes.end()
                    ? nullptr
                    : readBindingPath(
                          result_source->second.result,
                          binding.source_field_path);
            nlohmann::json selected;
            if (value) {
                selected = *value;
            } else if (
                binding.on_missing == "USE_DEFAULT") {
                selected = binding.default_value;
            } else {
                return {
                    false, false,
                    original.snapshot.version,
                    "ORCHESTRATOR_COMPENSATION_BINDING_FAILED",
                    {}};
            }
            if (!writeBindingPath(
                    &node.params, binding.target_param,
                    selected)) {
                return {
                    false, false,
                    original.snapshot.version,
                    "ORCHESTRATOR_COMPENSATION_BINDING_FAILED",
                    {}};
            }
        }
        previous = node.node_id;
        compensation.nodes.push_back(std::move(node));
    }
    if (compensation.nodes.empty()) {
        return {false, false, original.snapshot.version,
                "ORCHESTRATOR_NO_COMPENSATION_DECLARED", {}};
    }

    auto admission = original.admission;
    admission.deadline_mono_ns = request.deadline_mono_ns;
    OrchestratorSubmitRequest submit_request;
    submit_request.dag = std::move(compensation);
    submit_request.admission = std::move(admission);
    submit_request.idempotency_key =
        request.idempotency_key;
    submit_request.expected_capability_digest =
        original.catalog.catalog_digest;
    submit_request.trace_id = original.snapshot.trace_id;
    submit_request.submitted_at_utc_ms =
        clock_->utcNowMs();
    const auto committed = submit(submit_request, call);
    if (!committed.accepted) {
        return {false, false, original.snapshot.version,
                committed.reject_code, {}};
    }
    std::uint64_t original_version = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& parent = plans_.at(request.plan_id);
        parent.snapshot.control_epoch =
            request.control_epoch;
        ++parent.snapshot.version;
        emit(parent, nullptr,
             "ROLLBACK_PLAN_COMMITTED");
        original_version = parent.snapshot.version;
        auto& child = plans_.at(committed.plan_id);
        child.snapshot.parent_plan_id = request.plan_id;
        ++child.snapshot.version;
        emit(child, nullptr,
             "COMPENSATION_PLAN_LINKED");
    }
    return {true, committed.existing, original_version,
            {}, committed.plan_id};
}

Result<NodeRuntimeSnapshot> Orchestrator::getNodeStatus(
    const std::string& plan_id,
    const std::string& pid_or_node_id,
    const CallContext& call) const {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) {
        return Result<NodeRuntimeSnapshot>::Failure(caller);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto plan = plans_.find(plan_id);
    if (!clock_ || plan == plans_.end() ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<NodeRuntimeSnapshot>::Failure(
            Status::Error(
                "orchestrator",
                plan == plans_.end()
                    ? "ORCHESTRATOR_PLAN_NOT_FOUND"
                    : "ORCHESTRATOR_QUERY_EXPIRED",
                "node query cannot be served"));
    }
    if (call.request_id != plan->second.snapshot.request_id ||
        call.trace_id != plan->second.snapshot.trace_id ||
        call.principal_id_hash !=
            plan->second.admission.principal_id_hash) {
        return Result<NodeRuntimeSnapshot>::Failure(
            Status::Error(
                "orchestrator",
                "ORCHESTRATOR_QUERY_IDENTITY_MISMATCH",
                "query identity does not own the plan"));
    }
    const auto by_id =
        plan->second.snapshot.nodes.find(pid_or_node_id);
    if (by_id != plan->second.snapshot.nodes.end()) {
        return Result<NodeRuntimeSnapshot>::Success(
            by_id->second);
    }
    const auto by_pid = std::find_if(
        plan->second.snapshot.nodes.begin(),
        plan->second.snapshot.nodes.end(),
        [&pid_or_node_id](const auto& item) {
            return item.second.pid == pid_or_node_id;
        });
    if (by_pid == plan->second.snapshot.nodes.end()) {
        return Result<NodeRuntimeSnapshot>::Failure(
            Status::Error(
                "orchestrator",
                "ORCHESTRATOR_NODE_NOT_FOUND",
                "node or PID was not found"));
    }
    return Result<NodeRuntimeSnapshot>::Success(
        by_pid->second);
}

Result<TaskEventPage> Orchestrator::subscribeEvents(
    const EventSubscriptionRequest& request,
    const CallContext& call) const {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) {
        return Result<TaskEventPage>::Failure(caller);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        request.consumer_id.empty() ||
        request.consumer_epoch == 0 ||
        request.max_events == 0 ||
        request.max_events > 1024 ||
        request.cursor > events_.size()) {
        return Result<TaskEventPage>::Failure(
            Status::Error(
                "orchestrator",
                "ORCHESTRATOR_EVENT_SUBSCRIPTION_INVALID",
                "event cursor, epoch, or deadline is invalid"));
    }
    const auto acknowledged =
        event_acks_.find(request.consumer_id);
    if (acknowledged != event_acks_.end() &&
        request.consumer_epoch <
            acknowledged->second.first) {
        return Result<TaskEventPage>::Failure(
            Status::Error(
                "orchestrator",
                "ORCHESTRATOR_CONSUMER_EPOCH_STALE",
                "event consumer epoch is stale"));
    }
    TaskEventPage page;
    std::size_t index =
        static_cast<std::size_t>(request.cursor);
    for (; index < events_.size() &&
           page.events.size() < request.max_events;
         ++index) {
        const auto& event = events_[index];
        if (request.plan_id &&
            event.plan_id != *request.plan_id) {
            continue;
        }
        const auto plan = plans_.find(event.plan_id);
        if (plan == plans_.end() ||
            call.principal_id_hash !=
                plan->second.admission.principal_id_hash) {
            continue;
        }
        page.events.push_back(event);
    }
    page.next_cursor = index;
    page.has_more = index < events_.size();
    return Result<TaskEventPage>::Success(
        std::move(page));
}

Status Orchestrator::ackEvents(
    const EventAckRequest& request,
    const CallContext& call) {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) return caller;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        request.consumer_id.empty() ||
        request.consumer_epoch == 0 ||
        request.cursor > events_.size()) {
        return Status::Error(
            "orchestrator",
            "ORCHESTRATOR_EVENT_ACK_INVALID",
            "event acknowledgement is invalid");
    }
    auto& current = event_acks_[request.consumer_id];
    if (request.consumer_epoch < current.first ||
        (request.consumer_epoch == current.first &&
         request.cursor < current.second)) {
        return Status::Error(
            "orchestrator",
            "ORCHESTRATOR_EVENT_ACK_STALE",
            "event acknowledgement is not monotonic");
    }
    current =
        {request.consumer_epoch, request.cursor};
    if (!storage_directory_.empty() && !plans_.empty()) {
        const auto persisted =
            persistPlanUnlocked(plans_.begin()->second);
        if (!persisted.ok) return persisted;
    }
    return Status::Ok();
}

OrchestratorHealth Orchestrator::health(
    HealthDetailLevel detail,
    const CallContext& call) const {
    (void)detail;
    OrchestratorHealth health;
    const auto caller = validateAgentServiceCaller(call);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!caller.ok || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        health.status_code =
            "ORCHESTRATOR_HEALTH_CALL_INVALID";
        return health;
    }
    health.durable = durability_status_.ok;
    health.scheduler_available =
        atomic_ != nullptr && dispatch_ != nullptr;
    health.plan_count = plans_.size();
    for (const auto& [plan_id, plan] : plans_) {
        (void)plan_id;
        for (const auto& [node_id, node] :
             plan.snapshot.nodes) {
            (void)node_id;
            if (node.state == ActivationState::Ready) {
                ++health.ready_count;
            } else if (
                node.state == ActivationState::Queued ||
                node.state == ActivationState::Running ||
                node.state ==
                    ActivationState::Reconciling) {
                ++health.running_count;
            } else if (
                node.state == ActivationState::Blocked &&
                !node.trigger_satisfied) {
                if (node.definition.trigger.type ==
                    TriggerType::Timer) {
                    ++health.timer_wait_count;
                } else if (
                    node.definition.trigger.type ==
                    TriggerType::Signal) {
                    ++health.signal_wait_count;
                }
            }
        }
    }
    std::uint64_t minimum_ack = events_.size();
    for (const auto& [consumer, ack] : event_acks_) {
        (void)consumer;
        minimum_ack = std::min(
            minimum_ack, ack.second);
    }
    health.event_backlog =
        events_.size() -
        static_cast<std::size_t>(minimum_ack);
    health.healthy =
        health.durable && health.scheduler_available;
    health.status_code =
        health.healthy ? "READY"
                       : "DEGRADED";
    return health;
}

}  // namespace master_agent::orchestrator
