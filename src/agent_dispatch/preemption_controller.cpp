/**
 * @file preemption_controller.cpp
 * @brief Validates and applies cooperative dispatch preemption.
 */

#include "include/dispatch_state_rules.h"
#include "include/provider_identity.h"

#include <algorithm>

namespace master_agent::agent_dispatch {

Status AgentDispatch::requestPreempt(
    const std::string& dispatch_id, TaskPriority arriving_priority,
    std::uint64_t control_epoch, const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::TaskOrchestrationEngine)) {
        return Status::Error("agent_dispatch",
                             "DISPATCH_PREEMPT_CALLER_NOT_ALLOWED",
                             "only Orchestrator may request preemption");
    }
    if (!clock_ || call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        control_epoch == 0 ||
        !isValidTaskPriority(call.priority) ||
        !isValidTaskPriority(arriving_priority) ||
        call.priority != arriving_priority ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_PREEMPT_CALL_INVALID",
            "preemption call identity or absolute deadline is invalid");
    }
    return requestPreemptInternal(
        dispatch_id, arriving_priority, control_epoch,
        call.deadline_mono_ns, call.authorization_ref);
}

Status AgentDispatch::requestPreemptInternal(
    const std::string& dispatch_id, TaskPriority arriving_priority,
    std::uint64_t control_epoch,
    std::int64_t control_deadline_mono_ns,
    const std::string& authorization_ref) {
    if (!clock_ || control_deadline_mono_ns <= 0 ||
        deadlineExpired(control_deadline_mono_ns, *clock_) ||
        !isValidTaskPriority(arriving_priority) ||
        (arriving_priority == TaskPriority::P0 &&
         authorization_ref.rfind("trusted-safety:", 0) != 0)) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_PREEMPT_CALL_INVALID",
            "scheduler preemption authority is invalid or expired");
    }
    std::shared_ptr<sub_agents::ISubAgent> provider;
    DispatchTask victim_task;
    AgentRouteDecision victim_route;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = dispatches_.find(dispatch_id);
        if (found == dispatches_.end()) {
            return Status::Error("agent_dispatch",
                                 "DISPATCH_NOT_FOUND",
                                 "dispatch was not found");
        }
        if (control_epoch < found->second.control_epoch) {
            return Status::Error("agent_dispatch",
                                 "DISPATCH_CONTROL_EPOCH_STALE",
                                 "control epoch is stale");
        }
        if (control_epoch == found->second.control_epoch &&
            control_epoch != 0) {
            return Status::Ok();
        }
        if (found->second.state != DispatchState::Running ||
            !isHigherPriority(arriving_priority,
                              found->second.task.priority)) {
            return Status::Error("agent_dispatch",
                                 "DISPATCH_PREEMPT_NOT_APPLICABLE",
                                 "dispatch cannot be preempted");
        }
        const auto agent =
            agents_.find(found->second.route.agent_id);
        if (agent == agents_.end() ||
            !agent->second.manifest
                 .supports_safe_point_preemption) {
            return Status::Error(
                "agent_dispatch",
                "DISPATCH_PREEMPT_NOT_SUPPORTED",
                "selected Agent does not support safe-point preemption");
        }
        if (hasActiveChildInvocationUnlocked(dispatch_id)) {
            return Status::Error(
                "agent_dispatch",
                "DISPATCH_CHILD_INVOCATION_ACTIVE",
                "parent AgentLease is pinned by an Atomic child invocation",
                true, SideEffectState::NotStarted);
        }
        provider = dispatch_providers_.at(dispatch_id);
        victim_task = found->second.task;
        victim_route = found->second.route;
    }
    const auto provider_deadline =
        std::min(victim_task.deadline_mono_ns,
                 control_deadline_mono_ns);
    CallContext provider_call{
        CallerModuleId::AgentDispatch, victim_task.request_id,
        victim_task.trace_id, victim_task.principal_id_hash,
        victim_task.priority, provider_deadline, {}, 0,
        victim_task.authorization_ref};
    Status preempted = Status::Error(
        "agent_dispatch", "DISPATCH_PREEMPT_PROVIDER_EXCEPTION",
        "agent provider did not return from preemption", true,
        SideEffectState::Unknown);
    try {
        preempted = provider->requestPreempt(
            dispatch_id, control_epoch, provider_call);
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        markUnknownUnlocked(
            dispatch_id, "DISPATCH_PREEMPT_PROVIDER_EXCEPTION");
        return Status::Error(
            "agent_dispatch",
            "DISPATCH_PREEMPT_PROVIDER_EXCEPTION",
            "agent provider threw during preemption", false,
            SideEffectState::Unknown);
    }
    if (!preempted.ok) {
        if (preempted.error.side_effect_state ==
            SideEffectState::Unknown) {
            std::lock_guard<std::mutex> lock(mutex_);
            markUnknownUnlocked(
                dispatch_id, "DISPATCH_PREEMPT_UNCONFIRMED");
        }
        return preempted;
    }
    if (deadlineExpired(control_deadline_mono_ns, *clock_)) {
        std::lock_guard<std::mutex> lock(mutex_);
        markUnknownUnlocked(
            dispatch_id, "DISPATCH_PREEMPT_RESULT_AFTER_DEADLINE");
        return Status::Error(
            "agent_dispatch",
            "DISPATCH_PREEMPT_RESULT_AFTER_DEADLINE",
            "provider may have preempted after control authority expired",
            false, SideEffectState::Unknown);
    }
    auto provider_snapshot =
        Result<sub_agents::SubAgentSnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_PREEMPT_QUERY_EXCEPTION",
            "agent provider did not return a checkpoint", true,
            SideEffectState::Unknown));
    try {
        provider_snapshot =
            provider->query(dispatch_id, provider_call);
    } catch (...) {
        // The Provider accepted the preemption but its resulting checkpoint
        // cannot be observed. The physical execution fact is UNKNOWN.
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = dispatches_.find(dispatch_id);
    if (found == dispatches_.end() ||
        found->second.state != DispatchState::Running ||
        found->second.route.agent_epoch != victim_route.agent_epoch ||
        found->second.route.lease_id != victim_route.lease_id) {
        return Status::Error("agent_dispatch",
                             "DISPATCH_PREEMPT_STATE_CHANGED",
                             "dispatch ownership changed during preemption",
                             true);
    }
    auto& snapshot = found->second;
    // A successful control acknowledgement consumes the epoch even when the
    // subsequent safe-point proof is missing. This lets reconciliation bind
    // the Provider's eventual snapshot to the exact attempted transition.
    snapshot.control_epoch = control_epoch;
    if (deadlineExpired(control_deadline_mono_ns, *clock_)) {
        markUnknownUnlocked(
            dispatch_id, "DISPATCH_PREEMPT_RESULT_AFTER_DEADLINE");
        return Status::Error(
            "agent_dispatch",
            "DISPATCH_PREEMPT_RESULT_AFTER_DEADLINE",
            "late checkpoint evidence cannot publish SUSPENDED",
            false, SideEffectState::Unknown);
    }
    if (!provider_snapshot.status.ok || !provider_snapshot.value) {
        const auto error_code =
            provider_snapshot.status.ok
                ? "DISPATCH_PREEMPT_SNAPSHOT_MISSING"
                : provider_snapshot.status.error.code;
        markUnknownUnlocked(dispatch_id, error_code);
        return Status::Error(
            "agent_dispatch", error_code,
            "provider preempted but checkpoint state is unconfirmed",
            false, SideEffectState::Unknown);
    }
    if (!providerIdentityMatches(
            *provider_snapshot.value, snapshot,
            control_epoch)) {
        markUnknownUnlocked(
            dispatch_id, "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
        return Status::Error(
            "agent_dispatch",
            "DISPATCH_PROVIDER_IDENTITY_MISMATCH",
            "provider snapshot is not bound to dispatch",
            false, SideEffectState::Unknown);
    }
    if (provider_snapshot.value->state !=
            sub_agents::SubAgentState::Suspended ||
        provider_snapshot.value->checkpoint_ref.empty() ||
        !provider_snapshot.value->resource_released ||
        !validSuspendedSideEffect(
            provider_snapshot.value->side_effect_state)) {
        markUnknownUnlocked(
            dispatch_id, "DISPATCH_PREEMPT_PROOF_INVALID");
        return Status::Error(
            "agent_dispatch", "DISPATCH_PREEMPT_PROOF_INVALID",
            "provider did not prove a sealed safe point and release",
            false, SideEffectState::Unknown);
    }
    emit(snapshot, "PREEMPT_ACCEPTED");
    provider_submitted_.erase(dispatch_id);
    setStateUnlocked(snapshot, DispatchState::Suspended);
    snapshot.checkpoint_ref =
        provider_snapshot.value->checkpoint_ref;
    snapshot.side_effect_state =
        provider_snapshot.value->side_effect_state;
    emit(snapshot, "SAFE_POINT_REACHED", true, true);
    emit(snapshot, "SUSPENDED", true, true);
    return Status::Ok();
}


}  // namespace master_agent::agent_dispatch
