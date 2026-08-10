/**
 * @file execution_scheduler.cpp
 * @brief Schedules queued calls, applies preemption, and reconciles execution.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

bool AtomicServiceManager::pumpOne() {

    struct ProviderCall {
        std::string execution_id;
        std::string resource_key;
        std::uint64_t fencing_token = 0;
        AtomicMcpCallEnvelope envelope;
        AtomicProviderInvocationSeal invocation_seal;
        std::shared_ptr<IAtomicProvider> provider;
    };
    std::optional<ProviderCall> provider_call;
    bool progressed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!durability_status_.ok) return false;
        for (auto& pair : executions_) {
            auto& snapshot = pair.second;
            if (!terminal(snapshot.state) &&
                deadlineExpired(
                    snapshot.envelope.runtime.deadline_mono_ns,
                    *clock_)) {

                const bool may_have_executed =
                    snapshot.provider_invocation.has_value() ||
                    provider_inflight_.count(pair.first) != 0;
                snapshot.state = may_have_executed
                                     ? AtomicExecutionState::Unknown
                                     : AtomicExecutionState::Failed;
                snapshot.error_code = "ATOMIC_DEADLINE_EXPIRED";
                snapshot.side_effect_state =
                    may_have_executed ? SideEffectState::Unknown
                                      : SideEffectState::NotStarted;
                emit(snapshot,
                     may_have_executed ? "UNKNOWN" : "FAILED", false,
                     !may_have_executed);
                progressed = true;
            }
        }

        for (auto& pair : executions_) {
            auto& snapshot = pair.second;
            if ((snapshot.state != AtomicExecutionState::Queued &&
                 snapshot.state != AtomicExecutionState::Suspended &&
                 snapshot.state != AtomicExecutionState::Running) ||
                provider_inflight_.count(pair.first) != 0) {
                continue;
            }
            const auto high =
                highest_fencing_by_resource_.find(snapshot.resource_key);
            if (high != highest_fencing_by_resource_.end() &&
                (snapshot.envelope.runtime.fencing_token <
                     high->second.fencing_token ||
                 (snapshot.envelope.runtime.fencing_token ==
                      high->second.fencing_token &&
                  snapshot.envelope.runtime.caller_module_id ==
                      CallerModuleId::SubAgent &&
                  (snapshot.envelope.runtime.parent_dispatch_id !=
                       high->second.parent_dispatch_id ||
                   snapshot.envelope.runtime.parent_lease_id !=
                       high->second.parent_lease_id)))) {
                snapshot.state = AtomicExecutionState::Failed;
                snapshot.error_code = "ATOMIC_STALE_FENCING_TOKEN";
                snapshot.side_effect_state =
                    SideEffectState::NotStarted;
                emit(snapshot, "FAILED", false, true);
                progressed = true;
            }
        }

        auto queued = selectQueued();
        if (queued) {
            std::size_t running = 0;
            for (const auto& pair : executions_) {
                if (pair.second.state == AtomicExecutionState::Running ||
                    pair.second.state == AtomicExecutionState::Unknown) {
                    ++running;
                }
            }
            const auto& queued_snapshot = executions_.at(*queued);
            const auto queued_tool =
                queued_snapshot.envelope.mcp_request.name;
            const auto queued_priority =
                queued_snapshot.envelope.runtime.priority;
            const auto policy_limit =
                execution_tools_.at(*queued).policy.max_concurrency;
            std::size_t tool_active = 0;
            for (const auto& pair : executions_) {
                if (pair.second.envelope.mcp_request.name == queued_tool &&
                    (pair.second.state == AtomicExecutionState::Running ||
                     pair.second.state == AtomicExecutionState::Unknown)) {
                    ++tool_active;
                }
            }
            const auto suspend_victim =
                [this, &running](const std::string& victim_id) {
                    auto& victim_snapshot = executions_.at(victim_id);
                    emit(victim_snapshot, "PREEMPT_ACCEPTED");
                    emit(victim_snapshot, "SAFE_POINT_REACHED", true);
                    victim_snapshot.state =
                        AtomicExecutionState::Suspended;
                    emit(victim_snapshot, "SUSPENDED", true, true);
                    if (running > 0) --running;
                };

            if (tool_active >= policy_limit) {
                const auto victim = selectVictim(
                    queued_priority, queued_tool);
                if (victim) {
                    suspend_victim(*victim);
                    --tool_active;
                    progressed = true;
                }
            }
            if (tool_active < policy_limit &&
                running >= max_inflight_) {
                const auto victim = selectVictim(queued_priority);
                if (victim) {
                    suspend_victim(*victim);
                    progressed = true;
                }
            }
            if (tool_active < policy_limit &&
                running < max_inflight_) {
                auto& next = executions_.at(*queued);
                next.state = AtomicExecutionState::Running;
                emit(next, "STARTED");
                progressed = true;
            }
        }

        std::size_t running = 0;
        for (const auto& pair : executions_) {
            if (pair.second.state == AtomicExecutionState::Running ||
                pair.second.state == AtomicExecutionState::Unknown) {
                ++running;
            }
        }
        if (running < max_inflight_ && !selectQueued()) {
            for (auto& pair : executions_) {
                if (pair.second.state ==
                        AtomicExecutionState::Suspended &&
                    provider_inflight_.count(pair.first) == 0) {
                    const auto tool = execution_tools_.find(pair.first);
                    if (tool == execution_tools_.end()) continue;
                    const auto& tool_name =
                        pair.second.envelope.mcp_request.name;
                    const auto tool_active = std::count_if(
                        executions_.begin(), executions_.end(),
                        [&pair, &tool_name](const auto& other) {
                            return other.first != pair.first &&
                                   other.second.envelope.mcp_request.name ==
                                       tool_name &&
                                   (other.second.state ==
                                        AtomicExecutionState::Running ||
                                    other.second.state ==
                                        AtomicExecutionState::Unknown);
                        });
                    if (static_cast<std::uint32_t>(tool_active) >=
                        tool->second.policy.max_concurrency) {
                        continue;
                    }
                    const bool resource_busy = std::any_of(
                        executions_.begin(), executions_.end(),
                        [&pair](const auto& other) {
                            return other.first != pair.first &&
                                   other.second.resource_key ==
                                       pair.second.resource_key &&
                                   (other.second.state ==
                                        AtomicExecutionState::Running ||
                                    other.second.state ==
                                        AtomicExecutionState::Unknown);
                        });
                    if (resource_busy) continue;
                    pair.second.state = AtomicExecutionState::Running;
                    emit(pair.second, "STARTED");
                    ++running;
                    progressed = true;
                    if (running == max_inflight_) break;
                }
            }
        }

        for (auto& pair : executions_) {
            auto& snapshot = pair.second;
            if (snapshot.state != AtomicExecutionState::Running ||
                provider_inflight_.count(pair.first) != 0) {
                continue;
            }
            if (snapshot.remaining_work_units > 0) {
                --snapshot.remaining_work_units;
                progressed = true;
            }
            if (snapshot.remaining_work_units != 0 || provider_call) {
                continue;
            }
            const auto high =
                highest_fencing_by_resource_.find(snapshot.resource_key);
            if (high == highest_fencing_by_resource_.end() ||
                high->second.fencing_token !=
                    snapshot.envelope.runtime.fencing_token ||
                (snapshot.envelope.runtime.caller_module_id ==
                     CallerModuleId::SubAgent &&
                 (high->second.parent_dispatch_id !=
                      snapshot.envelope.runtime.parent_dispatch_id ||
                  high->second.parent_lease_id !=
                      snapshot.envelope.runtime.parent_lease_id))) {
                snapshot.state = AtomicExecutionState::Failed;
                snapshot.error_code = "ATOMIC_STALE_FENCING_TOKEN";
                snapshot.side_effect_state =
                    SideEffectState::NotStarted;
                emit(snapshot, "FAILED", false, true);
                progressed = true;
                continue;
            }
            const auto& record = execution_tools_.at(pair.first);
            AtomicProviderInvocationSeal invocation_seal;
            invocation_seal.invocation_id =
                ids_->next("atomic-provider-invocation");
            invocation_seal.provider_id =
                record.provider_id;
            invocation_seal.provider_epoch =
                record.provider_epoch;
            invocation_seal.operation_id =
                snapshot.envelope.runtime.operation_id;
            invocation_seal.execution_id =
                snapshot.envelope.runtime.execution_id;
            invocation_seal.attempt_no =
                snapshot.envelope.runtime.attempt_no;
            invocation_seal.tool_name =
                snapshot.envelope.mcp_request.name;
            invocation_seal.tool_catalog_snapshot_id =
                snapshot.envelope.runtime.tool_catalog_snapshot_id;
            invocation_seal.tool_digest =
                snapshot.envelope.runtime.tool_digest;
            invocation_seal.policy_digest =
                snapshot.envelope.runtime.policy_digest;
            invocation_seal.fencing_token =
                snapshot.envelope.runtime.fencing_token;
            invocation_seal.request_digest =
                callDigest(snapshot.envelope);
            snapshot.provider_invocation = invocation_seal;

            const auto sealed =
                persistCurrentExecutionUnlocked(pair.first);
            if (!sealed.ok) {
                snapshot.provider_invocation.reset();
                snapshot.state = AtomicExecutionState::Failed;
                snapshot.result.reset();
                snapshot.side_effect_state =
                    SideEffectState::NotStarted;
                snapshot.error_code =
                    "ATOMIC_PROVIDER_SEAL_NOT_DURABLE";
                emit(snapshot, "FAILED", false, true);
                progressed = true;
                continue;
            }
            provider_inflight_.insert(pair.first);
            provider_call = ProviderCall{
                pair.first, snapshot.resource_key,
                snapshot.envelope.runtime.fencing_token,
                snapshot.envelope, std::move(invocation_seal),
                record.provider};
        }
    }

    if (!provider_call) return progressed;

    std::string parent_invocation_reservation;
    const auto fail_before_provider =
        [this, &provider_call](const std::string& error_code) {
            std::lock_guard<std::mutex> lock(mutex_);
            provider_inflight_.erase(
                provider_call->execution_id);
            const auto found = executions_.find(
                provider_call->execution_id);
            if (found == executions_.end()) return true;
            auto& snapshot = found->second;
            if (!snapshot.provider_invocation ||
                !invocationSealMatches(
                    *snapshot.provider_invocation,
                    provider_call->invocation_seal)) {
                return true;
            }
            snapshot.provider_invocation.reset();
            snapshot.state = AtomicExecutionState::Failed;
            snapshot.result.reset();
            snapshot.side_effect_state =
                SideEffectState::NotStarted;
            snapshot.error_code = error_code;
            emit(snapshot, "FAILED", false, true);
            return true;
        };

    if (provider_call->envelope.runtime.caller_module_id ==
        CallerModuleId::SubAgent) {
        Result<std::string> reserved =
            Result<std::string>::Failure(Status::Error(
                "atomic_service",
                "ATOMIC_PARENT_INVOCATION_LEASE_FAILED",
                "parent invocation lease was not acquired"));
        try {
            const auto& runtime =
                provider_call->envelope.runtime;
            CallContext child_call{
                CallerModuleId::SubAgent,
                runtime.request_id,
                runtime.trace_id,
                runtime.principal_id_hash,
                runtime.priority,
                runtime.deadline_mono_ns,
                {},
                0,
                runtime.authorization_ref};
            reserved =
                lineage_validator_->
                    acquireAtomicParentInvocationLease(
                        provider_call->envelope, child_call);
        } catch (...) {
            return fail_before_provider(
                "ATOMIC_PARENT_INVOCATION_LEASE_EXCEPTION");
        }
        if (!reserved.status.ok || !reserved.value ||
            reserved.value->empty()) {
            return fail_before_provider(
                reserved.status.ok
                    ? "ATOMIC_PARENT_INVOCATION_LEASE_INVALID"
                    : reserved.status.error.code);
        }
        parent_invocation_reservation =
            *reserved.value;
    }

    if (!clock_ ||
        deadlineExpired(
            provider_call->envelope.runtime.deadline_mono_ns,
            *clock_)) {
        if (!parent_invocation_reservation.empty()) {
            try {
                lineage_validator_->
                    releaseAtomicParentInvocationLease(
                        parent_invocation_reservation);
            } catch (...) {
                // The Provider was not entered, so the effect remains
                // provably NotStarted even if cleanup needs reconciliation.
            }
        }
        return fail_before_provider(
            "ATOMIC_DEADLINE_EXPIRED");
    }

    ProviderInvocationResult invoked;
    bool provider_threw = false;
    try {
        invoked = provider_call->provider->call(
            provider_call->envelope,
            provider_call->invocation_seal);
    } catch (...) {
        provider_threw = true;
        invoked.state = ProviderInvocationState::Unknown;
        invoked.side_effect_state = SideEffectState::Unknown;
        invoked.error_code = "ATOMIC_PROVIDER_EXCEPTION";
    }

    Status parent_reservation_release = Status::Ok();
    bool parent_reservation_release_threw = false;
    if (!parent_invocation_reservation.empty()) {
        try {
            parent_reservation_release =
                lineage_validator_->
                    releaseAtomicParentInvocationLease(
                        parent_invocation_reservation);
        } catch (...) {
            parent_reservation_release_threw = true;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    provider_inflight_.erase(provider_call->execution_id);
    auto found = executions_.find(provider_call->execution_id);
    if (found == executions_.end()) return true;
    auto& snapshot = found->second;
    if (parent_reservation_release_threw ||
        !parent_reservation_release.ok) {
        snapshot.state = AtomicExecutionState::Unknown;
        snapshot.result.reset();
        snapshot.side_effect_state = SideEffectState::Unknown;
        snapshot.error_code =
            parent_reservation_release_threw
                ? "ATOMIC_PARENT_INVOCATION_LEASE_RELEASE_EXCEPTION"
                : parent_reservation_release.error.code;
        emit(snapshot, "UNKNOWN", false, false);
        return true;
    }
    if (!snapshot.provider_invocation ||
        !invocationSealMatches(*snapshot.provider_invocation,
                               provider_call->invocation_seal) ||
        (!provider_threw &&
         !invocationSealMatches(invoked.invocation_seal,
                                provider_call->invocation_seal))) {
        snapshot.state = AtomicExecutionState::Unknown;
        snapshot.result.reset();
        snapshot.side_effect_state = SideEffectState::Unknown;
        snapshot.error_code =
            "ATOMIC_PROVIDER_INVOCATION_SEAL_MISMATCH";
        emit(snapshot, "UNKNOWN", false, false);
        return true;
    }
    const auto high =
        highest_fencing_by_resource_.find(provider_call->resource_key);
    const bool provider_deadline_expired =
        deadlineExpired(
            snapshot.envelope.runtime.deadline_mono_ns,
            *clock_);
    const bool commit_allowed =
        snapshot.state == AtomicExecutionState::Running &&
        !provider_deadline_expired &&
        high != highest_fencing_by_resource_.end() &&
        high->second.fencing_token ==
            provider_call->fencing_token &&
        (snapshot.envelope.runtime.caller_module_id !=
             CallerModuleId::SubAgent ||
         (high->second.parent_dispatch_id ==
              snapshot.envelope.runtime.parent_dispatch_id &&
          high->second.parent_lease_id ==
              snapshot.envelope.runtime.parent_lease_id)) &&
        snapshot.envelope.runtime.fencing_token ==
            provider_call->fencing_token;
    if (!commit_allowed) {
        snapshot.state = AtomicExecutionState::Unknown;
        snapshot.result.reset();
        snapshot.side_effect_state = SideEffectState::Unknown;
        snapshot.error_code =
            provider_deadline_expired
                ? "ATOMIC_PROVIDER_RESULT_AFTER_DEADLINE"
                : "ATOMIC_LATE_PROVIDER_RESULT";
        emit(snapshot, "UNKNOWN", false, false);
        return true;
    }
    snapshot.result = invoked.result;
    snapshot.side_effect_state = invoked.side_effect_state;
    snapshot.completion_evidence = invoked.completion_evidence;
    snapshot.error_code = invoked.error_code;
    snapshot.retryable_hint = invoked.retryable_hint;
    if (invoked.state == ProviderInvocationState::Succeeded) {
        if (invoked.side_effect_state != SideEffectState::Committed &&
            invoked.side_effect_state !=
                SideEffectState::NotApplicable) {
            snapshot.state = AtomicExecutionState::Unknown;
            snapshot.result.reset();
            snapshot.side_effect_state = SideEffectState::Unknown;
            snapshot.error_code =
                "ATOMIC_PROVIDER_SUCCESS_SIDE_EFFECT_UNCONFIRMED";
            emit(snapshot, "UNKNOWN", false, false);
            return true;
        }
        const auto& output_schema =
            execution_tools_.at(provider_call->execution_id)
                .definition.output_schema;
        const auto output_validation =
            output_schema.empty()
                ? Status::Ok()
                : validateArguments(output_schema,
                                    invoked.result.structured_content);
        if (!output_validation.ok || invoked.result.is_error) {

            snapshot.state = AtomicExecutionState::Unknown;
            snapshot.result.reset();
            snapshot.side_effect_state = SideEffectState::Unknown;
            snapshot.error_code =
                "ATOMIC_PROVIDER_OUTPUT_SCHEMA_INVALID";
            emit(snapshot, "UNKNOWN", false, false);
            return true;
        }
        const auto completion_policy =
            execution_tools_.at(provider_call->execution_id)
                .policy.completion_policy;
        if (!satisfiesCompletionPolicy(completion_policy,
                                       invoked.completion_evidence)) {
            // A valid MCP payload is not proof that the physical capability
            // reached the post-condition promised by its frozen policy.
            snapshot.state = AtomicExecutionState::Unknown;
            snapshot.result.reset();
            snapshot.side_effect_state = SideEffectState::Unknown;
            snapshot.error_code =
                "ATOMIC_COMPLETION_EVIDENCE_MISMATCH";
            emit(snapshot, "UNKNOWN", false, false);
            return true;
        }
        snapshot.state = AtomicExecutionState::Succeeded;
        emit(snapshot, "SUCCEEDED", false, true);
    } else if (invoked.state == ProviderInvocationState::Failed) {
        const bool side_effect_known =
            isValidSideEffectState(invoked.side_effect_state) &&
            invoked.side_effect_state != SideEffectState::Unknown;
        snapshot.state = side_effect_known
                             ? AtomicExecutionState::Failed
                             : AtomicExecutionState::Unknown;
        if (!side_effect_known) {
            snapshot.result.reset();
            snapshot.side_effect_state = SideEffectState::Unknown;
            if (snapshot.error_code.empty()) {
                snapshot.error_code =
                    "ATOMIC_PROVIDER_FAILURE_SIDE_EFFECT_UNKNOWN";
            }
        }
        // Known committed/compensated business failure is terminal Failed;
        // the side-effect truth forbids blind retry but needs no reconcile.
        emit(snapshot, side_effect_known ? "FAILED" : "UNKNOWN",
             false, side_effect_known);
    } else {
        snapshot.state = AtomicExecutionState::Unknown;
        snapshot.result.reset();
        snapshot.side_effect_state = SideEffectState::Unknown;
        emit(snapshot, "UNKNOWN", false, false);
    }
    return progressed;
}

Status AtomicServiceManager::runUntilIdle(std::size_t max_steps) {
    for (std::size_t i = 0; i < max_steps; ++i) {
        if (!pumpOne()) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!durability_status_.ok) {
                return durability_status_;
            }
            const bool pending = std::any_of(
                executions_.begin(), executions_.end(),
                [](const auto& pair) {
                    return pair.second.state ==
                               AtomicExecutionState::Queued ||
                           pair.second.state ==
                               AtomicExecutionState::Running ||
                           pair.second.state ==
                               AtomicExecutionState::Suspended;
                });
            return pending
                       ? Status::Error(
                             "atomic_service", "ATOMIC_STALLED",
                             "non-terminal work cannot make progress", true)
                       : Status::Ok();
        }
    }
    return Status::Error("atomic_service", "ATOMIC_PUMP_LIMIT",
                         "atomic service did not become idle");
}

std::vector<AtomicExecutionEvent> AtomicServiceManager::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}


std::optional<std::string> AtomicServiceManager::selectQueued() const {
    const AtomicExecutionSnapshot* best = nullptr;
    std::string best_id;
    for (const auto& pair : executions_) {
        const auto& snapshot = pair.second;
        if (snapshot.state != AtomicExecutionState::Queued) continue;
        const auto tool = execution_tools_.find(pair.first);
        if (tool == execution_tools_.end()) continue;
        const auto tool_name = snapshot.envelope.mcp_request.name;
        std::size_t tool_active = 0;
        bool lower_priority_preemptible = false;
        for (const auto& other : executions_) {
            if (other.first == pair.first ||
                other.second.envelope.mcp_request.name != tool_name) {
                continue;
            }
            if (other.second.state == AtomicExecutionState::Running ||
                other.second.state == AtomicExecutionState::Unknown) {
                ++tool_active;
            }
            const auto other_tool = execution_tools_.find(other.first);
            if (other.second.state == AtomicExecutionState::Running &&
                provider_inflight_.count(other.first) == 0 &&
                other_tool != execution_tools_.end() &&
                other_tool->second.policy.supports_preemption &&
                isHigherPriority(
                    snapshot.envelope.runtime.priority,
                    other.second.envelope.runtime.priority)) {
                lower_priority_preemptible = true;
            }
        }
        if (tool_active >= tool->second.policy.max_concurrency &&
            !lower_priority_preemptible) {
            continue;
        }
        const bool resource_busy = std::any_of(
            executions_.begin(), executions_.end(),
            [&pair, &snapshot](const auto& other) {
                return other.first != pair.first &&
                       other.second.resource_key ==
                           snapshot.resource_key &&
                       (other.second.state ==
                            AtomicExecutionState::Running ||
                        other.second.state ==
                            AtomicExecutionState::Unknown);
            });
        if (resource_busy) continue;
        if (!best ||
            std::tie(snapshot.envelope.runtime.priority,
                     snapshot.envelope.runtime.deadline_mono_ns,
                     queue_sequence_.at(pair.first)) <
                std::tie(best->envelope.runtime.priority,
                         best->envelope.runtime.deadline_mono_ns,
                         queue_sequence_.at(best_id))) {
            best = &snapshot;
            best_id = pair.first;
        }
    }
    return best ? std::optional<std::string>(best_id) : std::nullopt;
}

std::optional<std::string> AtomicServiceManager::selectVictim(
    TaskPriority arriving,
    const std::optional<std::string>& tool_name) const {
    const AtomicExecutionSnapshot* victim = nullptr;
    std::string victim_id;
    for (const auto& pair : executions_) {
        const auto& snapshot = pair.second;
        if (snapshot.state != AtomicExecutionState::Running ||
            provider_inflight_.count(pair.first) != 0 ||
            (tool_name &&
             snapshot.envelope.mcp_request.name != *tool_name) ||
            !isHigherPriority(arriving,
                              snapshot.envelope.runtime.priority)) {
            continue;
        }
        const auto tool = execution_tools_.find(pair.first);
        if (tool == execution_tools_.end() ||
            !tool->second.policy.supports_preemption) {
            continue;
        }
        if (!victim ||
            static_cast<std::uint8_t>(
                snapshot.envelope.runtime.priority) >
                static_cast<std::uint8_t>(
                    victim->envelope.runtime.priority)) {
            victim = &snapshot;
            victim_id = pair.first;
        }
    }
    return victim ? std::optional<std::string>(victim_id) : std::nullopt;
}

void AtomicServiceManager::emit(AtomicExecutionSnapshot& snapshot,
                                   const std::string& event_type,
                                   bool safe_point,
                                   bool resource_released) {
    AtomicExecutionEvent event;
    event.event_id = ids_->next("atomic-event");
    event.event_type = event_type;
    event.request_id = snapshot.envelope.runtime.request_id;
    event.trace_id = snapshot.envelope.runtime.trace_id;
    event.plan_id = snapshot.envelope.runtime.plan_id;
    event.pid = snapshot.envelope.runtime.pid;
    event.activation_id = snapshot.envelope.runtime.activation_id;
    event.execution_id = snapshot.envelope.runtime.execution_id;
    event.attempt_no = snapshot.envelope.runtime.attempt_no;
    event.operation_id = snapshot.envelope.runtime.operation_id;
    event.mcp_request_id = snapshot.envelope.mcp_request.id;
    event.tool_name = snapshot.envelope.mcp_request.name;
    event.state = snapshot.state;
    event.fencing_token = snapshot.envelope.runtime.fencing_token;
    event.side_effect_state = snapshot.side_effect_state;
    event.completion_evidence = snapshot.completion_evidence;
    event.call_tool_result = snapshot.result;
    event.error_code = snapshot.error_code;
    event.retryable_hint = snapshot.retryable_hint;
    event.safe_point = safe_point;
    event.resource_released = resource_released;
    event.occurred_at_utc_ms = clock_->utcNowMs();
    events_.push_back(std::move(event));
    if (!storage_directory_.empty() && durability_status_.ok) {
        const auto execution_id =
            snapshot.envelope.runtime.execution_id;
        if (executions_.count(execution_id) != 0) {
            const auto persisted =
                persistCurrentExecutionUnlocked(execution_id);
            if (!persisted.ok) {
                durability_status_ = persisted;
            }
        }
    }
}

}  // namespace master_agent::atomic_service
