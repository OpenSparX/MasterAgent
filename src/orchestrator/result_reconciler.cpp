/**
 * @file result_reconciler.cpp
 * @brief Applies observations, settles plans, and emits task events.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {

bool Orchestrator::advanceRecurringActivationsUnlocked(
    PlanRuntime& plan) {
    if (plan.snapshot.cancellation_requested ||
        plan.snapshot.state == PlanState::Suspended ||
        planTerminal(plan.snapshot.state)) {
        return false;
    }
    bool scheduled = false;
    const auto now_utc_ms = clock_->utcNowMs();
    for (auto& [node_id, node_binding] : plan.snapshot.nodes) {
        (void)node_id;
        auto& node = node_binding;
        if (!nodeTerminal(node.state) || node.activation_id.empty()) {
            continue;
        }
        const auto& active_activation_id = node.activation_id;
        const bool archived = std::any_of(
            node.activation_history.begin(),
            node.activation_history.end(),
            [&active_activation_id](const ActivationHistoryEntry& entry) {
                return entry.activation_id == active_activation_id;
            });
        if (!archived) {
            ActivationHistoryEntry entry;
            entry.activation_id = node.activation_id;
            entry.trigger_instance_id = node.trigger_instance_id;
            entry.state = node.state;
            entry.attempt_count = node.attempt_count;
            entry.execution_id = node.execution_id;
            entry.operation_id = node.operation_id;
            entry.result = node.result;
            entry.error_code = node.error_code;
            entry.side_effect_state = node.side_effect_state;
            entry.terminal_at_utc_ms = now_utc_ms;
            node.activation_history.push_back(std::move(entry));
            ++plan.snapshot.version;
            emit(plan, &node, "ACTIVATION_ARCHIVED");
        }

        const bool recurring =
            node.definition.trigger.repeat_policy != RepeatPolicy::Once;
        const bool activation_budget_live =
            node.activation_count <
            node.definition.lifecycle.max_activations;
        const bool lifecycle_time_live =
            node.definition.lifecycle.end_at_utc_ms <= 0 ||
            now_utc_ms < node.definition.lifecycle.end_at_utc_ms;
        if (!recurring || !activation_budget_live ||
            !lifecycle_time_live) {
            continue;
        }

        const auto previous_trigger = node.trigger_instance_id;
        node.activation_id.clear();
        node.execution_id.clear();
        node.operation_id.clear();
        node.fencing_token = 0;
        node.attempt_count = 0;
        node.result = nlohmann::json::object();
        node.error_code.clear();
        node.side_effect_state = SideEffectState::NotStarted;
        node.retryable_hint = false;
        node.retry_at_mono_ns = 0;
        node.bound_params = node.definition.params;
        node.trigger_satisfied = false;
        node.trigger_instance_id.clear();
        node.blockers.clear();
        node.state = ActivationState::Blocked;

        if (node.pending_trigger_count > 0) {
            --node.pending_trigger_count;
            node.trigger_satisfied = true;
            node.activation_id = ids_->next("activation");
            ++node.activation_count;
            node.trigger_instance_id =
                node.pending_trigger_instance_id.empty()
                    ? ids_->next("trigger-queued")
                    : node.pending_trigger_instance_id;
            if (node.pending_trigger_count == 0) {
                node.pending_trigger_instance_id.clear();
            }
            ++plan.snapshot.version;
            emit(plan, &node, "QUEUED_TRIGGER_ACTIVATED");
            scheduled = true;
            continue;
        }

        if (node.definition.trigger.type == TriggerType::Timer) {
            const auto interval =
                node.definition.lifecycle.repeat_interval_ms;
            std::int64_t next_fire =
                node.definition.trigger.repeat_policy ==
                        RepeatPolicy::FixedDelay
                    ? now_utc_ms + interval
                    : node.next_fire_at_utc_ms + interval;
            if (next_fire <= now_utc_ms) {
                const auto missed = static_cast<std::uint64_t>(
                    (now_utc_ms - next_fire) / interval + 1);
                if (node.definition.trigger.missed_fire_policy ==
                    MissedFirePolicy::Skip) {
                    next_fire += static_cast<std::int64_t>(missed) *
                                 interval;
                    ++plan.snapshot.version;
                    emit(plan, &node, "TIMER_MISSED_FIRE_SKIPPED");
                } else {
                    next_fire = now_utc_ms;
                    if (node.definition.trigger.missed_fire_policy ==
                        MissedFirePolicy::CatchUpBounded) {
                        const auto remaining_budget =
                            node.definition.lifecycle.max_activations -
                            node.activation_count;
                        const auto catch_up = std::min<std::uint64_t>(
                            missed,
                            std::min<std::uint64_t>(
                                node.definition.trigger.catch_up_limit,
                                remaining_budget));
                        node.pending_trigger_count =
                            catch_up > 0
                                ? static_cast<std::uint32_t>(catch_up - 1)
                                : 0;
                    }
                    ++plan.snapshot.version;
                    emit(plan, &node,
                         node.definition.trigger.missed_fire_policy ==
                                 MissedFirePolicy::Coalesce
                             ? "TIMER_MISSED_FIRE_COALESCED"
                             : "TIMER_MISSED_FIRE_CATCH_UP_BOUNDED");
                }
            }
            node.next_fire_at_utc_ms = next_fire;
        } else {
            (void)previous_trigger;
        }
        ++plan.snapshot.version;
        emit(plan, &node, "ACTIVATION_WAITING_NEXT_TRIGGER");
        scheduled = true;
    }
    return scheduled;
}

void Orchestrator::applyObservation(
    const Observation& observation) {

    auto& plan = plans_.at(observation.plan_id);
    auto& node = plan.snapshot.nodes.at(observation.node_id);
    if (node.state != ActivationState::Queued &&
        node.state != ActivationState::Running &&
        node.state != ActivationState::Reconciling) {
        return;
    }
    if (node.pid != observation.pid ||
        node.activation_id != observation.activation_id ||
        node.execution_id != observation.execution_id ||
        node.operation_id != observation.operation_id ||
        node.attempt_count != observation.attempt_count ||
        node.fencing_token != observation.fencing_token) {
        return;
    }
    const bool success_side_effect_confirmed =
        observation.side_effect_state == SideEffectState::Committed ||
        observation.side_effect_state ==
            SideEffectState::NotApplicable;
    const bool requires_reconciliation =
        observation.state == ActivationState::Unknown ||
        observation.side_effect_state == SideEffectState::Unknown ||
        (observation.state == ActivationState::Succeeded &&
         !success_side_effect_confirmed);
    if (requires_reconciliation) {
        const bool transition =
            node.state != ActivationState::Reconciling;
        const bool payload_changed =
            node.result != observation.result ||
            node.error_code != observation.error_code ||
            node.side_effect_state != observation.side_effect_state;
        node.result = observation.result;
        node.error_code = observation.error_code;
        node.side_effect_state = observation.side_effect_state;
        node.state = ActivationState::Reconciling;
        if (node.error_code.empty()) {
            node.error_code =
                "ORCHESTRATOR_SIDE_EFFECT_RECONCILIATION_REQUIRED";
        }
        if (transition || payload_changed) {
            ++plan.snapshot.version;
            emit(plan, &node, "NODE_RECONCILING");
        }
        return;
    }
    if (observation.state == ActivationState::Queued) {
        // Repeated ACCEPTED/QUEUED snapshots are heartbeats, not state
        // transitions.  They must not manufacture a STARTED event.
        return;
    }
    if (observation.state == ActivationState::Running) {
        if (node.state != ActivationState::Running) {
            node.state = ActivationState::Running;
            ++plan.snapshot.version;
            emit(plan, &node, "NODE_RUNNING");
        }
        return;
    }
    node.result = observation.result;
    node.error_code = observation.error_code;
    node.side_effect_state = observation.side_effect_state;
    node.retryable_hint = observation.retryable_hint;
    const auto retry_policy =
        plan.admission.retry_policies.find(
            node.definition.action);
    const bool has_retry_policy =
        retry_policy != plan.admission.retry_policies.end();
    const bool retry_error_allowed =
        has_retry_policy &&
        retry_policy->second.retryable_errors.count(
            observation.error_code) != 0;
    const bool retry_effect_safe =
        has_retry_policy &&
        (observation.side_effect_state ==
             SideEffectState::NotStarted ||
         observation.side_effect_state ==
             SideEffectState::ConfirmedNotExecuted ||
         (observation.side_effect_state ==
              SideEffectState::NotApplicable &&
          retry_policy->second.idempotency_policy ==
              "READ_ONLY"));
    std::int64_t retry_at_mono_ns =
        std::numeric_limits<std::int64_t>::max();
    if (has_retry_policy) {
        std::int64_t delay =
            retry_policy->second.base_backoff_ns;
        for (std::uint32_t attempt = 1;
             attempt < node.attempt_count; ++attempt) {
            if (delay >=
                retry_policy->second.max_backoff_ns / 2) {
                delay =
                    retry_policy->second.max_backoff_ns;
                break;
            }
            delay = std::min(
                retry_policy->second.max_backoff_ns,
                delay * 2);
        }
        const auto now = clock_->monotonicNowNs();
        if (delay > 0 &&
            now <=
                std::numeric_limits<std::int64_t>::max() -
                    delay) {
            retry_at_mono_ns = now + delay;
        }
    }
    const bool retry_deadline_live =
        retry_at_mono_ns <
            node.definition.deadline_mono_ns &&
        retry_at_mono_ns <
            plan.snapshot.effective_deadline_mono_ns;
    if (observation.state == ActivationState::Failed &&
        observation.retryable_hint &&
        retry_error_allowed &&
        retry_effect_safe &&
        retry_deadline_live &&
        node.attempt_count < node.definition.max_attempts) {
        node.state = ActivationState::Blocked;
        node.retry_at_mono_ns = retry_at_mono_ns;
        ++plan.snapshot.version;
        emit(plan, &node, "NODE_RETRY_SCHEDULED");
    } else {
        node.retry_at_mono_ns = 0;
        node.state = observation.state;
        ++plan.snapshot.version;
        emit(plan, &node,
             node.state == ActivationState::Succeeded
                 ? "NODE_SUCCEEDED"
                 : (node.state == ActivationState::Unknown
                        ? "NODE_UNKNOWN"
                        : "NODE_TERMINAL"));
    }
    refreshBlockers(plan);
    settlePlan(plan);
}

void Orchestrator::settlePlan(PlanRuntime& plan) {

    if (advanceRecurringActivationsUnlocked(plan)) {
        refreshBlockers(plan);
        return;
    }

    bool all_terminal = true;
    bool any_failed = false;
    bool any_unknown = false;
    bool any_cancelled = false;
    for (const auto& pair : plan.snapshot.nodes) {
        all_terminal = all_terminal && nodeTerminal(pair.second.state);
        any_failed =
            any_failed || pair.second.state == ActivationState::Failed;
        any_unknown =
            any_unknown || pair.second.state == ActivationState::Unknown;
        any_cancelled =
            any_cancelled ||
            pair.second.state == ActivationState::Cancelled;
        for (const auto& activation :
             pair.second.activation_history) {
            any_failed =
                any_failed ||
                activation.state == ActivationState::Failed;
            any_unknown =
                any_unknown ||
                activation.state == ActivationState::Unknown;
            any_cancelled =
                any_cancelled ||
                activation.state == ActivationState::Cancelled;
        }
    }
    if (!all_terminal || planTerminal(plan.snapshot.state)) return;
    if (any_unknown) {
        plan.snapshot.state = PlanState::Unknown;
    } else if (any_failed) {
        plan.snapshot.state = PlanState::Failed;
    } else if (any_cancelled ||
               plan.snapshot.cancellation_requested) {
        plan.snapshot.state = PlanState::Cancelled;
    } else {
        plan.snapshot.state = PlanState::Succeeded;
    }
    ++plan.snapshot.version;
    emit(plan, nullptr, "PLAN_TERMINAL");
}

void Orchestrator::emit(PlanRuntime& plan,
                           const NodeRuntimeSnapshot* node,
                           const std::string& event_type) {
    TaskEvent event;
    event.event_id = ids_->next("task-event");
    event.event_type = event_type;
    event.plan_id = plan.snapshot.plan_id;
    if (node) {
        event.pid = node->pid;
        event.activation_id = node->activation_id;
        event.execution_id = node->execution_id;
        event.payload_digest =
            secureDigest(event_type + "|" + node->definition.node_id + "|" +
                         node->error_code);
    } else {
        event.payload_digest =
            secureDigest(event_type + "|" + plan.snapshot.plan_id);
    }
    event.plan_version = plan.snapshot.version;
    event.occurred_at_utc_ms = clock_->utcNowMs();
    event.trace_id = plan.snapshot.trace_id;
    plan.event_history.push_back(event);
    events_.push_back(std::move(event));
    if (!storage_directory_.empty() && durability_status_.ok) {
        const auto persisted = persistPlanUnlocked(plan);
        if (!persisted.ok) durability_status_ = persisted;
    }
}


}  // namespace master_agent::orchestrator
