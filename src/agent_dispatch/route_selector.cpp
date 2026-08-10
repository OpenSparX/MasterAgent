/**
 * @file route_selector.cpp
 * @brief Selects compatible agents, queued work, and preemption victims.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

#include <algorithm>
#include <tuple>

namespace master_agent::agent_dispatch {

AgentRouteDecision AgentDispatch::route(
    const DispatchTask& task) const {

    struct Candidate {
        const AgentRecord* record = nullptr;
        std::size_t active = 0;
        bool requires_preemption = false;
    };
    std::vector<Candidate> candidates;
    bool compatible_seen = false;
    for (const auto& pair : agents_) {
        const auto& record = pair.second;
        const bool has_capability =
            std::find(record.manifest.capabilities.begin(),
                      record.manifest.capabilities.end(),
                      task.action) != record.manifest.capabilities.end();
        if (!record.healthy || !has_capability) continue;
        if (!task.target_agent.empty() &&
            record.manifest.agent_id != task.target_agent &&
            !task.allow_agent_fallback) {
            continue;
        }
        compatible_seen = true;
        std::size_t active = 0;
        std::size_t active_non_p0 = 0;
        std::size_t preemptible_victims = 0;
        for (const auto& dispatch : dispatches_) {
            if (dispatch.second.route.agent_id !=
                record.manifest.agent_id) {
                continue;
            }
            const auto state = dispatch.second.state;
            if (state == DispatchState::Queued ||
                state == DispatchState::Running ||
                state == DispatchState::Unknown) {
                ++active;
                if (dispatch.second.task.priority !=
                    TaskPriority::P0) {
                    ++active_non_p0;
                }
            }
            if (state == DispatchState::Running &&
                record.manifest.supports_safe_point_preemption &&
                isHigherPriority(
                    task.priority,
                    dispatch.second.task.priority)) {
                ++preemptible_victims;
            }
        }
        const auto ordinary_limit =
            record.manifest.max_concurrency -
            std::min(record.manifest.max_concurrency,
                     record.manifest.reserved_p0_slots);
        const bool total_slot =
            active < record.manifest.max_concurrency;
        const bool ordinary_slot =
            active_non_p0 < ordinary_limit;
        const bool available =
            task.priority == TaskPriority::P0
                ? total_slot
                : total_slot && ordinary_slot;
        // One running victim can back exactly one queued over-capacity
        // reservation. Without this accounting, repeated P0 submissions
        // could all point at the same victim and exceed the Provider lease
        // limit without bound.
        const auto total_overcommit =
            active > record.manifest.max_concurrency
                ? active - record.manifest.max_concurrency
                : std::size_t{0};
        const auto ordinary_overcommit =
            active_non_p0 > ordinary_limit
                ? active_non_p0 - ordinary_limit
                : std::size_t{0};
        const auto existing_overcommit =
            task.priority == TaskPriority::P0
                ? total_overcommit
                : std::max(total_overcommit,
                           ordinary_overcommit);
        const bool preemption_credit =
            preemptible_victims > existing_overcommit;
        if (!available && !preemption_credit) continue;
        candidates.push_back(
            {&record, active, !available});
    }
    if (candidates.empty()) {
        return {false, {}, 0, {}, {}, {},
                compatible_seen ? "NO_AGENT_CAPACITY"
                                : "NO_COMPATIBLE_AGENT",
                clock_->utcNowMs()};
    }
    std::sort(candidates.begin(), candidates.end(),
              [&task](const Candidate& left,
                      const Candidate& right) {
                  if (left.requires_preemption !=
                      right.requires_preemption) {
                      return !left.requires_preemption;
                  }
                  const bool left_target =
                      left.record->manifest.agent_id ==
                      task.target_agent;
                  const bool right_target =
                      right.record->manifest.agent_id ==
                      task.target_agent;
                  if (left_target != right_target) return left_target;
                  const auto left_free =
                      left.record->manifest.max_concurrency -
                      std::min<std::size_t>(
                          left.active,
                          left.record->manifest.max_concurrency);
                  const auto right_free =
                      right.record->manifest.max_concurrency -
                      std::min<std::size_t>(
                          right.active,
                          right.record->manifest.max_concurrency);
                  if (left_free != right_free) {
                      return left_free > right_free;
                  }
                  return left.record->manifest.agent_id <
                         right.record->manifest.agent_id;
              });
    const auto& chosen = candidates.front().record->manifest;
    return {true,
            chosen.agent_id,
            chosen.agent_epoch,
            chosen.manifest_digest,
            chosen.capability_version,
            {},
            "ROUTED",
            clock_->utcNowMs()};
}

std::optional<std::string> AgentDispatch::selectQueued() const {
    const DispatchSnapshot* best = nullptr;
    std::string best_id;
    for (const auto& pair : dispatches_) {
        const auto& snapshot = pair.second;
        if (snapshot.state != DispatchState::Queued ||
            provider_submitted_.count(pair.first) != 0) {
            continue;
        }
        if (!best ||
            std::tie(snapshot.task.priority,
                     snapshot.task.deadline_mono_ns,
                     snapshot.enqueue_sequence) <
                std::tie(best->task.priority,
                         best->task.deadline_mono_ns,
                         best->enqueue_sequence)) {
            best = &snapshot;
            best_id = pair.first;
        }
    }
    return best ? std::optional<std::string>(best_id) : std::nullopt;
}

std::optional<std::string> AgentDispatch::selectVictim(
    const std::string& agent_id,
    TaskPriority arriving_priority) const {

    const auto agent = agents_.find(agent_id);
    if (agent == agents_.end() ||
        !agent->second.manifest.supports_safe_point_preemption) {
        return std::nullopt;
    }
    const DispatchSnapshot* victim = nullptr;
    std::string victim_id;
    for (const auto& pair : dispatches_) {
        const auto& snapshot = pair.second;
        if (snapshot.route.agent_id != agent_id ||
            snapshot.state != DispatchState::Running ||
            !isHigherPriority(arriving_priority, snapshot.task.priority)) {
            continue;
        }
        if (!victim ||
            static_cast<std::uint8_t>(snapshot.task.priority) >
                static_cast<std::uint8_t>(victim->task.priority)) {
            victim = &snapshot;
            victim_id = pair.first;
        }
    }
    return victim ? std::optional<std::string>(victim_id) : std::nullopt;
}


}  // namespace master_agent::agent_dispatch
