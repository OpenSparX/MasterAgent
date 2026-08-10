#pragma once

/**
 * @file sub_agent.h
 * @brief Defines the controlled execution contract implemented by external sub-agents.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/common/types.h"

namespace master_agent::sub_agents {

struct AgentManifest {
    std::string agent_id;
    std::uint64_t agent_epoch = 1;
    std::string manifest_digest;
    std::string capability_version = "1";
    std::vector<std::string> capabilities;
    // Per-capability least-privilege contract. Missing entries mean that the
    // capability delegates neither permissions nor child Tools.
    std::map<std::string, std::vector<std::string>>
        required_permissions;
    std::map<std::string, std::vector<std::string>>
        required_atomic_tools;
    std::uint32_t max_concurrency = 1;
    // Capacity owned by this Provider but unavailable to P1/P2 admission.
    // P0 may consume it directly; the value must never exceed
    // max_concurrency.
    std::uint32_t reserved_p0_slots = 0;
    bool supports_safe_point_preemption = true;
    std::string prompt_profile_id = "default";
    std::string model_profile_id = "mock";
};

struct SubAgentExecutionRequest {
    std::string dispatch_id;
    std::string request_id;
    std::string pid;
    std::string activation_id;
    std::uint32_t attempt_no = 0;
    std::string operation_id;
    std::string execution_id;
    std::string agent_id;
    std::uint64_t agent_epoch = 0;
    std::string manifest_digest;
    std::string lease_id;
    std::string action;
    nlohmann::json params = nlohmann::json::object();
    std::string capability_digest;
    std::uint32_t expected_output_schema_version = 0;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::uint64_t fencing_token = 0;
    std::string trace_id;
    std::string principal_id_hash;
    std::string authorization_ref;
};

enum class SubAgentState : std::uint8_t {
    Accepted,
    Queued,
    Running,
    Suspended,
    Succeeded,
    Failed,
    Cancelled
};

struct SubAgentAcceptance {
    bool accepted = false;
    std::string dispatch_id;
    std::string reject_code;
};

struct SubAgentSnapshot {
    SubAgentExecutionRequest request;
    SubAgentState state = SubAgentState::Accepted;
    nlohmann::json result = nlohmann::json::object();
    std::uint32_t output_schema_version = 0;
    std::string error_code;
    std::string checkpoint_ref;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    bool resource_released = false;
    std::uint64_t control_epoch = 0;
    bool retryable_hint = false;
};

struct SubAgentEvent {
    std::string event_id;
    std::string dispatch_id;
    std::string event_type;
    SubAgentState state = SubAgentState::Accepted;
    std::string checkpoint_ref;
    bool resource_released = false;
};

/**
 * @brief Execution contract implemented by externally supplied sub-agents.
 *
 * Every operation is bound to dispatch identity, agent epoch, lease, fencing
 * token, deadline, and idempotency data. Implementations own their internal
 * business logic; MasterAgent owns only lifecycle governance.
 */
class ISubAgent {
public:
    virtual ~ISubAgent() = default;

    /// Returns the immutable capability and compatibility manifest.
    virtual AgentManifest getManifest() const = 0;

    /// Admits governed work; acceptance does not imply execution completion.
    virtual SubAgentAcceptance submit(
        const SubAgentExecutionRequest& request,
        const CallContext& call) = 0;

    /// Returns the authoritative state for a dispatch identity.
    virtual Result<SubAgentSnapshot> query(
        const std::string& dispatch_id,
        const CallContext& call) const = 0;

    /// Requests cooperative suspension at the next checkpoint-safe boundary.
    virtual Status requestPreempt(const std::string& dispatch_id,
                                  std::uint64_t control_epoch,
                                  const CallContext& call) = 0;

    /// Restores only the checkpoint bound to the current dispatch and agent epoch.
    virtual Status restore(const std::string& dispatch_id,
                           const std::string& checkpoint_ref,
                           std::uint64_t control_epoch,
                           const CallContext& call) = 0;

    /// Requests cooperative cancellation and fences older control commands.
    virtual Status cancel(const std::string& dispatch_id,
                          std::uint64_t control_epoch,
                          const CallContext& call) = 0;

    /// Advances at most one owner-loop transition.
    virtual bool pumpOne() = 0;

    virtual bool heartbeat(const CallContext& call) const = 0;
};

class DeterministicSubAgent final : public ISubAgent {
public:
    DeterministicSubAgent(AgentManifest manifest,
                             std::shared_ptr<IRuntimeClock> clock,
                             std::shared_ptr<IdGenerator> ids,
                             std::uint32_t work_units = 1);

    AgentManifest getManifest() const override;

    SubAgentAcceptance submit(
        const SubAgentExecutionRequest& request,
        const CallContext& call) override;

    Result<SubAgentSnapshot> query(
        const std::string& dispatch_id,
        const CallContext& call) const override;

    Status requestPreempt(const std::string& dispatch_id,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;

    Status restore(const std::string& dispatch_id,
                   const std::string& checkpoint_ref,
                   std::uint64_t control_epoch,
                   const CallContext& call) override;

    Status cancel(const std::string& dispatch_id,
                  std::uint64_t control_epoch,
                  const CallContext& call) override;

    bool pumpOne() override;

    bool heartbeat(const CallContext& call) const override;

private:
    struct Runtime {
        SubAgentSnapshot snapshot;
        std::uint32_t remaining_work_units = 0;
        std::uint64_t control_epoch = 0;
        std::string request_digest;
    };

    AgentManifest manifest_;
    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::uint32_t work_units_;
    mutable std::mutex mutex_;
    std::map<std::string, Runtime> runtimes_;
};

}  // namespace master_agent::sub_agents
