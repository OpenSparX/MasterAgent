/**
 * @file scheduler.cpp
 * @brief Runs ready task activations and terminal-plan pumping.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {

bool Orchestrator::pumpOne() {

    std::lock_guard<std::mutex> pump_lock(pump_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!durability_status_.ok) return false;
    }
    bool progressed = atomic_->pumpOne();
    progressed = dispatch_->pumpOne() || progressed;

    struct RunningRef {
        std::string plan_id;
        std::string node_id;
        std::string pid;
        std::string activation_id;
        std::string executor;
        std::string execution_id;
        std::string operation_id;
        std::uint32_t attempt_count = 0;
        std::uint64_t fencing_token = 0;
        std::string request_id;
        std::string trace_id;
        std::string principal_id_hash;
        TaskPriority priority = TaskPriority::P1;
        std::int64_t deadline_mono_ns = 0;
        std::string authorization_ref;
    };
    std::vector<RunningRef> active;
    std::vector<RunningRef> reconciling;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& plan_pair : plans_) {
            auto& plan = plan_pair.second;
            if (!planTerminal(plan.snapshot.state)) {
                const bool plan_deadline_expired = deadlineExpired(
                    plan.snapshot.effective_deadline_mono_ns, *clock_);
                for (auto& node_pair : plan.snapshot.nodes) {
                    auto& node = node_pair.second;
                    const bool node_deadline_expired =
                        deadlineExpired(
                            node.definition.deadline_mono_ns, *clock_);
                    if ((plan_deadline_expired ||
                         node_deadline_expired) &&
                        (node.state == ActivationState::Blocked ||
                         node.state == ActivationState::Ready ||
                         node.state ==
                             ActivationState::DispatchPending)) {
                        node.state = ActivationState::Failed;
                        node.error_code =
                            "ORCHESTRATOR_DEADLINE_EXPIRED";
                        ++plan.snapshot.version;
                        emit(plan, &node, "NODE_TERMINAL");
                        progressed = true;
                    }
                }
                settlePlan(plan);
            }
            for (const auto& node_pair : plan.snapshot.nodes) {
                const auto& node = node_pair.second;
                if (node.state == ActivationState::Queued ||
                    node.state == ActivationState::Running) {
                    active.push_back(
                        {plan_pair.first,
                         node_pair.first,
                         node.pid,
                         node.activation_id,
                         node.definition.executor,
                         node.execution_id,
                         node.operation_id,
                         node.attempt_count,
                         node.fencing_token,
                         plan.snapshot.request_id,
                         plan.snapshot.trace_id,
                         plan.admission.principal_id_hash,
                         node.effective_priority,
                         node.definition.deadline_mono_ns,
                         plan.admission.authorization_ref});
                } else if (node.state ==
                           ActivationState::Reconciling) {
                    reconciling.push_back(
                        {plan_pair.first,
                         node_pair.first,
                         node.pid,
                         node.activation_id,
                         node.definition.executor,
                         node.execution_id,
                         node.operation_id,
                         node.attempt_count,
                         node.fencing_token,
                         plan.snapshot.request_id,
                         plan.snapshot.trace_id,
                         plan.admission.principal_id_hash,
                         node.effective_priority,
                         node.definition.deadline_mono_ns,
                         plan.admission.authorization_ref});
                }
            }
        }
    }

    std::vector<Observation> observations;

    for (const auto& ref : active) {
        const auto observation_deadline =
            clock_->monotonicNowNs() + 1'000'000'000LL;
        CallContext query_call{
            CallerModuleId::TaskOrchestrationEngine,
            ref.request_id, ref.trace_id,
            ref.principal_id_hash, ref.priority,
            observation_deadline, {}, 0, ref.authorization_ref};
        if (ref.executor == "atomic_service") {
            const auto queried =
                atomic_->queryExecution(ref.execution_id, query_call);
            if (!queried.status.ok || !queried.value) continue;
            const auto& runtime = queried.value->envelope.runtime;
            if (runtime.pid != ref.pid ||
                runtime.activation_id != ref.activation_id ||
                runtime.execution_id != ref.execution_id ||
                runtime.operation_id != ref.operation_id ||
                runtime.attempt_no != ref.attempt_count ||
                runtime.fencing_token != ref.fencing_token) {
                continue;
            }
            Observation observation;
            observation.plan_id = ref.plan_id;
            observation.node_id = ref.node_id;
            observation.pid = ref.pid;
            observation.activation_id = ref.activation_id;
            observation.execution_id = ref.execution_id;
            observation.operation_id = ref.operation_id;
            observation.attempt_count = ref.attempt_count;
            observation.fencing_token = ref.fencing_token;
            observation.result =
                queried.value->result
                    ? queried.value->result->structured_content
                    : nlohmann::json::object();
            observation.error_code = queried.value->error_code;
            observation.side_effect_state =
                queried.value->side_effect_state;
            observation.retryable_hint =
                queried.value->retryable_hint;
            switch (queried.value->state) {
                case atomic_service::AtomicExecutionState::Accepted:
                case atomic_service::AtomicExecutionState::Queued:
                    continue;
                case atomic_service::AtomicExecutionState::Running:
                    observation.state = ActivationState::Running;
                    break;
                case atomic_service::AtomicExecutionState::Succeeded:
                    observation.state = ActivationState::Succeeded;
                    break;
                case atomic_service::AtomicExecutionState::Failed:
                    observation.state = ActivationState::Failed;
                    break;
                case atomic_service::AtomicExecutionState::Cancelled:
                    observation.state = ActivationState::Cancelled;
                    break;
                case atomic_service::AtomicExecutionState::Unknown:
                    observation.state = ActivationState::Unknown;
                    break;
                default:
                    continue;
            }
            observations.push_back(std::move(observation));
        } else {
            const auto queried =
                dispatch_->queryDispatch(ref.operation_id, query_call);
            if (!queried.status.ok || !queried.value) continue;
            const auto& task = queried.value->task;
            if (task.pid != ref.pid ||
                task.activation_id != ref.activation_id ||
                task.execution_id != ref.execution_id ||
                task.operation_id != ref.operation_id ||
                task.attempt_no != ref.attempt_count ||
                task.fencing_token != ref.fencing_token) {
                continue;
            }
            Observation observation;
            observation.plan_id = ref.plan_id;
            observation.node_id = ref.node_id;
            observation.pid = ref.pid;
            observation.activation_id = ref.activation_id;
            observation.execution_id = ref.execution_id;
            observation.operation_id = ref.operation_id;
            observation.attempt_count = ref.attempt_count;
            observation.fencing_token = ref.fencing_token;
            observation.result = queried.value->result;
            observation.error_code = queried.value->error_code;
            observation.side_effect_state =
                queried.value->side_effect_state;
            observation.retryable_hint =
                queried.value->retryable_hint;
            switch (queried.value->state) {
                case agent_dispatch::DispatchState::Accepted:
                case agent_dispatch::DispatchState::Queued:
                    continue;
                case agent_dispatch::DispatchState::Running:
                    observation.state = ActivationState::Running;
                    break;
                case agent_dispatch::DispatchState::Succeeded:
                    observation.state = ActivationState::Succeeded;
                    break;
                case agent_dispatch::DispatchState::Failed:
                    observation.state = ActivationState::Failed;
                    break;
                case agent_dispatch::DispatchState::Cancelled:
                    observation.state = ActivationState::Cancelled;
                    break;
                case agent_dispatch::DispatchState::Unknown:
                    observation.state = ActivationState::Unknown;
                    break;
                default:
                    continue;
            }
            observations.push_back(std::move(observation));
        }
    }
    if (!observations.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& observation : observations) {
            applyObservation(observation);
        }
        progressed = true;
    }

    std::vector<Observation> reconciled_observations;
    for (const auto& ref : reconciling) {
        const auto control_deadline =
            clock_->monotonicNowNs() + 1'000'000'000LL;
        CallContext reconcile_call{
            CallerModuleId::TaskOrchestrationEngine,
            ref.request_id, ref.trace_id,
            ref.principal_id_hash, ref.priority,
            control_deadline, {}, 0, ref.authorization_ref};
        if (ref.executor == "agent_dispatch") {
            auto settled = dispatch_->queryDispatch(
                ref.operation_id, reconcile_call);
            if (!settled.status.ok || !settled.value) continue;
            const auto initial_route = settled.value->route;
            const auto task_matches =
                [&ref](const agent_dispatch::DispatchSnapshot& value) {
                    return value.task.pid == ref.pid &&
                           value.task.activation_id ==
                               ref.activation_id &&
                           value.task.execution_id ==
                               ref.execution_id &&
                           value.task.operation_id ==
                               ref.operation_id &&
                           value.task.attempt_no ==
                               ref.attempt_count &&
                           value.task.fencing_token ==
                               ref.fencing_token;
                };
            if (!task_matches(*settled.value) ||
                initial_route.agent_epoch == 0 ||
                initial_route.lease_id.empty()) {
                continue;
            }
            if (settled.value->state ==
                agent_dispatch::DispatchState::Unknown) {
                settled = dispatch_->reconcileDispatch(
                    ref.operation_id, initial_route.agent_epoch,
                    reconcile_call);
                if (!settled.status.ok || !settled.value) continue;
                if (!task_matches(*settled.value) ||
                    settled.value->route.agent_id !=
                        initial_route.agent_id ||
                    settled.value->route.agent_epoch !=
                        initial_route.agent_epoch ||
                    settled.value->route.lease_id !=
                        initial_route.lease_id) {
                    continue;
                }
                progressed = true;
            }

            Observation observation;
            observation.plan_id = ref.plan_id;
            observation.node_id = ref.node_id;
            observation.pid = ref.pid;
            observation.activation_id = ref.activation_id;
            observation.execution_id = ref.execution_id;
            observation.operation_id = ref.operation_id;
            observation.attempt_count = ref.attempt_count;
            observation.fencing_token = ref.fencing_token;
            observation.result = settled.value->result;
            observation.error_code =
                settled.value->error_code;
            observation.side_effect_state =
                settled.value->side_effect_state;
            observation.retryable_hint =
                settled.value->retryable_hint;
            if (settled.value->state ==
                agent_dispatch::DispatchState::Succeeded) {
                observation.state = ActivationState::Succeeded;
            } else if (settled.value->state ==
                       agent_dispatch::DispatchState::Failed) {
                observation.state = ActivationState::Failed;
            } else if (settled.value->state ==
                       agent_dispatch::DispatchState::Cancelled) {
                observation.state = ActivationState::Cancelled;
            } else if (settled.value->state ==
                       agent_dispatch::DispatchState::Running) {
                observation.state = ActivationState::Running;
            } else {
                continue;
            }
            reconciled_observations.push_back(
                std::move(observation));
            continue;
        }
        if (ref.executor != "atomic_service") continue;
        const auto reconciled =
            atomic_->reconcileExecution(ref.operation_id, reconcile_call);
        if (!reconciled.status.ok || !reconciled.value) continue;
        if (reconciled.value->operation_id != ref.operation_id ||
            reconciled.value->execution_id != ref.execution_id ||
            reconciled.value->fencing_token != ref.fencing_token) {
            continue;
        }
        if (reconciled.value->status ==
            atomic_service::ReconcileStatus::StillUnknown) {
            continue;
        }
        progressed = true;
        const auto queried =
            atomic_->queryExecution(ref.execution_id, reconcile_call);
        if (!queried.status.ok || !queried.value) continue;
        const auto& runtime = queried.value->envelope.runtime;
        if (runtime.pid != ref.pid ||
            runtime.activation_id != ref.activation_id ||
            runtime.execution_id != ref.execution_id ||
            runtime.operation_id != ref.operation_id ||
            runtime.attempt_no != ref.attempt_count ||
            runtime.fencing_token != ref.fencing_token) {
            continue;
        }
        Observation observation;
        observation.plan_id = ref.plan_id;
        observation.node_id = ref.node_id;
        observation.pid = ref.pid;
        observation.activation_id = ref.activation_id;
        observation.execution_id = ref.execution_id;
        observation.operation_id = ref.operation_id;
        observation.attempt_count = ref.attempt_count;
        observation.fencing_token = ref.fencing_token;
        observation.result =
            queried.value->result
                ? queried.value->result->structured_content
                : nlohmann::json::object();
        observation.error_code = queried.value->error_code;
        observation.side_effect_state =
            queried.value->side_effect_state;
        observation.retryable_hint =
            queried.value->retryable_hint;
        if (queried.value->state ==
            atomic_service::AtomicExecutionState::Succeeded) {
            observation.state = ActivationState::Succeeded;
        } else if (queried.value->state ==
                   atomic_service::AtomicExecutionState::Failed) {
            observation.state = ActivationState::Failed;
        } else {
            continue;
        }
        reconciled_observations.push_back(std::move(observation));
    }
    if (!reconciled_observations.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& observation : reconciled_observations) {
            applyObservation(observation);
        }
        progressed = true;
    }

    std::optional<std::pair<std::string, std::string>> ready;
    NodeRuntimeSnapshot local_node;
    AdmissionContext admission;
    atomic_service::McpToolCatalogSnapshot catalog;
    std::string trace_id;
    std::string request_id;
    std::uint64_t dispatch_generation = 0;
    bool binding_failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : plans_) {
            refreshBlockers(pair.second);
            if (!planTerminal(pair.second.snapshot.state)) {
                const bool plan_deadline_expired = deadlineExpired(
                    pair.second.snapshot.effective_deadline_mono_ns,
                    *clock_);
                for (auto& node_pair :
                     pair.second.snapshot.nodes) {
                    auto& node = node_pair.second;
                    if (node.state == ActivationState::Ready &&
                        (plan_deadline_expired ||
                         deadlineExpired(
                             node.definition.deadline_mono_ns,
                             *clock_))) {
                        node.state = ActivationState::Failed;
                        node.error_code =
                            "ORCHESTRATOR_DEADLINE_EXPIRED";
                        ++pair.second.snapshot.version;
                        emit(pair.second, &node, "NODE_TERMINAL");
                        progressed = true;
                    }
                }
            }
            settlePlan(pair.second);
        }
        ready = selectReady();
        if (ready) {
            auto& plan = plans_.at(ready->first);
            auto& node = plan.snapshot.nodes.at(ready->second);
            nlohmann::json bound_params;
            std::string binding_error;
            if (!bindResultsUnlocked(
                    plan, node, &bound_params,
                    &binding_error)) {
                node.state =
                    binding_error ==
                            "ORCHESTRATOR_BINDING_SKIP_PATH"
                        ? ActivationState::Skipped
                        : ActivationState::Failed;
                node.error_code = binding_error;
                ++plan.snapshot.version;
                emit(
                    plan, &node,
                    node.state == ActivationState::Skipped
                        ? "NODE_SKIPPED"
                        : "NODE_TERMINAL");
                refreshBlockers(plan);
                settlePlan(plan);
                binding_failed = true;
                ready.reset();
            }
            if (!binding_failed) {
                node.bound_params = std::move(bound_params);
            node.state = ActivationState::DispatchPending;
            node.result = nlohmann::json::object();
            node.error_code.clear();
            node.side_effect_state = SideEffectState::NotStarted;
            node.retryable_hint = false;
            node.retry_at_mono_ns = 0;
            node.execution_id = ids_->next("execution");
            node.operation_id = ids_->next("operation");

            node.fencing_token = ++fencing_sequence_;
            ++node.attempt_count;
            ++plan.snapshot.version;
            local_node = node;
            admission = plan.admission;
            catalog = plan.catalog;
            trace_id = plan.snapshot.trace_id;
            request_id = plan.snapshot.request_id;
            emit(plan, &node, "DISPATCH_PENDING");
            dispatch_generation = plan.snapshot.version;
            }
        }
    }
    if (binding_failed) return true;
    if (!ready) return progressed;

    bool accepted = false;
    std::string reject_code;
    CallContext execution_call{
        CallerModuleId::TaskOrchestrationEngine,
        request_id, trace_id,
        admission.principal_id_hash, local_node.effective_priority,
        local_node.definition.deadline_mono_ns, {}, 0,
        admission.authorization_ref};
    if (local_node.definition.executor == "atomic_service") {
        atomic_service::AtomicMcpCallEnvelope envelope;
        envelope.mcp_request.id = local_node.operation_id;
        envelope.mcp_request.name = local_node.definition.action;
        envelope.mcp_request.arguments = local_node.bound_params;
        envelope.runtime.caller_module_id =
            CallerModuleId::TaskOrchestrationEngine;
        envelope.runtime.request_id = execution_call.request_id;
        envelope.runtime.trace_id = trace_id;
        envelope.runtime.plan_id = ready->first;
        envelope.runtime.pid = local_node.pid;
        envelope.runtime.activation_id = local_node.activation_id;
        envelope.runtime.execution_id = local_node.execution_id;
        envelope.runtime.attempt_no = local_node.attempt_count;
        envelope.runtime.operation_id = local_node.operation_id;
        envelope.runtime.priority = local_node.effective_priority;
        envelope.runtime.deadline_mono_ns =
            local_node.definition.deadline_mono_ns;
        envelope.runtime.idempotency_key =
            ready->first + "|" + local_node.activation_id + "|" +
            std::to_string(local_node.attempt_count);
        envelope.runtime.fencing_token = local_node.fencing_token;
        envelope.runtime.tool_catalog_snapshot_id = catalog.snapshot_id;
        envelope.runtime.tool_digest =
            catalog.tool_digests.at(local_node.definition.action);
        envelope.runtime.policy_digest =
            catalog.policy_digests.at(local_node.definition.action);
        envelope.runtime.granted_permissions.assign(
            admission.granted_permissions.begin(),
            admission.granted_permissions.end());
        // The reference runtime has no platform ResourceCoordinator. Do not disguise
        // resource requirement names as acquired lease references; Atomic
        // still enforces its internal resource/fencing ledger.
        envelope.runtime.resource_lease_refs.clear();
        envelope.runtime.principal_id_hash =
            admission.principal_id_hash;
        envelope.runtime.authorization_ref =
            admission.authorization_ref;
        const auto result = atomic_->callTool(envelope, execution_call);
        accepted = result.accepted;
        reject_code = result.reject_code;
    } else {
        agent_dispatch::DispatchTask task;
        task.request_id = execution_call.request_id;
        task.plan_id = ready->first;
        task.pid = local_node.pid;
        task.activation_id = local_node.activation_id;
        task.execution_id = local_node.execution_id;
        task.attempt_no = local_node.attempt_count;
        task.operation_id = local_node.operation_id;
        task.task_id = local_node.execution_id;
        task.action = local_node.definition.action;
        task.target_agent = local_node.definition.target_agent;
        task.allow_agent_fallback =
            local_node.definition.allow_agent_fallback;
        task.params = local_node.bound_params;
        task.input_schema_version =
            local_node.definition.input_schema_version;
        task.expected_output_schema_version =
            local_node.definition.expected_output_schema_version;
        task.priority = local_node.effective_priority;
        task.deadline_mono_ns =
            local_node.definition.deadline_mono_ns;
        task.idempotency_key =
            ready->first + "|" + local_node.activation_id + "|" +
            std::to_string(local_node.attempt_count);
        task.fencing_token = local_node.fencing_token;
        task.capability_digest =
            agent_dispatch::dispatchCapabilityDigest(
                task.action, task.input_schema_version,
                task.expected_output_schema_version);
        task.capacity_epoch =
            dispatch_->getCapacity(execution_call).capacity_epoch;
        task.resource_lease_refs.clear();
        task.granted_permissions.assign(
            admission.granted_permissions.begin(),
            admission.granted_permissions.end());
        task.allowed_child_capabilities.assign(
            admission.allowed_capabilities.begin(),
            admission.allowed_capabilities.end());
        task.principal_id_hash = admission.principal_id_hash;
        task.authorization_ref = admission.authorization_ref;
        task.trace_id = trace_id;
        const auto result = dispatch_->submitDispatch(task, execution_call);
        accepted = result.accepted;
        reject_code = result.reject_code;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto plan_found = plans_.find(ready->first);
        if (plan_found == plans_.end()) return true;
        auto& plan = plan_found->second;
        auto node_found = plan.snapshot.nodes.find(ready->second);
        if (node_found == plan.snapshot.nodes.end() ||
            node_found->second.state !=
                ActivationState::DispatchPending ||
            node_found->second.execution_id !=
                local_node.execution_id ||
            node_found->second.operation_id !=
                local_node.operation_id ||
            plan.snapshot.version != dispatch_generation) {
            return true;
        }
        auto& node = node_found->second;
        if (accepted) {
            // DispatchAcceptance only proves that the executor owns the
            // ticket.  RUNNING is entered later from an authenticated
            // downstream STARTED/Running observation.
            node.state = ActivationState::Queued;
            ++plan.snapshot.version;
            emit(plan, &node, "DISPATCH_ACCEPTED");
        } else if (
            reject_code == "DISPATCH_CAPACITY_EPOCH_STALE" ||
            reject_code == "NO_AGENT_CAPACITY") {
            // Capacity snapshots are admission facts, not business
            // attempts. A topology race or short AgentLease contention is
            // re-evaluated without consuming retry budget or retaining an
            // unowned execution identity.
            node.state = ActivationState::Ready;
            node.execution_id.clear();
            node.operation_id.clear();
            node.fencing_token = 0;
            node.result.clear();
            node.error_code.clear();
            node.side_effect_state =
                SideEffectState::NotStarted;
            if (node.attempt_count > 0) {
                --node.attempt_count;
            }
            plan.node_ready_sequence[ready->second] =
                ++ready_sequence_;
            node.error_code = reject_code;
            ++plan.snapshot.version;
            emit(plan, &node, "DISPATCH_REEVALUATE");
        } else {
            node.state = ActivationState::Failed;
            node.error_code = reject_code;
            ++plan.snapshot.version;
            emit(plan, &node, "DISPATCH_REJECTED");
            settlePlan(plan);
        }
    }
    return true;
}

Status Orchestrator::runUntilPlanTerminal(
    const std::string& plan_id, std::size_t max_steps) {
    for (std::size_t i = 0; i < max_steps; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = plans_.find(plan_id);
            if (found == plans_.end()) {
                return Status::Error("orchestrator",
                                     "ORCHESTRATOR_PLAN_NOT_FOUND",
                                     "plan was not found");
            }
            if (planTerminal(found->second.snapshot.state)) {
                return Status::Ok();
            }
        }
        if (!pumpOne()) {
            // pumpOne() may settle a plan while finding no further runnable
            // activation.  Re-read the authoritative state before reporting a
            // stall so a just-emitted PLAN_TERMINAL is not misclassified.
            std::lock_guard<std::mutex> lock(mutex_);
            if (!durability_status_.ok) {
                return durability_status_;
            }
            const auto found = plans_.find(plan_id);
            if (found == plans_.end()) {
                return Status::Error("orchestrator",
                                     "ORCHESTRATOR_PLAN_NOT_FOUND",
                                     "plan was not found");
            }
            if (planTerminal(found->second.snapshot.state)) {
                return Status::Ok();
            }
            return Status::Error("orchestrator",
                                 "ORCHESTRATOR_STALLED",
                                 "plan is non-terminal without progress");
        }
    }
    return Status::Error("orchestrator", "ORCHESTRATOR_PUMP_LIMIT",
                         "plan did not terminate within step limit");
}

std::vector<TaskEvent> Orchestrator::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}


}  // namespace master_agent::orchestrator
