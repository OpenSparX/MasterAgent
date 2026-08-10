/**
 * @file dispatch_state.cpp
 * @brief Applies guarded dispatch state transitions and emits events.
 */

#include "include/dispatch_state_rules.h"

#include <algorithm>
#include <utility>

namespace master_agent::agent_dispatch {

bool AgentDispatch::hasActiveChildInvocationUnlocked(
    const std::string& dispatch_id) const {
    const bool atomic_active = std::any_of(
        atomic_child_reservations_.begin(),
        atomic_child_reservations_.end(),
        [&dispatch_id](const auto& reservation) {
            return reservation.second.parent_dispatch_id ==
                   dispatch_id;
        });
    return atomic_active ||
           std::any_of(
               inference_child_reservations_.begin(),
               inference_child_reservations_.end(),
               [&dispatch_id](const auto& reservation) {
                   return reservation.second.parent_dispatch_id ==
                          dispatch_id;
               });
}

void AgentDispatch::applyDeferredParentTransitionUnlocked(
    const std::string& dispatch_id) {
    if (hasActiveChildInvocationUnlocked(dispatch_id)) {
        return;
    }
    const auto pending =
        deferred_parent_transitions_.find(dispatch_id);
    if (pending == deferred_parent_transitions_.end()) {
        return;
    }
    const auto deferred = pending->second;
    deferred_parent_transitions_.erase(pending);
    const auto found = dispatches_.find(dispatch_id);
    if (found == dispatches_.end()) return;
    auto& snapshot = found->second;
    setStateUnlocked(snapshot, deferred.state);
    const auto event_type =
        !deferred.event_type.empty()
            ? deferred.event_type
            : deferred.state == DispatchState::Succeeded
                  ? "SUCCEEDED"
                  : deferred.state == DispatchState::Failed
                        ? "FAILED"
                        : deferred.state ==
                                  DispatchState::Cancelled
                              ? "CANCELLED"
                              : deferred.state ==
                                        DispatchState::Suspended
                                    ? "SUSPENDED"
                                    : "UNKNOWN";
    emit(snapshot, event_type, deferred.safe_point,
         deferred.resource_released);
}

void AgentDispatch::setStateUnlocked(
    DispatchSnapshot& snapshot, DispatchState state) {
    if (snapshot.state == state) return;
    if (snapshot.state == DispatchState::Running &&
        state != DispatchState::Running &&
        hasActiveChildInvocationUnlocked(
            snapshot.dispatch_id)) {
        auto& deferred =
            deferred_parent_transitions_[snapshot.dispatch_id];
        // UNKNOWN dominates a provider terminal observation because it is
        // the conservative state whenever ownership/effect evidence raced.
        if (deferred.state != DispatchState::Unknown ||
            state == DispatchState::Unknown) {
            deferred.state = state;
        }
        return;
    }
    const bool consumed_before =
        consumesAgentCredit(snapshot.state);
    const bool consumed_after = consumesAgentCredit(state);
    snapshot.state = state;
    if (consumed_before != consumed_after) {
        ++capacity_epoch_;
    }
}

void AgentDispatch::markUnknownUnlocked(
    const std::string& dispatch_id, const std::string& error_code) {
    const auto found = dispatches_.find(dispatch_id);
    if (found == dispatches_.end() ||
        found->second.state == DispatchState::Succeeded ||
        found->second.state == DispatchState::Failed ||
        found->second.state == DispatchState::Cancelled ||
        found->second.state == DispatchState::Unknown) {
        return;
    }
    auto& snapshot = found->second;
    setStateUnlocked(snapshot, DispatchState::Unknown);
    snapshot.result = nlohmann::json::object();
    snapshot.side_effect_state = SideEffectState::Unknown;
    snapshot.retryable_hint = false;
    snapshot.error_code = error_code;
    // Do not assert resource release: the external Agent may still own the
    // operation. Capacity accounting deliberately retains UNKNOWN leases.
    emit(snapshot, "UNKNOWN", false, false);
}

void AgentDispatch::emit(DispatchSnapshot& snapshot,
                            const std::string& event_type,
                            bool safe_point,
                            bool resource_released) {
    const auto pending =
        deferred_parent_transitions_.find(
            snapshot.dispatch_id);
    if (pending != deferred_parent_transitions_.end() &&
        snapshot.state == DispatchState::Running) {
        const auto target_matches =
            (pending->second.state ==
                 DispatchState::Succeeded &&
             event_type == "SUCCEEDED") ||
            (pending->second.state ==
                 DispatchState::Failed &&
             event_type == "FAILED") ||
            (pending->second.state ==
                 DispatchState::Cancelled &&
             event_type == "CANCELLED") ||
            (pending->second.state ==
                 DispatchState::Suspended &&
             event_type == "SUSPENDED") ||
            (pending->second.state ==
                 DispatchState::Unknown &&
             event_type == "UNKNOWN");
        if (target_matches) {
            pending->second.event_type = event_type;
            pending->second.safe_point = safe_point;
            pending->second.resource_released =
                resource_released;
            return;
        }
        if (event_type == "SUCCEEDED" ||
            event_type == "FAILED" ||
            event_type == "CANCELLED" ||
            event_type == "SUSPENDED" ||
            event_type == "UNKNOWN") {
            return;
        }
    }
    DispatchEvent event;
    event.event_id = ids_->next("dispatch-event");
    event.event_type = event_type;
    event.dispatch_id = snapshot.dispatch_id;
    event.request_id = snapshot.task.request_id;
    event.plan_id = snapshot.task.plan_id;
    event.pid = snapshot.task.pid;
    event.activation_id = snapshot.task.activation_id;
    event.execution_id = snapshot.task.execution_id;
    event.attempt_no = snapshot.task.attempt_no;
    event.operation_id = snapshot.task.operation_id;
    event.agent_id = snapshot.route.agent_id;
    event.agent_epoch = snapshot.route.agent_epoch;
    event.lease_id = snapshot.route.lease_id;
    event.state = snapshot.state;
    event.fencing_token = snapshot.task.fencing_token;
    event.side_effect_state = snapshot.side_effect_state;
    event.result = snapshot.result;
    event.output_schema_version =
        snapshot.task.expected_output_schema_version;
    event.error_code = snapshot.error_code;
    event.safe_point = safe_point;
    event.checkpoint_ref = snapshot.checkpoint_ref;
    event.resource_released = resource_released;
    event.occurred_at_utc_ms = clock_->utcNowMs();
    event.trace_id = snapshot.task.trace_id;
    events_.push_back(std::move(event));
    recordCapacityUnlocked();
    const auto persisted = persistStateUnlocked();
    durable_ = persisted.ok;
}

}  // namespace master_agent::agent_dispatch
