/**
 * @file orchestrator_impl.cpp
 * @brief Implements plan submission and read-only plan queries.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {

PlanCommitResult Orchestrator::submit(
    const OrchestratorSubmitRequest& request, const CallContext& call) {

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!durability_status_.ok) {
            return {false, false, {}, {}, TaskPriority::P2, 0,
                    durability_status_.error.code};
        }
    }

    if (!request.idempotency_key.empty() &&
        !request.admission.principal_id_hash.empty() &&
        hasHostModuleIdentity(call, CallerModuleId::AgentService) &&
        call.request_id == request.dag.request_id &&
        call.trace_id == request.trace_id &&
        call.principal_id_hash ==
            request.admission.principal_id_hash &&
        call.priority == request.admission.granted_priority &&
        call.authorization_ref ==
            request.admission.authorization_ref &&
        call.deadline_mono_ns > 0 &&
        clock_ &&
        !deadlineExpired(call.deadline_mono_ns, *clock_)) {
        try {
            const auto durable_digest = submitDigest(request);
            const auto durable_ledger =
                scopedIdempotencyLedgerKey(
                    request.admission.principal_id_hash,
                    request.idempotency_key);
            std::lock_guard<std::mutex> lock(mutex_);
            const auto replay =
                idempotency_to_plan_.find(durable_ledger);
            if (replay != idempotency_to_plan_.end()) {
                if (idempotency_digest_.at(durable_ledger) !=
                    durable_digest) {
                    return {false, false, {}, {},
                            TaskPriority::P2, 0,
                            "ORCHESTRATOR_IDEMPOTENCY_CONFLICT"};
                }
                const auto& existing =
                    plans_.at(replay->second).snapshot;
                PlanCommitResult result;
                result.accepted = true;
                result.existing = true;
                result.plan_id = existing.plan_id;
                result.summary_priority =
                    existing.summary_priority;
                result.committed_at_utc_ms =
                    clock_->utcNowMs();
                for (const auto& pair : existing.nodes) {
                    result.node_id_to_pid[pair.first] =
                        pair.second.pid;
                }
                return result;
            }
        } catch (...) {
            return {false, false, {}, {}, TaskPriority::P2, 0,
                    "ORCHESTRATOR_REQUEST_ENCODING_INVALID"};
        }
    }
    if (!clock_ || !ids_ || !atomic_ || !dispatch_ ||
        !hasHostModuleIdentity(
            call, CallerModuleId::AgentService) ||
        request.trace_id.empty() ||
        request.trace_id != call.trace_id ||
        request.dag.request_id.empty() ||
        request.dag.request_id != call.request_id ||
        request.admission.principal_id_hash.empty() ||
        request.admission.principal_id_hash !=
            call.principal_id_hash ||
        call.authorization_ref !=
            request.admission.authorization_ref ||
        call.priority !=
            request.admission.granted_priority ||
        call.deadline_mono_ns !=
            request.admission.deadline_mono_ns ||
        !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                "ORCHESTRATOR_CALL_IDENTITY_MISMATCH"};
    }
    if (request.idempotency_key.empty() ||
        request.idempotency_key != request.dag.idempotency_key) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                "ORCHESTRATOR_IDEMPOTENCY_KEY_INVALID"};
    }
    if (request.expected_capability_digest.empty()) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                "ORCHESTRATOR_CAPABILITY_DIGEST_REQUIRED"};
    }
    const auto validation =
        validateDAG(request.dag, request.admission, call);
    if (!validation.valid) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                validation.reject_code};
    }
    const auto digest = submitDigest(request);
    const auto ledger_key = scopedIdempotencyLedgerKey(
        request.admission.principal_id_hash,
        request.idempotency_key);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto replay = idempotency_to_plan_.find(ledger_key);
        if (replay != idempotency_to_plan_.end()) {
            if (idempotency_digest_.at(ledger_key) != digest) {
                return {false, false, {}, {}, TaskPriority::P2, 0,
                        "ORCHESTRATOR_IDEMPOTENCY_CONFLICT"};
            }
            const auto& existing =
                plans_.at(replay->second).snapshot;
            PlanCommitResult result;
            result.accepted = true;
            result.existing = true;
            result.plan_id = existing.plan_id;
            result.summary_priority = existing.summary_priority;
            result.committed_at_utc_ms = clock_->utcNowMs();
            for (const auto& pair : existing.nodes) {
                result.node_id_to_pid[pair.first] =
                    pair.second.pid;
            }
            return result;
        }
    }
    const auto normalized_dag = normalizeEdges(request.dag);
    auto atomic_call = makeChildCallContext(
        call, CallerModuleId::TaskOrchestrationEngine);
    const auto catalog =
        atomic_->getToolCatalogSnapshot(atomic_call);
    if (!catalog.status.ok || !catalog.value) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                catalog.status.error.code};
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                "ORCHESTRATOR_CATALOG_RESULT_AFTER_DEADLINE"};
    }
    if (request.expected_capability_digest !=
        catalog.value->catalog_digest) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                "ORCHESTRATOR_CAPABILITY_DIGEST_MISMATCH"};
    }
    std::set<std::string> atomic_names;
    for (const auto& tool : catalog.value->tools) {
        atomic_names.insert(tool.name);
    }
    for (const auto& node : normalized_dag.nodes) {
        if (node.executor == "atomic_service" &&
            atomic_names.count(node.action) == 0) {
            return {false, false, {}, {}, TaskPriority::P2, 0,
                    "ORCHESTRATOR_ATOMIC_TOOL_NOT_FOUND"};
        }
        if (node.executor == "atomic_service" &&
            node.max_attempts > 1) {
            const auto trusted =
                catalog.value->idempotency_policies.find(
                    node.action);
            const auto admitted =
                request.admission.retry_policies.find(
                    node.action);
            const auto trusted_errors =
                catalog.value->retryable_errors.find(
                    node.action);
            if (trusted ==
                    catalog.value->idempotency_policies.end() ||
                admitted ==
                    request.admission.retry_policies.end() ||
                trusted_errors ==
                    catalog.value->retryable_errors.end() ||
                trusted->second !=
                    admitted->second.idempotency_policy ||
                std::set<std::string>(
                    trusted_errors->second.begin(),
                    trusted_errors->second.end()) !=
                    admitted->second.retryable_errors) {
                return {
                    false, false, {}, {}, TaskPriority::P2, 0,
                    "ORCHESTRATOR_RETRY_POLICY_CAPABILITY_MISMATCH"};
            }
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto replay = idempotency_to_plan_.find(ledger_key);
    if (replay != idempotency_to_plan_.end()) {
        if (idempotency_digest_.at(ledger_key) != digest) {
            return {false, false, {}, {}, TaskPriority::P2, 0,
                    "ORCHESTRATOR_IDEMPOTENCY_CONFLICT"};
        }
        const auto& existing = plans_.at(replay->second).snapshot;
        PlanCommitResult result;
        result.accepted = true;
        result.existing = true;
        result.plan_id = existing.plan_id;
        result.summary_priority = existing.summary_priority;
        result.committed_at_utc_ms = clock_->utcNowMs();
        for (const auto& pair : existing.nodes) {
            result.node_id_to_pid[pair.first] = pair.second.pid;
        }
        return result;
    }

    PlanRuntime runtime;
    runtime.snapshot.plan_id = ids_->next("plan");
    runtime.snapshot.request_id = normalized_dag.request_id;
    runtime.snapshot.state = PlanState::Committed;
    runtime.snapshot.summary_priority =
        highestPriority(normalized_dag.nodes);
    runtime.snapshot.active_priority = runtime.snapshot.summary_priority;
    runtime.snapshot.effective_deadline_mono_ns =
        normalized_dag.deadline_mono_ns;
    runtime.snapshot.capability_snapshot_id = catalog.value->snapshot_id;
    runtime.snapshot.policy_snapshot_id =
        request.admission.policy_snapshot_id;
    runtime.snapshot.trace_id = request.trace_id;
    runtime.admission = request.admission;
    runtime.catalog = *catalog.value;
    runtime.submit_digest = digest;
    runtime.ledger_key = ledger_key;
    runtime.edges = normalized_dag.edges;
    const auto effective_priorities =
        inheritedPriorities(normalized_dag);
    for (const auto& definition : normalized_dag.nodes) {
        NodeRuntimeSnapshot node;
        node.definition = definition;
        node.pid = ids_->next("pid");
        node.trigger_satisfied =
            definition.trigger.type == TriggerType::Immediate;
        if (node.trigger_satisfied) {
            node.activation_id = ids_->next("activation");
            node.activation_count = 1;
            node.trigger_instance_id =
                ids_->next("trigger-immediate");
        }
        node.next_fire_at_utc_ms =
            definition.trigger.next_fire_at_utc_ms;
        node.bound_params = definition.params;
        node.state =
            definition.dependencies.empty() &&
                    node.trigger_satisfied
                ? ActivationState::Ready
                : ActivationState::Blocked;
        if (!node.trigger_satisfied) {
            node.blockers.push_back(
                definition.trigger.type == TriggerType::Timer
                    ? "TRIGGER_TIMER"
                    : "TRIGGER_SIGNAL");
        }
        for (const auto& dependency :
             definition.dependencies) {
            node.blockers.push_back(
                "DEPENDENCY:" + dependency);
        }
        node.effective_priority =
            effective_priorities.at(definition.node_id);
        runtime.snapshot.nodes[definition.node_id] = std::move(node);
        if (definition.dependencies.empty() &&
            definition.trigger.type == TriggerType::Immediate) {
            runtime.node_ready_sequence[definition.node_id] =
                ++ready_sequence_;
        }
    }
    runtime.snapshot.state = PlanState::Running;
    const auto plan_id = runtime.snapshot.plan_id;
    plans_[plan_id] = std::move(runtime);
    idempotency_to_plan_[ledger_key] = plan_id;
    idempotency_digest_[ledger_key] = digest;
    emit(plans_.at(plan_id), nullptr, "PLAN_COMMITTED");
    if (!durability_status_.ok) {
        return {false, false, {}, {}, TaskPriority::P2, 0,
                durability_status_.error.code};
    }
    PlanCommitResult result;
    result.accepted = true;
    result.plan_id = plan_id;
    result.summary_priority = plans_.at(plan_id).snapshot.summary_priority;
    result.committed_at_utc_ms = clock_->utcNowMs();
    for (const auto& pair : plans_.at(plan_id).snapshot.nodes) {
        result.node_id_to_pid[pair.first] = pair.second.pid;
    }
    return result;
}

Result<TaskPlanSnapshot> Orchestrator::getPlan(
    const std::string& plan_id, const CallContext& call) const {
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) return Result<TaskPlanSnapshot>::Failure(caller);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<TaskPlanSnapshot>::Failure(Status::Error(
            "orchestrator", "ORCHESTRATOR_QUERY_EXPIRED",
            "plan query requires a live control deadline"));
    }
    const auto found = plans_.find(plan_id);
    if (found == plans_.end()) {
        return Result<TaskPlanSnapshot>::Failure(Status::Error(
            "orchestrator", "ORCHESTRATOR_PLAN_NOT_FOUND",
            "plan was not found"));
    }
    if (call.request_id != found->second.snapshot.request_id ||
        call.trace_id != found->second.snapshot.trace_id ||
        call.principal_id_hash !=
            found->second.admission.principal_id_hash) {
        return Result<TaskPlanSnapshot>::Failure(Status::Error(
            "orchestrator", "ORCHESTRATOR_QUERY_IDENTITY_MISMATCH",
            "query identity does not own the plan"));
    }
    return Result<TaskPlanSnapshot>::Success(found->second.snapshot);
}

// pump_mutex_ preserves single-actor scheduler semantics while mutex_ remains
// available to concurrent submit and query calls. External module calls occur
// from frozen activation references and are validated before state publication.

}  // namespace master_agent::orchestrator
