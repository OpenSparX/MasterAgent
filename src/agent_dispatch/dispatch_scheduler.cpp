/**
 * @file dispatch_scheduler.cpp
 * @brief Pumps dispatch work and reports scheduler capacity.
 */

#include "include/dispatch_state_rules.h"
#include "include/provider_identity.h"

#include <algorithm>

namespace master_agent::agent_dispatch {

bool AgentDispatch::pumpOne() {

    std::lock_guard<std::mutex> pump_lock(pump_mutex_);
    struct AgentProbe {
        std::string agent_id;
        std::uint64_t agent_epoch = 0;
        std::shared_ptr<sub_agents::ISubAgent> provider;
    };
    struct RunningProvider {
        std::string dispatch_id;
        std::shared_ptr<sub_agents::ISubAgent> provider;
        DispatchTask task;
    };
    struct ProviderObservation {
        std::string dispatch_id;
        std::optional<sub_agents::SubAgentSnapshot> snapshot;
        std::string error_code;
    };
    bool progressed = false;
    std::vector<AgentProbe> probes;
    std::vector<RunningProvider> running;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Expired queued work must never preempt a live lower-priority task.
        // Suspended work already proved resource release and is terminalized;
        // running work becomes UNKNOWN because the provider may have crossed
        // its side-effect boundary.
        for (auto& pair : dispatches_) {
            auto& snapshot = pair.second;
            if (!deadlineExpired(
                    snapshot.task.deadline_mono_ns, *clock_)) {
                continue;
            }
            if (snapshot.state == DispatchState::Queued) {
                if (provider_submitted_.count(pair.first) != 0) {
                    markUnknownUnlocked(
                        pair.first,
                        "DISPATCH_RESULT_AFTER_DEADLINE");
                } else {
                    setStateUnlocked(snapshot,
                                     DispatchState::Failed);
                    snapshot.side_effect_state =
                        SideEffectState::NotStarted;
                    snapshot.error_code =
                        "DISPATCH_DEADLINE_EXPIRED";
                    emit(snapshot, "FAILED", false, true);
                }
                progressed = true;
            } else if (snapshot.state ==
                       DispatchState::Suspended) {
                if (snapshot.side_effect_state ==
                        SideEffectState::Committed ||
                    snapshot.side_effect_state ==
                        SideEffectState::Unknown) {
                    markUnknownUnlocked(
                        pair.first,
                        "DISPATCH_SUSPENDED_DEADLINE_RECONCILIATION_REQUIRED");
                } else {
                    setStateUnlocked(snapshot,
                                     DispatchState::Failed);
                    snapshot.error_code =
                        "DISPATCH_DEADLINE_EXPIRED";
                    emit(snapshot, "FAILED", true, true);
                }
                progressed = true;
            } else if (snapshot.state == DispatchState::Running) {
                markUnknownUnlocked(
                    pair.first, "DISPATCH_RESULT_AFTER_DEADLINE");
                progressed = true;
            }
        }
        for (const auto& pair : agents_) {
            probes.push_back({pair.first,
                              pair.second.manifest.agent_epoch,
                              pair.second.provider});
        }
        for (const auto& pair : dispatches_) {
            if (pair.second.state == DispatchState::Running ||
                (pair.second.state == DispatchState::Queued &&
                 provider_submitted_.count(pair.first) != 0)) {
                running.push_back(
                    {pair.first,
                     dispatch_providers_.at(pair.first),
                     pair.second.task});
            }
        }
    }

    // Health calls are endpoint I/O and therefore never run while holding the
    // registry mutex. The epoch and Provider pointer are revalidated before a
    // result can change routing capacity, closing the replacement race.
    std::vector<std::pair<AgentProbe, bool>> probe_results;
    probe_results.reserve(probes.size());
    for (const auto& probe : probes) {
        bool healthy = false;
        try {
            const auto probe_deadline =
                clock_->monotonicNowNs() + 1'000'000'000LL;
            CallContext health_call{
                CallerModuleId::AgentDispatch,
                "health:" + probe.agent_id,
                "health:" + probe.agent_id,
                "system-agent-dispatch",
                TaskPriority::P1,
                probe_deadline,
                {},
                0,
                "internal-health-probe"};
            healthy = probe.provider->heartbeat(health_call) &&
                      !deadlineExpired(probe_deadline, *clock_);
        } catch (...) {
            healthy = false;
        }
        probe_results.push_back({probe, healthy});
    }

    std::vector<std::shared_ptr<sub_agents::ISubAgent>> providers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool capacity_changed = false;
        for (const auto& result : probe_results) {
            const auto found = agents_.find(result.first.agent_id);
            if (found == agents_.end() ||
                found->second.manifest.agent_epoch !=
                    result.first.agent_epoch ||
                found->second.provider != result.first.provider) {
                continue;
            }
            if (found->second.healthy != result.second) {
                found->second.healthy = result.second;
                capacity_changed = true;
                progressed = true;
            }
            if (result.second) providers.push_back(result.first.provider);
        }
        if (capacity_changed) {
            ++capacity_epoch_;
            recordCapacityUnlocked();
            const auto saved = persistStateUnlocked();
            durable_ = saved.ok;
        }
    }

    // Provider calls intentionally happen without the Dispatch registry lock.
    std::vector<std::shared_ptr<sub_agents::ISubAgent>>
        failed_pumps;
    for (const auto& provider : providers) {
        try {
            progressed = provider->pumpOne() || progressed;
        } catch (...) {
            failed_pumps.push_back(provider);
            progressed = true;
        }
    }
    if (!failed_pumps.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool capacity_changed = false;
        for (const auto& failed : failed_pumps) {
            for (auto& agent : agents_) {
                if (agent.second.provider == failed &&
                    agent.second.healthy) {
                    agent.second.healthy = false;
                    capacity_changed = true;
                }
            }
            for (const auto& dispatch : dispatch_providers_) {
                const auto live = dispatches_.find(dispatch.first);
                if (dispatch.second == failed &&
                    live != dispatches_.end() &&
                    (live->second.state == DispatchState::Running ||
                     live->second.state == DispatchState::Suspended ||
                     (live->second.state == DispatchState::Queued &&
                      provider_submitted_.count(dispatch.first) != 0))) {
                    markUnknownUnlocked(dispatch.first,
                                        "DISPATCH_PROVIDER_PUMP_EXCEPTION");
                } else if (dispatch.second == failed &&
                           live != dispatches_.end() &&
                           live->second.state ==
                               DispatchState::Queued) {
                    auto& snapshot = live->second;
                    setStateUnlocked(snapshot,
                                     DispatchState::Failed);
                    snapshot.side_effect_state =
                        SideEffectState::NotStarted;
                    snapshot.error_code =
                        "DISPATCH_PROVIDER_UNAVAILABLE";
                    emit(snapshot, "FAILED", false, true);
                }
            }
        }
        if (capacity_changed) ++capacity_epoch_;
    }

    std::vector<ProviderObservation> observations;
    for (const auto& item : running) {
        const auto observation_deadline =
            clock_->monotonicNowNs() + 1'000'000'000LL;
        CallContext provider_call{
            CallerModuleId::AgentDispatch, item.task.request_id,
            item.task.trace_id, item.task.principal_id_hash,
            item.task.priority, observation_deadline, {}, 0,
            item.task.authorization_ref};
        try {
            const auto observed =
                item.provider->query(item.dispatch_id, provider_call);
            if (observed.status.ok && observed.value) {
                observations.push_back(
                    {item.dispatch_id, *observed.value, {}});
            } else {
                observations.push_back(
                    {item.dispatch_id, std::nullopt,
                     observed.status.ok
                         ? "DISPATCH_PROVIDER_SNAPSHOT_MISSING"
                         : observed.status.error.code});
            }
        } catch (...) {
            observations.push_back(
                {item.dispatch_id, std::nullopt,
                 "DISPATCH_PROVIDER_QUERY_EXCEPTION"});
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& observation : observations) {
            auto found =
                dispatches_.find(observation.dispatch_id);
            if (found == dispatches_.end() ||
                (found->second.state != DispatchState::Running &&
                 !(found->second.state == DispatchState::Queued &&
                   provider_submitted_.count(
                       observation.dispatch_id) != 0))) {
                continue;
            }
            if (!observation.error_code.empty() ||
                !observation.snapshot) {
                markUnknownUnlocked(
                    observation.dispatch_id,
                    observation.error_code.empty()
                        ? "DISPATCH_PROVIDER_SNAPSHOT_MISSING"
                        : observation.error_code);
                continue;
            }
            auto& snapshot = found->second;
            const auto& provider_snapshot = *observation.snapshot;
            if (deadlineExpired(
                    snapshot.task.deadline_mono_ns, *clock_)) {
                markUnknownUnlocked(
                    observation.dispatch_id,
                    "DISPATCH_RESULT_AFTER_DEADLINE");
                continue;
            }
            if (!providerIdentityMatches(
                    provider_snapshot, snapshot)) {
                markUnknownUnlocked(
                    observation.dispatch_id,
                    "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
                continue;
            }
            if (provider_snapshot.state ==
                    sub_agents::SubAgentState::Accepted ||
                provider_snapshot.state ==
                    sub_agents::SubAgentState::Queued) {
                // Provider ownership is established, but acceptance alone is
                // not STARTED evidence.
                continue;
            }
            if (provider_snapshot.state ==
                sub_agents::SubAgentState::Running) {
                if (provider_snapshot.resource_released ||
                    !isValidSideEffectState(
                        provider_snapshot.side_effect_state) ||
                    provider_snapshot.side_effect_state ==
                        SideEffectState::Unknown) {
                    markUnknownUnlocked(
                        observation.dispatch_id,
                        "DISPATCH_PROVIDER_RUNNING_PROOF_INVALID");
                    continue;
                }
                if (snapshot.state != DispatchState::Running) {
                    setStateUnlocked(snapshot,
                                     DispatchState::Running);
                    snapshot.side_effect_state =
                        provider_snapshot.side_effect_state;
                    emit(snapshot, "STARTED");
                }
                continue;
            }
            if (provider_snapshot.state ==
                sub_agents::SubAgentState::Suspended) {
                if (provider_snapshot.checkpoint_ref.empty() ||
                    !provider_snapshot.resource_released ||
                    !validSuspendedSideEffect(
                        provider_snapshot.side_effect_state)) {
                    markUnknownUnlocked(
                        observation.dispatch_id,
                        "DISPATCH_PROVIDER_SUSPEND_PROOF_INVALID");
                    continue;
                }
                setStateUnlocked(snapshot,
                                 DispatchState::Suspended);
                snapshot.checkpoint_ref =
                    provider_snapshot.checkpoint_ref;
                snapshot.side_effect_state =
                    provider_snapshot.side_effect_state;
                provider_submitted_.erase(
                    observation.dispatch_id);
                emit(snapshot, "SUSPENDED", true, true);
                continue;
            }
            const bool provider_terminal =
                provider_snapshot.state ==
                    sub_agents::SubAgentState::Succeeded ||
                provider_snapshot.state ==
                    sub_agents::SubAgentState::Failed ||
                provider_snapshot.state ==
                    sub_agents::SubAgentState::Cancelled;
            if (provider_terminal &&
                !provider_snapshot.resource_released) {
                markUnknownUnlocked(
                    observation.dispatch_id,
                    "DISPATCH_PROVIDER_RELEASE_UNCONFIRMED");
                continue;
            }
            const bool success_side_effect_confirmed =
                provider_snapshot.side_effect_state ==
                    SideEffectState::Committed ||
                provider_snapshot.side_effect_state ==
                    SideEffectState::NotApplicable;
            if ((provider_snapshot.state ==
                     sub_agents::SubAgentState::Succeeded &&
                 !success_side_effect_confirmed) ||
                ((provider_snapshot.state ==
                      sub_agents::SubAgentState::Failed ||
                  provider_snapshot.state ==
                      sub_agents::SubAgentState::Cancelled) &&
                 provider_snapshot.side_effect_state ==
                     SideEffectState::Unknown)) {
                markUnknownUnlocked(
                    observation.dispatch_id,
                    "DISPATCH_PROVIDER_SIDE_EFFECT_UNCONFIRMED");
                continue;
            }
            if (provider_snapshot.state ==
                sub_agents::SubAgentState::Succeeded) {
                if (snapshot.state != DispatchState::Running) {
                    setStateUnlocked(snapshot,
                                     DispatchState::Running);
                    emit(snapshot, "STARTED");
                }
                setStateUnlocked(snapshot,
                                 DispatchState::Succeeded);
                snapshot.result = provider_snapshot.result;
                snapshot.side_effect_state =
                    provider_snapshot.side_effect_state;
                snapshot.retryable_hint = false;
                provider_submitted_.erase(
                    observation.dispatch_id);
                emit(snapshot, "SUCCEEDED", false, true);
            } else if (provider_snapshot.state ==
                       sub_agents::SubAgentState::Failed) {
                if (snapshot.state != DispatchState::Running) {
                    setStateUnlocked(snapshot,
                                     DispatchState::Running);
                    emit(snapshot, "STARTED");
                }
                setStateUnlocked(snapshot,
                                 DispatchState::Failed);
                snapshot.error_code = provider_snapshot.error_code;
                snapshot.side_effect_state =
                    provider_snapshot.side_effect_state;
                snapshot.retryable_hint =
                    provider_snapshot.retryable_hint;
                provider_submitted_.erase(
                    observation.dispatch_id);
                emit(snapshot, "FAILED", false, true);
            } else if (provider_snapshot.state ==
                       sub_agents::SubAgentState::Cancelled) {
                if (snapshot.state != DispatchState::Running) {
                    setStateUnlocked(snapshot,
                                     DispatchState::Running);
                    emit(snapshot, "STARTED");
                }
                setStateUnlocked(snapshot,
                                 DispatchState::Cancelled);
                snapshot.error_code =
                    provider_snapshot.error_code;
                snapshot.side_effect_state =
                    provider_snapshot.side_effect_state;
                snapshot.retryable_hint = false;
                provider_submitted_.erase(
                    observation.dispatch_id);
                emit(snapshot, "CANCELLED", false, true);
            }
        }
    }

    std::optional<std::string> queued_id;
    std::optional<std::string> victim_id;
    std::shared_ptr<sub_agents::ISubAgent> selected_provider;
    DispatchTask arriving_task;
    std::uint64_t preempt_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queued_id = selectQueued();
        if (queued_id) {
            auto& queued = dispatches_.at(*queued_id);
            selected_provider =
                dispatch_providers_.at(*queued_id);
            std::size_t active_for_agent = 0;
            for (const auto& pair : dispatches_) {
                if (pair.second.route.agent_id == queued.route.agent_id &&
                    (pair.second.state == DispatchState::Running ||
                     pair.second.state == DispatchState::Unknown ||
                     provider_submitted_.count(pair.first) != 0)) {
                    ++active_for_agent;
                }
            }
            const auto capacity =
                agents_.at(queued.route.agent_id).manifest.max_concurrency;
            if (active_for_agent >= capacity) {
                victim_id =
                    selectVictim(queued.route.agent_id, queued.task.priority);
                if (victim_id) {
                    arriving_task = queued.task;
                    preempt_epoch = ++control_sequence_;
                }
            }
        }
    }
    if (victim_id && queued_id) {
        const auto result = requestPreemptInternal(
            *victim_id, arriving_task.priority, preempt_epoch,
            arriving_task.deadline_mono_ns,
            arriving_task.authorization_ref);
        progressed = result.ok || progressed;
    }

    if (queued_id) {
        DispatchSnapshot local;
        bool capacity = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local = dispatches_.at(*queued_id);
            std::size_t active = 0;
            for (const auto& pair : dispatches_) {
                if (pair.second.route.agent_id == local.route.agent_id &&
                    (pair.second.state == DispatchState::Running ||
                     pair.second.state == DispatchState::Unknown ||
                     provider_submitted_.count(pair.first) != 0)) {
                    ++active;
                }
            }
            capacity =
                active <
                agents_.at(local.route.agent_id).manifest.max_concurrency;
            selected_provider =
                dispatch_providers_.at(*queued_id);
        }
        if (capacity) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto found = dispatches_.find(*queued_id);
                if (found == dispatches_.end() ||
                    found->second.state != DispatchState::Queued) {
                    return true;
                }
                if (deadlineExpired(
                        found->second.task.deadline_mono_ns,
                        *clock_)) {
                    setStateUnlocked(found->second,
                                     DispatchState::Failed);
                    found->second.side_effect_state =
                        SideEffectState::NotStarted;
                    found->second.error_code =
                        "DISPATCH_DEADLINE_EXPIRED";
                    emit(found->second, "FAILED", false, true);
                    return true;
                }
                // Reserve the provider credit before crossing the external
                // submit boundary. A throw/late result retains this marker
                // because the Provider may already own the execution.
                provider_submitted_.insert(*queued_id);
            }
            sub_agents::SubAgentExecutionRequest request;
            request.dispatch_id = local.dispatch_id;
            request.request_id = local.task.request_id;
            request.pid = local.task.pid;
            request.activation_id = local.task.activation_id;
            request.attempt_no = local.task.attempt_no;
            request.operation_id = local.task.operation_id;
            request.execution_id = local.task.execution_id;
            request.agent_id = local.route.agent_id;
            request.agent_epoch = local.route.agent_epoch;
            request.manifest_digest = local.route.manifest_digest;
            request.lease_id = local.route.lease_id;
            request.action = local.task.action;
            request.params = local.task.params;
            request.capability_digest =
                local.task.capability_digest;
            request.expected_output_schema_version =
                local.task.expected_output_schema_version;
            request.priority = local.task.priority;
            request.deadline_mono_ns = local.task.deadline_mono_ns;
            request.fencing_token = local.task.fencing_token;
            request.trace_id = local.task.trace_id;
            request.principal_id_hash =
                local.task.principal_id_hash;
            request.authorization_ref =
                local.task.authorization_ref;
            CallContext provider_call{
                CallerModuleId::AgentDispatch, local.task.request_id,
                local.task.trace_id, local.task.principal_id_hash,
                local.task.priority, local.task.deadline_mono_ns, {}, 0,
                local.task.authorization_ref};
            sub_agents::SubAgentAcceptance accepted;
            bool submit_threw = false;
            try {
                accepted =
                    selected_provider->submit(request, provider_call);
            } catch (...) {
                submit_threw = true;
            }
            auto accepted_observation =
                Result<sub_agents::SubAgentSnapshot>::Failure(
                    Status::Error(
                        "agent_dispatch",
                        "DISPATCH_PROVIDER_ACCEPTED_NOT_OBSERVED",
                        "provider acceptance has no running proof", true));
            if (!submit_threw && accepted.accepted &&
                accepted.dispatch_id == local.dispatch_id) {
                auto observation_call = provider_call;
                observation_call.deadline_mono_ns =
                    clock_->monotonicNowNs() + 1'000'000'000LL;
                try {
                    accepted_observation =
                        selected_provider->query(
                            local.dispatch_id,
                            observation_call);
                } catch (...) {
                    // The accepted ticket remains provider-owned and will be
                    // observed by a later pump/reconciliation pass.
                }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*queued_id);
            if (found == dispatches_.end() ||
                found->second.state != DispatchState::Queued ||
                found->second.route.agent_epoch !=
                    local.route.agent_epoch ||
                found->second.route.lease_id != local.route.lease_id) {
                return true;
            }
            auto& live = found->second;
            const bool submit_result_after_deadline =
                deadlineExpired(
                    live.task.deadline_mono_ns, *clock_);
            if (submit_result_after_deadline &&
                (submit_threw || accepted.accepted)) {
                markUnknownUnlocked(
                    *queued_id,
                    "DISPATCH_SUBMIT_RESULT_AFTER_DEADLINE");
                progressed = true;
            } else if (submit_result_after_deadline) {
                provider_submitted_.erase(*queued_id);
                setStateUnlocked(live, DispatchState::Failed);
                live.side_effect_state =
                    SideEffectState::NotStarted;
                live.error_code =
                    "DISPATCH_DEADLINE_EXPIRED";
                emit(live, "FAILED", false, true);
                progressed = true;
            } else if (submit_threw) {
                markUnknownUnlocked(
                    *queued_id,
                    "DISPATCH_PROVIDER_SUBMIT_EXCEPTION");
                progressed = true;
            } else if (accepted.accepted &&
                       accepted.dispatch_id !=
                           local.dispatch_id) {
                markUnknownUnlocked(
                    *queued_id,
                    "DISPATCH_PROVIDER_ACCEPTANCE_IDENTITY_MISMATCH");
                progressed = true;
            } else if (accepted.accepted) {
                emit(live, "PROVIDER_ACCEPTED");
                if (accepted_observation.status.ok &&
                    accepted_observation.value) {
                    if (!providerIdentityMatches(
                            *accepted_observation.value, live)) {
                        markUnknownUnlocked(
                            *queued_id,
                            "DISPATCH_PROVIDER_IDENTITY_MISMATCH");
                    } else if (
                        accepted_observation.value->state ==
                            sub_agents::SubAgentState::Running &&
                        !accepted_observation.value->
                            resource_released &&
                        isValidSideEffectState(
                            accepted_observation.value->
                                side_effect_state) &&
                        accepted_observation.value->
                                side_effect_state !=
                            SideEffectState::Unknown) {
                        setStateUnlocked(live,
                                         DispatchState::Running);
                        live.side_effect_state =
                            accepted_observation.value->
                                side_effect_state;
                        emit(live, "STARTED");
                    }
                }
                progressed = true;
            } else {
                provider_submitted_.erase(*queued_id);
                setStateUnlocked(live, DispatchState::Failed);
                live.side_effect_state =
                    SideEffectState::NotStarted;
                if (accepted.reject_code ==
                    "SUB_AGENT_CAPACITY_EXHAUSTED") {
                    live.error_code =
                        "DISPATCH_PROVIDER_CAPACITY_DRIFT";
                    const auto agent =
                        agents_.find(live.route.agent_id);
                    if (agent != agents_.end() &&
                        agent->second.healthy) {
                        agent->second.healthy = false;
                        ++capacity_epoch_;
                    }
                } else {
                    live.error_code = accepted.reject_code;
                }
                emit(live, "FAILED", false, true);
                progressed = true;
            }
        }
    }

    // Restore suspended work after all higher-priority queued work drains.
    std::optional<std::string> restore_id;
    std::shared_ptr<sub_agents::ISubAgent> restore_provider;
    std::string checkpoint;
    std::uint64_t next_control_epoch = 0;
    DispatchTask restore_task;
    AgentRouteDecision restore_route;
    DispatchSnapshot restore_expected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!selectQueued()) {
            for (auto& pair : dispatches_) {
                if (pair.second.state != DispatchState::Suspended) continue;
                bool busy = false;
                for (const auto& other : dispatches_) {
                    if (other.second.route.agent_id ==
                            pair.second.route.agent_id &&
                        (other.second.state == DispatchState::Running ||
                         other.second.state == DispatchState::Unknown ||
                         provider_submitted_.count(other.first) != 0)) {
                        busy = true;
                    }
                }
                if (!busy) {
                    restore_id = pair.first;
                    restore_provider =
                        dispatch_providers_.at(pair.first);
                    checkpoint = pair.second.checkpoint_ref;
                    restore_task = pair.second.task;
                    restore_route = pair.second.route;
                    restore_expected = pair.second;
                    next_control_epoch = ++control_sequence_;
                    break;
                }
            }
        }
    }
    if (restore_id) {
        if (deadlineExpired(
                restore_task.deadline_mono_ns, *clock_)) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*restore_id);
            if (found != dispatches_.end() &&
                found->second.state == DispatchState::Suspended) {
                if (found->second.side_effect_state ==
                        SideEffectState::Committed ||
                    found->second.side_effect_state ==
                        SideEffectState::Unknown) {
                    markUnknownUnlocked(
                        *restore_id,
                        "DISPATCH_SUSPENDED_DEADLINE_RECONCILIATION_REQUIRED");
                } else {
                    setStateUnlocked(found->second,
                                     DispatchState::Failed);
                    found->second.error_code =
                        "DISPATCH_DEADLINE_EXPIRED";
                    emit(found->second, "FAILED", true, true);
                }
            }
            return true;
        }
        CallContext provider_call{
            CallerModuleId::AgentDispatch, restore_task.request_id,
            restore_task.trace_id, restore_task.principal_id_hash,
            restore_task.priority, restore_task.deadline_mono_ns, {}, 0,
            restore_task.authorization_ref};
        Status restored = Status::Error(
            "agent_dispatch", "DISPATCH_PROVIDER_RESTORE_EXCEPTION",
            "agent provider did not return from restore", true,
            SideEffectState::Unknown);
        bool restore_threw = false;
        try {
            restored = restore_provider->restore(
                *restore_id, checkpoint, next_control_epoch,
                provider_call);
        } catch (...) {
            restore_threw = true;
        }
        auto restore_observation =
            Result<sub_agents::SubAgentSnapshot>::Failure(
                Status::Error(
                    "agent_dispatch",
                    "DISPATCH_RESTORE_SNAPSHOT_MISSING",
                    "restored provider state was not observed",
                    true, SideEffectState::Unknown));
        if (!restore_threw && restored.ok) {
            auto query_call = provider_call;
            query_call.deadline_mono_ns =
                clock_->monotonicNowNs() + 1'000'000'000LL;
            try {
                restore_observation =
                    restore_provider->query(
                        *restore_id, query_call);
            } catch (...) {
                restore_observation =
                    Result<sub_agents::SubAgentSnapshot>::Failure(
                        Status::Error(
                            "agent_dispatch",
                            "DISPATCH_RESTORE_QUERY_EXCEPTION",
                            "provider restore evidence was unavailable",
                            true, SideEffectState::Unknown));
            }
        }
        const bool restore_result_after_deadline =
            deadlineExpired(
                restore_task.deadline_mono_ns, *clock_);
        if (restore_result_after_deadline &&
            (restore_threw || restored.ok ||
             restored.error.side_effect_state ==
                 SideEffectState::Unknown)) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*restore_id);
            if (found != dispatches_.end() &&
                found->second.state == DispatchState::Suspended &&
                found->second.route.agent_epoch ==
                    restore_route.agent_epoch &&
                found->second.route.lease_id ==
                    restore_route.lease_id) {
                if (!restore_threw && restored.ok) {
                    found->second.control_epoch =
                        next_control_epoch;
                }
                markUnknownUnlocked(
                    *restore_id,
                    "DISPATCH_RESTORE_RESULT_AFTER_DEADLINE");
                progressed = true;
            }
        } else if (restore_result_after_deadline) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*restore_id);
            if (found != dispatches_.end() &&
                found->second.state == DispatchState::Suspended) {
                setStateUnlocked(found->second,
                                 DispatchState::Failed);
                found->second.error_code =
                    "DISPATCH_DEADLINE_EXPIRED";
                emit(found->second, "FAILED", false, true);
                progressed = true;
            }
        } else if (restore_threw ||
            (!restored.ok &&
             restored.error.side_effect_state ==
                 SideEffectState::Unknown)) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*restore_id);
            if (found != dispatches_.end() &&
                found->second.state == DispatchState::Suspended &&
                found->second.route.agent_epoch ==
                    restore_route.agent_epoch &&
                found->second.route.lease_id ==
                    restore_route.lease_id) {
                found->second.control_epoch =
                    next_control_epoch;
                markUnknownUnlocked(
                    *restore_id,
                    restore_threw
                        ? "DISPATCH_PROVIDER_RESTORE_EXCEPTION"
                        : "DISPATCH_PROVIDER_RESTORE_UNCONFIRMED");
                progressed = true;
            }
        } else if (restored.ok &&
                   (!restore_observation.status.ok ||
                    !restore_observation.value ||
                    !providerIdentityMatches(
                        *restore_observation.value,
                        restore_expected,
                        next_control_epoch) ||
                    restore_observation.value->state !=
                        sub_agents::SubAgentState::Running ||
                    restore_observation.value->resource_released ||
                    restore_observation.value->checkpoint_ref !=
                        checkpoint ||
                    restore_observation.value->side_effect_state !=
                        restore_expected.side_effect_state)) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*restore_id);
            if (found != dispatches_.end() &&
                found->second.state == DispatchState::Suspended &&
                found->second.route.agent_epoch ==
                    restore_route.agent_epoch &&
                found->second.route.lease_id ==
                    restore_route.lease_id) {
                markUnknownUnlocked(
                    *restore_id,
                    "DISPATCH_RESTORE_PROOF_INVALID");
                progressed = true;
            }
        } else if (restored.ok) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = dispatches_.find(*restore_id);
            if (found == dispatches_.end() ||
                found->second.state != DispatchState::Suspended ||
                found->second.route.agent_epoch !=
                    restore_route.agent_epoch ||
                found->second.route.lease_id !=
                    restore_route.lease_id) {
                return true;
            }
            auto& snapshot = found->second;
            snapshot.control_epoch = next_control_epoch;
            setStateUnlocked(snapshot, DispatchState::Running);
            provider_submitted_.insert(*restore_id);
            snapshot.side_effect_state =
                restore_observation.value->side_effect_state;
            emit(snapshot, "STARTED");
            progressed = true;
        }
    }
    return progressed;
}

Status AgentDispatch::runUntilIdle(std::size_t max_steps) {
    for (std::size_t i = 0; i < max_steps; ++i) {
        if (!pumpOne()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool pending = std::any_of(
                dispatches_.begin(), dispatches_.end(),
                [](const auto& pair) {
                    return pair.second.state == DispatchState::Queued ||
                           pair.second.state == DispatchState::Running ||
                           pair.second.state == DispatchState::Suspended;
                });
            return pending
                       ? Status::Error(
                             "agent_dispatch", "DISPATCH_STALLED",
                             "non-terminal dispatch cannot make progress",
                             true)
                       : Status::Ok();
        }
    }
    return Status::Error("agent_dispatch", "DISPATCH_PUMP_LIMIT",
                         "dispatch did not become idle");
}

std::vector<DispatchEvent> AgentDispatch::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

AgentDispatchCapacity AgentDispatch::getCapacity(
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    AgentDispatchCapacity capacity;
    if (!hasHostModuleIdentity(
            call, CallerModuleId::TaskOrchestrationEngine) ||
        !clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        capacity.health_state = "CALLER_NOT_ALLOWED";
        return capacity;
    }
    capacity = capacityUnlocked();
    if (call.priority == TaskPriority::P0) {
        std::uint32_t active_total = 0;
        for (const auto& pair : dispatches_) {
            if (pair.second.state == DispatchState::Queued ||
                pair.second.state == DispatchState::Running ||
                pair.second.state == DispatchState::Unknown ||
                provider_submitted_.count(pair.first) != 0) {
                ++active_total;
            }
        }
        capacity.available_credits =
            capacity.max_inflight > active_total
                ? capacity.max_inflight - active_total
                : 0;
    }
    return capacity;
}


}  // namespace master_agent::agent_dispatch
