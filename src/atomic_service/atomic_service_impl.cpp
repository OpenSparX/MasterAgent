/**
 * @file atomic_service_impl.cpp
 * @brief Admits tool calls and implements atomic-service public operations.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

DispatchAcceptance AtomicServiceManager::callTool(
    const AtomicMcpCallEnvelope& request, const CallContext& call) {

    if (call.caller == CallerModuleId::SubAgent) {
        if (!lineage_validator_) {
            return {false, false,
                    request.runtime.operation_id,
                    request.runtime.execution_id,
                    "ATOMIC_PARENT_LINEAGE_VALIDATOR_UNAVAILABLE"};
        }
        Status lineage = Status::Error(
            "atomic_service",
            "ATOMIC_PARENT_LINEAGE_VALIDATION_FAILED",
            "parent lineage validation failed");
        try {
            lineage =
                lineage_validator_->
                    validateAtomicParentLineage(request, call);
        } catch (...) {
            return {false, false,
                    request.runtime.operation_id,
                    request.runtime.execution_id,
                    "ATOMIC_PARENT_LINEAGE_VALIDATOR_EXCEPTION"};
        }
        if (!lineage.ok) {
            return {false, false,
                    request.runtime.operation_id,
                    request.runtime.execution_id,
                    lineage.error.code};
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!durability_status_.ok) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id,
                durability_status_.error.code};
    }
    if (!request.runtime.principal_id_hash.empty() &&
        !request.runtime.idempotency_key.empty()) {
        const auto replay_ledger_key =
            scopedIdempotencyLedgerKey(
                request.runtime.principal_id_hash,
                request.runtime.idempotency_key);
        const auto recovered_replay =
            idempotency_to_execution_.find(replay_ledger_key);
        if (recovered_replay !=
            idempotency_to_execution_.end()) {
            const ToolRecord* replay_record = nullptr;
            const auto replay_validation = validateCall(
                request, call, &replay_record, true);
            if (!replay_validation.ok) {
                return {false, false,
                        request.runtime.operation_id,
                        request.runtime.execution_id,
                        replay_validation.error.code};
            }
            std::string replay_digest;
            try {
                replay_digest = callDigest(request);
            } catch (...) {
                return {false, false,
                        request.runtime.operation_id,
                        request.runtime.execution_id,
                        "ATOMIC_REQUEST_ENCODING_INVALID"};
            }
            if (idempotency_digest_.at(replay_ledger_key) !=
                replay_digest) {
                return {false, false,
                        request.runtime.operation_id,
                        request.runtime.execution_id,
                        "ATOMIC_IDEMPOTENCY_CONFLICT"};
            }
            const auto& existing =
                executions_.at(recovered_replay->second);
            return {true, true,
                    existing.envelope.runtime.operation_id,
                    existing.envelope.runtime.execution_id, {}};
        }
    }
    const ToolRecord* record = nullptr;
    const auto validation = validateCall(request, call, &record);
    if (!validation.ok) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id, validation.error.code};
    }
    std::string digest;
    try {
        digest = callDigest(request);
    } catch (...) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id,
                "ATOMIC_REQUEST_ENCODING_INVALID"};
    }
    const auto ledger_key = scopedIdempotencyLedgerKey(
        request.runtime.principal_id_hash,
        request.runtime.idempotency_key);
    const auto replay =
        idempotency_to_execution_.find(ledger_key);
    if (replay != idempotency_to_execution_.end()) {
        if (idempotency_digest_.at(ledger_key) != digest) {
            return {false, false, request.runtime.operation_id,
                    request.runtime.execution_id,
                    "ATOMIC_IDEMPOTENCY_CONFLICT"};
        }
        const auto& existing = executions_.at(replay->second);
        return {true, true, existing.envelope.runtime.operation_id,
                existing.envelope.runtime.execution_id, {}};
    }
    if (executions_.count(request.runtime.execution_id) != 0) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id,
                "ATOMIC_EXECUTION_ID_CONFLICT"};
    }
    if (operation_to_execution_.count(request.runtime.operation_id) != 0) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id,
                "ATOMIC_OPERATION_ID_CONFLICT"};
    }
    std::string resource;
    try {
        resource = resourceKey(
            record->policy,
            request.mcp_request.arguments);
    } catch (...) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id,
                "ATOMIC_REQUEST_ENCODING_INVALID"};
    }
    const auto fencing = highest_fencing_by_resource_.find(resource);
    const bool sub_agent_owner =
        request.runtime.caller_module_id ==
        CallerModuleId::SubAgent;
    const bool same_fence_owner =
        fencing != highest_fencing_by_resource_.end() &&
        sub_agent_owner &&
        fencing->second.parent_dispatch_id ==
            request.runtime.parent_dispatch_id &&
        fencing->second.parent_lease_id ==
            request.runtime.parent_lease_id;
    if (fencing != highest_fencing_by_resource_.end()) {
        if (request.runtime.fencing_token <
                fencing->second.fencing_token ||
            (request.runtime.fencing_token ==
                 fencing->second.fencing_token &&
             !same_fence_owner)) {
            return {false, false,
                    request.runtime.operation_id,
                    request.runtime.execution_id,
                    "ATOMIC_STALE_FENCING_TOKEN"};
        }
    }
    if (fencing == highest_fencing_by_resource_.end() ||
        request.runtime.fencing_token >
            fencing->second.fencing_token) {
        highest_fencing_by_resource_[resource] = {
            request.runtime.fencing_token,
            sub_agent_owner
                ? request.runtime.parent_dispatch_id
                : std::string{},
            sub_agent_owner
                ? request.runtime.parent_lease_id
                : std::string{}};
    }

    // A newly granted fence invalidates work that has not crossed the
    // provider side-effect boundary.  Running provider calls are settled by
    // the generation/fence check in pumpOne and never silently committed.
    for (auto& existing : executions_) {
        auto& prior = existing.second;
        if (prior.resource_key != resource ||
            prior.envelope.runtime.fencing_token >=
                request.runtime.fencing_token ||
            (prior.state != AtomicExecutionState::Queued &&
             prior.state != AtomicExecutionState::Suspended &&
             prior.state != AtomicExecutionState::Running) ||
            provider_inflight_.count(existing.first) != 0) {
            continue;
        }
        if (prior.state == AtomicExecutionState::Running) {
            // No Provider call is in flight (guarded above), therefore this
            // is an explicit local safe point: no physical side effect has
            // crossed the ProviderAdapter boundary.
            emit(prior, "PREEMPT_ACCEPTED");
            emit(prior, "SAFE_POINT_REACHED", true, true);
        }
        prior.state = AtomicExecutionState::Failed;
        prior.side_effect_state = SideEffectState::NotStarted;
        prior.error_code = "ATOMIC_STALE_FENCING_TOKEN";
        emit(prior, "FAILED", false, true);
    }

    AtomicExecutionSnapshot snapshot;
    snapshot.envelope = request;
    snapshot.state = AtomicExecutionState::Queued;
    snapshot.resource_key = resource;
    snapshot.remaining_work_units =
        std::max<std::uint32_t>(1, record->policy.simulated_work_units);
    executions_[request.runtime.execution_id] = snapshot;
    execution_tools_[request.runtime.execution_id] = *record;
    operation_to_execution_[request.runtime.operation_id] =
        request.runtime.execution_id;
    idempotency_to_execution_[ledger_key] =
        request.runtime.execution_id;
    idempotency_digest_[ledger_key] = digest;
    queue_sequence_[request.runtime.execution_id] = ++enqueue_sequence_;
    emit(executions_.at(request.runtime.execution_id), "ACCEPTED");
    emit(executions_.at(request.runtime.execution_id), "QUEUED");
    if (!durability_status_.ok) {
        return {false, false, request.runtime.operation_id,
                request.runtime.execution_id,
                durability_status_.error.code};
    }
    return {true, false, request.runtime.operation_id,
            request.runtime.execution_id, {}};
}

Result<AtomicExecutionSnapshot> AtomicServiceManager::queryExecution(
    const std::string& execution_or_operation_id,
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!canQuery(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) ||
        !clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<AtomicExecutionSnapshot>::Failure(Status::Error(
            "atomic_service", "ATOMIC_QUERY_CALLER_NOT_ALLOWED",
            "caller may not query atomic execution"));
    }
    auto execution_id = execution_or_operation_id;
    const auto operation =
        operation_to_execution_.find(execution_or_operation_id);
    if (operation != operation_to_execution_.end()) {
        execution_id = operation->second;
    }
    const auto found = executions_.find(execution_id);
    if (found == executions_.end()) {
        return Result<AtomicExecutionSnapshot>::Failure(Status::Error(
            "atomic_service", "ATOMIC_EXECUTION_NOT_FOUND",
            "atomic execution was not found"));
    }
    const auto& runtime = found->second.envelope.runtime;
    if (call.request_id != runtime.request_id ||
        call.trace_id != runtime.trace_id ||
        call.principal_id_hash != runtime.principal_id_hash) {
        return Result<AtomicExecutionSnapshot>::Failure(Status::Error(
            "atomic_service", "ATOMIC_QUERY_IDENTITY_MISMATCH",
            "query identity does not own the atomic execution"));
    }
    return Result<AtomicExecutionSnapshot>::Success(found->second);
}

Status AtomicServiceManager::requestPreempt(
    const std::string& execution_id, TaskPriority arriving_priority,
    std::uint64_t control_epoch, const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!durability_status_.ok) return durability_status_;
    if (!canControl(call.caller) ||
        !hasHostModuleIdentity(call, call.caller)) {
        return Status::Error("atomic_service",
                             "ATOMIC_PREEMPT_CALLER_NOT_ALLOWED",
                             "caller may not preempt atomic execution");
    }
    if (!clock_ || control_epoch == 0 ||
        !isValidTaskPriority(arriving_priority) ||
        !isValidTaskPriority(call.priority) ||
        call.priority != arriving_priority ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        (arriving_priority == TaskPriority::P0 &&
         call.authorization_ref.rfind(
             "trusted-safety:", 0) != 0)) {
        return Status::Error(
            "atomic_service", "ATOMIC_PREEMPT_CALL_INVALID",
            "preemption control identity, priority or deadline is invalid");
    }
    auto found = executions_.find(execution_id);
    if (found == executions_.end()) {
        return Status::Error("atomic_service", "ATOMIC_EXECUTION_NOT_FOUND",
                             "atomic execution was not found");
    }
    auto& snapshot = found->second;
    if (control_epoch < snapshot.control_epoch) {
        return Status::Error("atomic_service",
                             "ATOMIC_CONTROL_EPOCH_STALE",
                             "control epoch is stale");
    }
    if (control_epoch == snapshot.control_epoch && control_epoch != 0) {
        return Status::Ok();
    }
    const auto tool = execution_tools_.find(execution_id);
    if (snapshot.state != AtomicExecutionState::Running ||
        provider_inflight_.count(execution_id) != 0 ||
        tool == execution_tools_.end() ||
        !tool->second.policy.supports_preemption ||
        !isHigherPriority(arriving_priority,
                          snapshot.envelope.runtime.priority)) {
        return Status::Error("atomic_service",
                             "ATOMIC_PREEMPT_NOT_APPLICABLE",
                             "execution cannot be preempted");
    }
    snapshot.control_epoch = control_epoch;
    emit(snapshot, "PREEMPT_ACCEPTED");
    emit(snapshot, "SAFE_POINT_REACHED", true);
    snapshot.state = AtomicExecutionState::Suspended;
    emit(snapshot, "SUSPENDED", true, true);
    return durability_status_;
}

Result<AtomicReconcileResult>
AtomicServiceManager::reconcileExecution(
    const std::string& operation_id, const CallContext& call) {

    if (!clock_ || operation_id.empty() ||
        !canControl(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) ||
        !isValidTaskPriority(call.priority) ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<AtomicReconcileResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_RECONCILE_CALL_INVALID",
            "reconcile identity or absolute deadline is invalid"));
    }
    AtomicMcpCallEnvelope envelope;
    AtomicProviderInvocationSeal invocation_seal;
    std::shared_ptr<IAtomicProvider> provider;
    std::string execution_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!durability_status_.ok) {
            return Result<AtomicReconcileResult>::Failure(
                durability_status_);
        }
        if (!canControl(call.caller)) {
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_RECONCILE_CALLER_NOT_ALLOWED",
                "caller may not reconcile atomic execution"));
        }
        const auto operation =
            operation_to_execution_.find(operation_id);
        if (operation == operation_to_execution_.end()) {
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_EXECUTION_NOT_FOUND",
                "operation was not found"));
        }
        execution_id = operation->second;
        auto& snapshot = executions_.at(execution_id);
        if (snapshot.state != AtomicExecutionState::Unknown) {
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_RECONCILE_NOT_REQUIRED",
                "execution is not UNKNOWN"));
        }
        const auto& record = execution_tools_.at(execution_id);
        if (!record.policy.supports_reconcile) {
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_RECONCILE_UNSUPPORTED",
                "provider cannot reconcile"));
        }
        if (!snapshot.provider_invocation) {
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_INVOCATION_SEAL_MISSING",
                "UNKNOWN execution has no sealed Provider invocation",
                false, SideEffectState::Unknown));
        }
        if (!reconcile_inflight_.insert(execution_id).second) {
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_RECONCILE_IN_PROGRESS",
                "reconciliation is already in progress", true));
        }
        envelope = snapshot.envelope;
        invocation_seal = *snapshot.provider_invocation;
        provider = record.provider;
    }

    AtomicReconcileResult result;
    try {
        result = provider->reconcile(envelope, invocation_seal);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        reconcile_inflight_.erase(execution_id);
        return Result<AtomicReconcileResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_RECONCILE_PROVIDER_EXCEPTION",
            "provider threw during reconciliation", true,
            SideEffectState::Unknown));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    reconcile_inflight_.erase(execution_id);
    auto& snapshot = executions_.at(execution_id);
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<AtomicReconcileResult>::Failure(Status::Error(
            "atomic_service",
            "ATOMIC_RECONCILE_RESULT_AFTER_DEADLINE",
            "late reconciliation evidence cannot settle UNKNOWN",
            false, SideEffectState::Unknown));
    }
    if (snapshot.state != AtomicExecutionState::Unknown) {
        return Result<AtomicReconcileResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_RECONCILE_STATE_CHANGED",
            "execution changed while reconciliation was in flight", true));
    }
    if (!invocationSealMatches(result.invocation_seal,
                               invocation_seal) ||
        result.operation_id != envelope.runtime.operation_id ||
        result.execution_id != envelope.runtime.execution_id ||
        result.tool_name != envelope.mcp_request.name ||
        result.fencing_token != envelope.runtime.fencing_token ||
        (result.status == ReconcileStatus::ConfirmedSuccess &&
         (!result.call_tool_result ||
          (result.side_effect_state != SideEffectState::Committed &&
           result.side_effect_state !=
               SideEffectState::NotApplicable))) ||
        (result.status ==
             ReconcileStatus::ConfirmedNotExecuted &&
         result.side_effect_state !=
             SideEffectState::ConfirmedNotExecuted) ||
        (result.status == ReconcileStatus::ConfirmedFailure &&
         (result.side_effect_state == SideEffectState::Unknown ||
          result.side_effect_state == SideEffectState::NotStarted ||
          result.side_effect_state ==
              SideEffectState::ConfirmedNotExecuted))) {
        return Result<AtomicReconcileResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_RECONCILE_IDENTITY_MISMATCH",
            "provider reconciliation result is not bound to execution",
            false, SideEffectState::Unknown));
    }
    if (result.status == ReconcileStatus::ConfirmedSuccess) {
        const auto& execution_tool = execution_tools_.at(execution_id);
        const auto& output_schema = execution_tool.definition.output_schema;
        const auto output_validation =
            output_schema.empty()
                ? Status::Ok()
                : validateArguments(
                      output_schema,
                      result.call_tool_result->structured_content);
        if (!output_validation.ok ||
            result.call_tool_result->is_error) {
            snapshot.error_code =
                "ATOMIC_RECONCILE_OUTPUT_SCHEMA_INVALID";
            emit(snapshot, "RECONCILE_REJECTED", false, false);
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service",
                "ATOMIC_RECONCILE_OUTPUT_SCHEMA_INVALID",
                "reconcile result violates the frozen MCP output schema",
                false, SideEffectState::Unknown));
        }
        if (!satisfiesCompletionPolicy(
                execution_tool.policy.completion_policy,
                result.completion_evidence)) {
            snapshot.error_code =
                "ATOMIC_RECONCILE_COMPLETION_EVIDENCE_MISMATCH";
            snapshot.completion_evidence = result.completion_evidence;
            emit(snapshot, "RECONCILE_REJECTED", false, false);
            return Result<AtomicReconcileResult>::Failure(Status::Error(
                "atomic_service",
                "ATOMIC_RECONCILE_COMPLETION_EVIDENCE_MISMATCH",
                "reconcile result does not satisfy the frozen completion policy",
                false, SideEffectState::Unknown));
        }
        snapshot.state = AtomicExecutionState::Succeeded;
        snapshot.side_effect_state = result.side_effect_state;
        snapshot.completion_evidence = result.completion_evidence;
        snapshot.result = result.call_tool_result;
        snapshot.error_code.clear();
        snapshot.retryable_hint = false;
        emit(snapshot, "RECONCILE_SETTLED", false, true);
    } else if (result.status == ReconcileStatus::ConfirmedNotExecuted) {
        snapshot.state = AtomicExecutionState::Failed;
        snapshot.side_effect_state =
            SideEffectState::ConfirmedNotExecuted;
        snapshot.error_code = "CONFIRMED_NOT_EXECUTED";
        snapshot.retryable_hint = result.retryable_hint;
        emit(snapshot, "RECONCILE_SETTLED", false, true);
    } else if (result.status == ReconcileStatus::ConfirmedFailure) {
        snapshot.state = AtomicExecutionState::Failed;
        snapshot.side_effect_state = result.side_effect_state;
        snapshot.error_code = "CONFIRMED_FAILURE";
        snapshot.retryable_hint = result.retryable_hint;
        emit(snapshot, "RECONCILE_SETTLED", false, true);
    }
    if (!durability_status_.ok) {
        return Result<AtomicReconcileResult>::Failure(
            durability_status_);
    }
    return Result<AtomicReconcileResult>::Success(std::move(result));
}

// Provider work is prepared under mutex_, invoked without the state lock, and
// published only after the invocation seal is revalidated. Ambiguous callbacks
// become Unknown and are reconciled; they are never retried as if not executed.

}  // namespace master_agent::atomic_service
