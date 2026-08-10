#pragma once

/**
 * @file interaction_layer.h
 * @brief Defines the external text-ingress contract and durable session turn allocation.
 */

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "master_agent/common/types.h"

namespace master_agent::interaction {

/// Returns the highest committed turn already present in the memory journal.
/// It is invoked at most once while recovering a missing durable counter;
/// sealed and legacy counters are authoritative and never call this hook.
using TurnFloorLookup = std::function<Result<std::uint64_t>(
    const std::string&, const std::string&)>;

struct TextInput {
    /// UTF-8 user text as received from the channel.
    std::string text;
    std::string user_id;
    std::string session_id;
    std::string source = "hmi";
    std::map<std::string, std::string> params;
};

struct StandardRequest {
    /// Stable identities allocated once at the ingress boundary.
    std::string request_id;
    std::string trace_id;
    std::string trigger_type = "TEXT_INPUT";
    std::string text;
    std::map<std::string, std::string> params;
    std::int64_t timestamp_utc_ms = 0;
    std::int64_t deadline_mono_ns = 0;
    std::string user_id;
    std::string session_id;
    std::uint64_t turn_id = 0;
    TaskPriority priority = TaskPriority::P1;
    std::string resume_task_id;
};

/**
 * @brief Converts external text into an authenticated, traceable request.
 *
 * submitText validates channel input and durably allocates one monotonically
 * increasing turn per user/session. It does not execute plans or vehicle side
 * effects.
 */
class IInteractionLayer {
public:
    virtual ~IInteractionLayer() = default;

    /**
     * @brief Validates input and allocates the next durable session turn.
     * @return A frozen StandardRequest, or a validation/persistence error.
     *
     * A rejected input does not consume a turn. Once allocation is committed,
     * the turn identifier is never reused even if downstream processing fails.
     */
    virtual Result<StandardRequest> submitText(const TextInput& input) = 0;
};

class InteractionLayer final : public IInteractionLayer {
public:
    InteractionLayer(std::shared_ptr<IRuntimeClock> clock,
                        std::shared_ptr<IdGenerator> ids,
                        std::filesystem::path turn_state_directory = {},
                        TurnFloorLookup turn_floor_lookup = {});

    Result<StandardRequest> submitText(const TextInput& input) override;

private:
    /// Persistently allocates one user/session turn while holding an
    /// inter-process lock over read-increment-replace.
    Result<std::uint64_t> allocateTurn(
        const std::string& user_id,
        const std::string& session_id,
        const std::string& request_id);

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::filesystem::path turn_state_directory_;
    TurnFloorLookup turn_floor_lookup_;
    // Per-session lanes keep one allocator instance single-writer without
    // serializing unrelated sessions across file I/O or migration callbacks.
    std::mutex lane_registry_mutex_;
    std::map<std::string, std::weak_ptr<std::mutex>>
        allocation_lanes_;
    std::mutex memory_turns_mutex_;
    std::map<std::string, std::uint64_t> session_turns_;
};

/**
 * @brief Creates the default single-process text-ingress component.
 *
 * This factory is the initialization entry point defined by the interaction
 * design. Tests and the runtime may use the injectable constructor when a
 * deterministic clock, identifier source, or turn-state location is needed.
 */
std::unique_ptr<IInteractionLayer> createInteractionLayer();

}  // namespace master_agent::interaction
