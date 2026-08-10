/**
 * @file query_reconcile.cpp
 * @brief Queries dispatches and reconciles child execution observations.
 */

#include "include/dispatch_access_control.h"
#include "include/provider_identity.h"

namespace master_agent::agent_dispatch {

Result<DispatchSnapshot> AgentDispatch::queryDispatch(
    const std::string& dispatch_or_operation_id,
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!canQuery(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) ||
        !clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_QUERY_CALLER_NOT_ALLOWED",
            "caller may not query dispatch"));
    }
    auto id = dispatch_or_operation_id;
    const auto operation =
        operation_to_dispatch_.find(dispatch_or_operation_id);
    if (operation != operation_to_dispatch_.end()) id = operation->second;
    const auto found = dispatches_.find(id);
    if (found == dispatches_.end()) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_NOT_FOUND",
            "dispatch was not found"));
    }
    if (call.request_id != found->second.task.request_id ||
        call.trace_id != found->second.task.trace_id ||
        call.principal_id_hash !=
            found->second.task.principal_id_hash) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_QUERY_IDENTITY_MISMATCH",
            "query identity does not own the dispatch"));
    }
    return Result<DispatchSnapshot>::Success(found->second);
}

Result<DispatchSnapshot> AgentDispatch::reconcileDispatch(
    const std::string& operation_id,
    std::uint64_t expected_agent_epoch,
    const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::TaskOrchestrationEngine)) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_RECONCILE_CALLER_NOT_ALLOWED",
            "only Orchestrator may reconcile a dispatch"));
    }
    if (!clock_ || operation_id.empty() ||
        expected_agent_epoch == 0 || call.request_id.empty() ||
        call.trace_id.empty() || call.principal_id_hash.empty() ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_RECONCILE_CALL_INVALID",
            "reconcile identity, epoch or deadline is invalid"));
    }

    std::string dispatch_id;
    DispatchSnapshot captured;
    std::shared_ptr<sub_agents::ISubAgent> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto operation =
            operation_to_dispatch_.find(operation_id);
        if (operation == operation_to_dispatch_.end()) {
            return Result<DispatchSnapshot>::Failure(Status::Error(
                "agent_dispatch", "DISPATCH_NOT_FOUND",
                "dispatch operation was not found"));
        }
        dispatch_id = operation->second;
        const auto found = dispatches_.find(dispatch_id);
        if (found == dispatches_.end()) {
            return Result<DispatchSnapshot>::Failure(Status::Error(
                "agent_dispatch", "DISPATCH_NOT_FOUND",
                "dispatch was not found"));
        }
        if (found->second.route.agent_epoch !=
            expected_agent_epoch) {
            return Result<DispatchSnapshot>::Failure(Status::Error(
                "agent_dispatch",
                "DISPATCH_RECONCILE_AGENT_EPOCH_MISMATCH",
                "expected Agent epoch does not own this dispatch",
                false, SideEffectState::Unknown));
        }
        if (found->second.state == DispatchState::Succeeded ||
            found->second.state == DispatchState::Failed ||
            found->second.state == DispatchState::Cancelled) {
            return Result<DispatchSnapshot>::Success(found->second);
        }
        if (found->second.state != DispatchState::Unknown) {
            return Result<DispatchSnapshot>::Failure(Status::Error(
                "agent_dispatch",
                "DISPATCH_RECONCILE_NOT_APPLICABLE",
                "only UNKNOWN dispatches require reconciliation"));
        }
        if (!reconcile_inflight_.insert(dispatch_id).second) {
            return Result<DispatchSnapshot>::Failure(Status::Error(
                "agent_dispatch",
                "DISPATCH_RECONCILE_IN_PROGRESS",
                "reconciliation is already in progress", true,
                SideEffectState::Unknown));
        }
        captured = found->second;
        provider = dispatch_providers_.at(dispatch_id);
    }

    CallContext provider_call{
        CallerModuleId::AgentDispatch, captured.task.request_id,
        captured.task.trace_id, captured.task.principal_id_hash,
        captured.task.priority, call.deadline_mono_ns, {}, 0,
        captured.task.authorization_ref};
    auto observed =
        Result<sub_agents::SubAgentSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_RECONCILE_PROVIDER_EXCEPTION",
            "provider did not return reconciliation evidence", true,
            SideEffectState::Unknown));
    try {
        observed = provider->query(dispatch_id, provider_call);
    } catch (...) {
        // Preserve UNKNOWN and its lease: query failure is not evidence that
        // the physical execution stopped.
    }
    if (!observed.status.ok || !observed.value) {
        std::lock_guard<std::mutex> lock(mutex_);
        reconcile_inflight_.erase(dispatch_id);
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_RECONCILE_INCONCLUSIVE",
            "provider state remains unconfirmed", true,
            SideEffectState::Unknown));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    reconcile_inflight_.erase(dispatch_id);
    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch",
            "DISPATCH_RECONCILE_RESULT_AFTER_DEADLINE",
            "late reconciliation evidence cannot settle UNKNOWN",
            false, SideEffectState::Unknown));
    }
    auto found = dispatches_.find(dispatch_id);
    if (found == dispatches_.end() ||
        found->second.state != DispatchState::Unknown ||
        found->second.route.agent_epoch !=
            captured.route.agent_epoch ||
        found->second.route.lease_id != captured.route.lease_id ||
        found->second.task.fencing_token !=
            captured.task.fencing_token) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_RECONCILE_OWNERSHIP_CHANGED",
            "dispatch ownership changed during reconciliation",
            true, SideEffectState::Unknown));
    }
    auto& live = found->second;
    if (!providerIdentityMatches(*observed.value, live)) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch",
            "DISPATCH_PROVIDER_IDENTITY_MISMATCH",
            "reconcile evidence is not bound to the current lease",
            false, SideEffectState::Unknown));
    }
    if ((observed.value->state ==
             sub_agents::SubAgentState::Succeeded ||
         observed.value->state ==
             sub_agents::SubAgentState::Failed ||
         observed.value->state ==
             sub_agents::SubAgentState::Cancelled) &&
        !observed.value->resource_released) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch",
            "DISPATCH_RECONCILE_RELEASE_UNCONFIRMED",
            "terminal provider evidence did not prove resource release",
            false, SideEffectState::Unknown));
    }
    const bool reconciled_success_confirmed =
        observed.value->side_effect_state ==
            SideEffectState::Committed ||
        observed.value->side_effect_state ==
            SideEffectState::NotApplicable;
    if ((observed.value->state ==
             sub_agents::SubAgentState::Succeeded &&
         !reconciled_success_confirmed) ||
        ((observed.value->state ==
              sub_agents::SubAgentState::Failed ||
          observed.value->state ==
              sub_agents::SubAgentState::Cancelled) &&
         observed.value->side_effect_state ==
             SideEffectState::Unknown)) {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch",
            "DISPATCH_RECONCILE_SIDE_EFFECT_UNCONFIRMED",
            "provider terminal evidence did not settle side effects",
            false, SideEffectState::Unknown));
    }

    if (observed.value->state ==
        sub_agents::SubAgentState::Succeeded) {
        provider_submitted_.erase(dispatch_id);
        setStateUnlocked(live, DispatchState::Succeeded);
        live.result = observed.value->result;
        live.side_effect_state =
            observed.value->side_effect_state;
        live.error_code.clear();
        live.retryable_hint = false;
        emit(live, "RECONCILED_SUCCEEDED", false, true);
    } else if (observed.value->state ==
               sub_agents::SubAgentState::Failed) {
        provider_submitted_.erase(dispatch_id);
        setStateUnlocked(live, DispatchState::Failed);
        live.error_code = observed.value->error_code;
        live.side_effect_state =
            observed.value->side_effect_state;
        live.retryable_hint =
            observed.value->retryable_hint;
        emit(live, "RECONCILED_FAILED", false, true);
    } else if (observed.value->state ==
               sub_agents::SubAgentState::Cancelled) {
        provider_submitted_.erase(dispatch_id);
        setStateUnlocked(live, DispatchState::Cancelled);
        live.side_effect_state =
            observed.value->side_effect_state;
        live.retryable_hint = false;
        emit(live, "RECONCILED_CANCELLED", false, true);
    } else if (observed.value->state ==
                   sub_agents::SubAgentState::Suspended &&
               !observed.value->checkpoint_ref.empty() &&
               observed.value->resource_released) {
        provider_submitted_.erase(dispatch_id);
        setStateUnlocked(live, DispatchState::Suspended);
        live.checkpoint_ref = observed.value->checkpoint_ref;
        live.side_effect_state =
            observed.value->side_effect_state;
        live.error_code.clear();
        live.retryable_hint = false;
        emit(live, "RECONCILED_SUSPENDED", true, true);
    } else {
        return Result<DispatchSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_RECONCILE_INCONCLUSIVE",
            "provider has not reached a sealed terminal or safe point",
            true, SideEffectState::Unknown));
    }
    return Result<DispatchSnapshot>::Success(live);
}


}  // namespace master_agent::agent_dispatch
