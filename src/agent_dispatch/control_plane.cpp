/**
 * @file control_plane.cpp
 * @brief Implements Agent Dispatch control, subscription, registry, health, and persistence APIs.
 *
 * Design trace: Sub-Agent Dispatch Engine Detailed Design, sections 3, 4,
 * 5.6, 6, 7, 9, and 11. The implementation is single-process; persistence
 * protects registry epochs, dispatch ownership, and event delivery cursors.
 */

#include "include/dispatch_access_control.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace master_agent::agent_dispatch {
namespace {

bool orchestratorControl(const CallContext& call) {
    return hasHostModuleIdentity(
               call, CallerModuleId::TaskOrchestrationEngine) &&
           isValidTaskPriority(call.priority) &&
           !call.request_id.empty() && !call.trace_id.empty() &&
           !call.principal_id_hash.empty() &&
           call.deadline_mono_ns > 0;
}

bool terminal(DispatchState state) {
    return state == DispatchState::Succeeded ||
           state == DispatchState::Failed ||
           state == DispatchState::Cancelled;
}

std::string capabilitySnapshotDigest(
    std::uint64_t generation,
    const std::vector<sub_agents::AgentManifest>& manifests) {
    nlohmann::json encoded = {
        {"generation", generation},
        {"manifests", nlohmann::json::array()}};
    for (const auto& manifest : manifests) {
        encoded["manifests"].push_back({
            {"agent_id", manifest.agent_id},
            {"agent_epoch", manifest.agent_epoch},
            {"manifest_digest", manifest.manifest_digest},
            {"capability_version", manifest.capability_version},
            {"capabilities", manifest.capabilities},
            {"required_permissions", manifest.required_permissions},
            {"required_atomic_tools", manifest.required_atomic_tools},
            {"max_concurrency", manifest.max_concurrency},
            {"reserved_p0_slots", manifest.reserved_p0_slots},
            {"supports_safe_point_preemption",
             manifest.supports_safe_point_preemption},
            {"prompt_profile_id", manifest.prompt_profile_id},
            {"model_profile_id", manifest.model_profile_id}});
    }
    return secureDigest(encoded.dump());
}

}  // namespace

AgentDispatch::AgentDispatch(
    std::filesystem::path storage_directory,
    std::shared_ptr<IRuntimeClock> clock,
    std::shared_ptr<IdGenerator> ids)
    : storage_directory_(std::move(storage_directory)),
      clock_(std::move(clock)),
      ids_(std::move(ids)),
      durable_(false) {}

Status AgentDispatch::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_directory_.empty()) {
        durable_ = true;
        return Status::Ok();
    }
    std::error_code error;
    std::filesystem::create_directories(storage_directory_, error);
    if (error) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_STORAGE_UNAVAILABLE",
            "dispatch storage directory could not be created", true);
    }
    const auto recovered = recoverStateUnlocked();
    durable_ = recovered.ok;
    return recovered;
}

Result<AgentCapabilitySnapshot>
AgentDispatch::snapshotAgentCapabilities(
    const AgentCapabilityQuery& query,
    const CallContext& call) const {
    if (!canQuery(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<AgentCapabilitySnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CAPABILITY_CALLER_NOT_ALLOWED",
            "capability snapshot caller is not authorized"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    AgentCapabilitySnapshot snapshot;
    snapshot.registry_generation = capacity_epoch_;
    for (const auto& [agent_id, record] : agents_) {
        if ((query.agent_id && agent_id != *query.agent_id) ||
            (query.capability &&
             std::find(record.manifest.capabilities.begin(),
                       record.manifest.capabilities.end(),
                       *query.capability) ==
                 record.manifest.capabilities.end())) {
            continue;
        }
        snapshot.manifests.push_back(record.manifest);
    }
    snapshot.snapshot_digest = capabilitySnapshotDigest(
        snapshot.registry_generation, snapshot.manifests);
    if (query.expected_snapshot_digest &&
        *query.expected_snapshot_digest != snapshot.snapshot_digest) {
        return Result<AgentCapabilitySnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CAPABILITY_DIGEST_CONFLICT",
            "capability snapshot digest changed"));
    }
    return Result<AgentCapabilitySnapshot>::Success(std::move(snapshot));
}

DispatchControlAck AgentDispatch::updateDispatchPriority(
    const std::string& dispatch_id,
    TaskPriority effective_priority,
    const std::string& inheritance_source,
    std::uint64_t control_epoch,
    const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!orchestratorControl(call) || !clock_ ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        !isValidTaskPriority(effective_priority) ||
        inheritance_source.empty() || control_epoch == 0) {
        return {false, false, 0, "DISPATCH_PRIORITY_UPDATE_INVALID"};
    }
    const auto found = dispatches_.find(dispatch_id);
    if (found == dispatches_.end()) {
        return {false, false, 0, "DISPATCH_NOT_FOUND"};
    }
    auto& snapshot = found->second;
    if (call.request_id != snapshot.task.request_id ||
        call.trace_id != snapshot.task.trace_id ||
        call.principal_id_hash != snapshot.task.principal_id_hash) {
        return {false, false, snapshot.control_epoch,
                "DISPATCH_CONTROL_IDENTITY_MISMATCH"};
    }
    if (effective_priority == TaskPriority::P0 &&
        snapshot.task.authorization_ref.rfind(
            "trusted-safety:", 0) != 0) {
        return {false, false, snapshot.control_epoch,
                "DISPATCH_P0_AUTHORIZATION_REQUIRED"};
    }
    if (control_epoch < snapshot.control_epoch) {
        return {false, false, snapshot.control_epoch,
                "DISPATCH_CONTROL_EPOCH_STALE"};
    }
    if (control_epoch == snapshot.control_epoch) {
        return {snapshot.task.priority == effective_priority, true,
                snapshot.control_epoch,
                snapshot.task.priority == effective_priority
                    ? std::string{}
                    : "DISPATCH_CONTROL_EPOCH_CONFLICT"};
    }
    if (terminal(snapshot.state)) {
        return {false, false, snapshot.control_epoch,
                "DISPATCH_TERMINAL"};
    }
    snapshot.task.priority = effective_priority;
    snapshot.control_epoch = control_epoch;
    emit(snapshot, "PRIORITY_UPDATED");
    return {durable_, false, control_epoch,
            durable_ ? std::string{} : "DISPATCH_DURABILITY_FAILED"};
}

DispatchControlAck AgentDispatch::restoreDispatch(
    const std::string& dispatch_id,
    const std::string& checkpoint_ref,
    std::uint64_t control_epoch,
    const CallContext& call) {
    if (!orchestratorControl(call) || !clock_ || checkpoint_ref.empty() ||
        control_epoch == 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return {false, false, 0, "DISPATCH_RESTORE_INVALID"};
    }
    std::shared_ptr<sub_agents::ISubAgent> provider;
    DispatchTask task;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = dispatches_.find(dispatch_id);
        if (found == dispatches_.end()) {
            return {false, false, 0, "DISPATCH_NOT_FOUND"};
        }
        if (found->second.state != DispatchState::Suspended ||
            found->second.checkpoint_ref != checkpoint_ref ||
            control_epoch <= found->second.control_epoch ||
            call.request_id != found->second.task.request_id ||
            call.trace_id != found->second.task.trace_id ||
            call.principal_id_hash !=
                found->second.task.principal_id_hash) {
            return {false, false, found->second.control_epoch,
                    "DISPATCH_RESTORE_CONFLICT"};
        }
        const auto provider_it = dispatch_providers_.find(dispatch_id);
        if (provider_it == dispatch_providers_.end()) {
            return {false, false, found->second.control_epoch,
                    "DISPATCH_PROVIDER_UNAVAILABLE"};
        }
        provider = provider_it->second;
        task = found->second.task;
    }
    const auto provider_call = makeChildCallContext(
        call, CallerModuleId::AgentDispatch);
    Status restored;
    try {
        restored = provider->restore(
            dispatch_id, checkpoint_ref, control_epoch, provider_call);
    } catch (...) {
        restored = Status::Error(
            "agent_dispatch", "DISPATCH_RESTORE_PROVIDER_EXCEPTION",
            "agent provider threw during restore", false,
            SideEffectState::Unknown);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = dispatches_.find(dispatch_id);
    if (!restored.ok) {
        if (restored.error.side_effect_state == SideEffectState::Unknown) {
            markUnknownUnlocked(dispatch_id, restored.error.code);
        }
        return {false, false,
                found == dispatches_.end() ? 0 : found->second.control_epoch,
                restored.error.code};
    }
    if (found == dispatches_.end() ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        if (found != dispatches_.end()) {
            markUnknownUnlocked(
                dispatch_id, "DISPATCH_RESTORE_RESULT_AFTER_DEADLINE");
        }
        return {false, false, control_epoch,
                "DISPATCH_RESTORE_RESULT_AFTER_DEADLINE"};
    }
    found->second.control_epoch = control_epoch;
    setStateUnlocked(found->second, DispatchState::Queued);
    emit(found->second, "RESTORED");
    return {durable_, false, control_epoch,
            durable_ ? std::string{} : "DISPATCH_DURABILITY_FAILED"};
}

DispatchControlAck AgentDispatch::cancelDispatch(
    const std::string& dispatch_id,
    const std::string& reason,
    std::uint64_t control_epoch,
    std::int64_t deadline_mono_ns,
    const CallContext& call) {
    if (!orchestratorControl(call) || !clock_ || reason.empty() ||
        control_epoch == 0 || deadline_mono_ns != call.deadline_mono_ns ||
        deadlineExpired(deadline_mono_ns, *clock_)) {
        return {false, false, 0, "DISPATCH_CANCEL_INVALID"};
    }
    std::shared_ptr<sub_agents::ISubAgent> provider;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = dispatches_.find(dispatch_id);
        if (found == dispatches_.end()) {
            return {false, false, 0, "DISPATCH_NOT_FOUND"};
        }
        if (control_epoch < found->second.control_epoch) {
            return {false, false, found->second.control_epoch,
                    "DISPATCH_CONTROL_EPOCH_STALE"};
        }
        if (terminal(found->second.state)) {
            return {true, true, found->second.control_epoch, {}};
        }
        if (hasActiveChildInvocationUnlocked(dispatch_id)) {
            return {false, false, found->second.control_epoch,
                    "DISPATCH_CHILD_INVOCATION_ACTIVE"};
        }
        const auto provider_it = dispatch_providers_.find(dispatch_id);
        if (provider_it != dispatch_providers_.end()) provider = provider_it->second;
        if (!provider_submitted_.count(dispatch_id) &&
            found->second.state == DispatchState::Queued) {
            auto& snapshot = found->second;
            snapshot.control_epoch = control_epoch;
            snapshot.error_code = reason;
            snapshot.side_effect_state = SideEffectState::ConfirmedNotExecuted;
            setStateUnlocked(snapshot, DispatchState::Cancelled);
            emit(snapshot, "CANCELLED", true, true);
            return {durable_, false, control_epoch,
                    durable_ ? std::string{} : "DISPATCH_DURABILITY_FAILED"};
        }
    }
    if (!provider) {
        std::lock_guard<std::mutex> lock(mutex_);
        markUnknownUnlocked(dispatch_id, "DISPATCH_CANCEL_PROVIDER_UNAVAILABLE");
        return {false, false, control_epoch,
                "DISPATCH_CANCEL_PROVIDER_UNAVAILABLE"};
    }
    Status cancelled;
    try {
        cancelled = provider->cancel(
            dispatch_id, control_epoch,
            makeChildCallContext(call, CallerModuleId::AgentDispatch));
    } catch (...) {
        cancelled = Status::Error(
            "agent_dispatch", "DISPATCH_CANCEL_PROVIDER_EXCEPTION",
            "agent provider threw during cancellation", false,
            SideEffectState::Unknown);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = dispatches_.find(dispatch_id);
    if (!cancelled.ok || found == dispatches_.end() ||
        deadlineExpired(deadline_mono_ns, *clock_)) {
        if (found != dispatches_.end()) {
            markUnknownUnlocked(dispatch_id,
                !cancelled.ok ? cancelled.error.code
                              : "DISPATCH_CANCEL_RESULT_AFTER_DEADLINE");
        }
        return {false, false, control_epoch,
                !cancelled.ok ? cancelled.error.code
                              : "DISPATCH_CANCEL_RESULT_AFTER_DEADLINE"};
    }
    auto& snapshot = found->second;
    snapshot.control_epoch = control_epoch;
    snapshot.error_code = reason;
    snapshot.side_effect_state = SideEffectState::ConfirmedNotExecuted;
    setStateUnlocked(snapshot, DispatchState::Cancelled);
    provider_submitted_.erase(dispatch_id);
    emit(snapshot, "CANCELLED", true, true);
    return {durable_, false, control_epoch,
            durable_ ? std::string{} : "DISPATCH_DURABILITY_FAILED"};
}

Result<DispatchEventPage> AgentDispatch::subscribeEvents(
    const DispatchEventSubscription& request,
    const CallContext& call) const {
    if (!orchestratorControl(call) || request.consumer_id.empty() ||
        request.consumer_epoch == 0 || request.max_events == 0 ||
        request.max_events > 1000) {
        return Result<DispatchEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_SUBSCRIPTION_INVALID",
            "event subscription is invalid"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (request.cursor > events_.size()) {
        return Result<DispatchEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_CURSOR_INVALID",
            "event cursor exceeds the outbox"));
    }
    const auto ack = event_acks_.find(request.consumer_id);
    if (ack != event_acks_.end() &&
        request.consumer_epoch < ack->second.first) {
        return Result<DispatchEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CONSUMER_EPOCH_STALE",
            "event consumer epoch is stale"));
    }
    DispatchEventPage page;
    auto cursor = static_cast<std::size_t>(request.cursor);
    for (; cursor < events_.size() &&
           page.events.size() < request.max_events; ++cursor) {
        if (request.dispatch_id &&
            events_[cursor].dispatch_id != *request.dispatch_id) continue;
        page.events.push_back(events_[cursor]);
    }
    page.next_cursor = cursor;
    page.has_more = cursor < events_.size();
    return Result<DispatchEventPage>::Success(std::move(page));
}

Status AgentDispatch::ackDispatchEvent(
    const DispatchEventAck& ack,
    const CallContext& call) {
    if (!orchestratorControl(call) || ack.consumer_id.empty() ||
        ack.consumer_epoch == 0) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_ACK_INVALID",
            "event acknowledgement is invalid");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (ack.cursor > events_.size()) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_ACK_INVALID",
            "event acknowledgement cursor exceeds the outbox");
    }
    auto& current = event_acks_[ack.consumer_id];
    if (ack.consumer_epoch < current.first ||
        (ack.consumer_epoch == current.first &&
         ack.cursor < current.second)) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_ACK_STALE",
            "event acknowledgement rolled back");
    }
    current = {ack.consumer_epoch, ack.cursor};
    return persistStateUnlocked();
}

Status AgentDispatch::unregisterAgent(
    const std::string& agent_id,
    std::uint64_t expected_epoch,
    std::int64_t drain_deadline_mono_ns,
    const CallContext& call) {
    if (!hasHostModuleIdentity(call, CallerModuleId::AgentService) ||
        !clock_ || agent_id.empty() || expected_epoch == 0 ||
        drain_deadline_mono_ns != call.deadline_mono_ns ||
        deadlineExpired(drain_deadline_mono_ns, *clock_)) {
        return Status::Error(
            "agent_dispatch", "AGENT_UNREGISTER_INVALID",
            "agent unregistration is invalid");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = agents_.find(agent_id);
    if (found == agents_.end() ||
        found->second.manifest.agent_epoch != expected_epoch) {
        return Status::Error(
            "agent_dispatch", "AGENT_EPOCH_CONFLICT",
            "agent registration epoch does not match");
    }
    found->second.healthy = false;
    const bool active = std::any_of(
        dispatches_.begin(), dispatches_.end(),
        [&agent_id](const auto& item) {
            return item.second.route.agent_id == agent_id &&
                   !terminal(item.second.state);
        });
    if (active) {
        ++capacity_epoch_;
        recordCapacityUnlocked();
        (void)persistStateUnlocked();
        return Status::Error(
            "agent_dispatch", "AGENT_DRAIN_IN_PROGRESS",
            "agent remains registered until active leases drain", true);
    }
    recovered_agent_epochs_[agent_id] = expected_epoch;
    recovered_manifests_.erase(agent_id);
    agents_.erase(found);
    ++capacity_epoch_;
    recordCapacityUnlocked();
    return persistStateUnlocked();
}

AgentDispatchCapacity AgentDispatch::capacityUnlocked() const {
    AgentDispatchCapacity capacity;
    capacity.capacity_epoch = capacity_epoch_;
    for (const auto& [agent_id, record] : agents_) {
        (void)agent_id;
        if (record.healthy) {
            capacity.max_inflight += record.manifest.max_concurrency;
            capacity.reserved_p0_credits +=
                record.manifest.reserved_p0_slots;
        } else {
            capacity.health_state = "DEGRADED";
        }
    }
    std::uint32_t active_total = 0;
    std::uint32_t active_non_p0 = 0;
    for (const auto& [dispatch_id, snapshot] : dispatches_) {
        const auto agent = agents_.find(snapshot.route.agent_id);
        const bool owner_ready =
            agent != agents_.end() && agent->second.healthy;
        if (owner_ready &&
            (snapshot.state == DispatchState::Queued ||
             snapshot.state == DispatchState::Running ||
             snapshot.state == DispatchState::Unknown ||
             provider_submitted_.count(dispatch_id) != 0)) {
            ++active_total;
            if (snapshot.task.priority != TaskPriority::P0) ++active_non_p0;
        }
        if (snapshot.state == DispatchState::Queued) {
            ++capacity.queue_depth_by_priority[snapshot.task.priority];
        }
    }
    const auto total_free = capacity.max_inflight > active_total
                                ? capacity.max_inflight - active_total
                                : 0;
    const auto ordinary_limit = capacity.max_inflight -
        std::min(capacity.max_inflight, capacity.reserved_p0_credits);
    const auto ordinary_free = ordinary_limit > active_non_p0
                                   ? ordinary_limit - active_non_p0
                                   : 0;
    capacity.available_credits = std::min(total_free, ordinary_free);
    return capacity;
}

void AgentDispatch::recordCapacityUnlocked() {
    if (!capacity_events_.empty() &&
        capacity_events_.back().capacity_epoch == capacity_epoch_) return;
    capacity_events_.push_back(capacityUnlocked());
}

Result<CapacityEventPage> AgentDispatch::subscribeCapacity(
    const CapacitySubscription& request,
    const CallContext& call) const {
    if (!orchestratorControl(call) || request.consumer_id.empty() ||
        request.consumer_epoch == 0 || request.max_events == 0 ||
        request.max_events > 1000) {
        return Result<CapacityEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CAPACITY_SUBSCRIPTION_INVALID",
            "capacity subscription is invalid"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (request.cursor > capacity_events_.size()) {
        return Result<CapacityEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CAPACITY_CURSOR_INVALID",
            "capacity cursor exceeds the event history"));
    }
    CapacityEventPage page;
    auto cursor = static_cast<std::size_t>(request.cursor);
    for (; cursor < capacity_events_.size() &&
           page.events.size() < request.max_events; ++cursor) {
        page.events.push_back(capacity_events_[cursor]);
    }
    page.next_cursor = cursor;
    page.has_more = cursor < capacity_events_.size();
    return Result<CapacityEventPage>::Success(std::move(page));
}

AgentDispatchHealth AgentDispatch::health(
    const CallContext& call) const {
    AgentDispatchHealth health;
    if (!hasHostModuleIdentity(call, CallerModuleId::AgentService) ||
        !clock_ || deadlineExpired(call.deadline_mono_ns, *clock_)) {
        health.status_code = "DISPATCH_HEALTH_CALLER_NOT_ALLOWED";
        return health;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    health.accepting = accepting_;
    health.durable = durable_;
    health.registry_generation = capacity_epoch_;
    health.registered_agents = agents_.size();
    for (const auto& [agent_id, record] : agents_) {
        (void)agent_id;
        if (record.healthy) ++health.healthy_agents;
    }
    for (const auto& [dispatch_id, snapshot] : dispatches_) {
        (void)dispatch_id;
        if (!terminal(snapshot.state)) ++health.active_dispatches;
        if (snapshot.state == DispatchState::Unknown) {
            ++health.unknown_dispatches;
        }
    }
    std::uint64_t maximum_ack = 0;
    for (const auto& [consumer, ack] : event_acks_) {
        (void)consumer;
        maximum_ack = std::max(maximum_ack, ack.second);
    }
    health.event_backlog = events_.size() > maximum_ack
                               ? events_.size() - maximum_ack
                               : 0;
    health.healthy = durable_ && health.unknown_dispatches == 0 &&
                     health.healthy_agents == health.registered_agents;
    health.status_code = !durable_ ? "DISPATCH_DURABILITY_DEGRADED"
                         : health.unknown_dispatches != 0
                             ? "DISPATCH_RECONCILIATION_REQUIRED"
                             : "DISPATCH_HEALTHY";
    return health;
}

Status AgentDispatch::drain(
    std::int64_t deadline_mono_ns,
    const CallContext& call) {
    if (!hasHostModuleIdentity(call, CallerModuleId::AgentService) ||
        !clock_ || deadline_mono_ns != call.deadline_mono_ns ||
        deadlineExpired(deadline_mono_ns, *clock_)) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_DRAIN_INVALID",
            "dispatch drain request is invalid");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        const auto saved = persistStateUnlocked();
        if (!saved.ok) return saved;
    }
    while (!deadlineExpired(deadline_mono_ns, *clock_)) {
        const auto status = runUntilIdle(1024);
        if (status.ok) return Status::Ok();
        if (status.error.code != "DISPATCH_STALLED") return status;
        return status;
    }
    return Status::Error(
        "agent_dispatch", "DISPATCH_DRAIN_DEADLINE_EXPIRED",
        "dispatch drain deadline expired", true);
}

}  // namespace master_agent::agent_dispatch
