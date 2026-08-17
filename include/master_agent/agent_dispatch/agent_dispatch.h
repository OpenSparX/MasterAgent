#pragma once

/**
 * @file agent_dispatch.h
 * @brief Defines lifecycle, scheduling, recovery, and control contracts for sub-agent dispatch.
 */

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/sub_agents/sub_agent.h"

namespace master_agent::agent_dispatch {

/// Deterministic digest for the capability/schema contract carried by a
/// dispatch task.  It is independent from the selected agent instance;
/// route.manifest_digest and agent_epoch bind that second dimension.
std::string dispatchCapabilityDigest(
    const std::string& action, std::uint32_t input_schema_version,
    std::uint32_t output_schema_version);

/// Stable child-Tool idempotency identity frozen by the parent Dispatch.
std::string atomicChildIdempotencyKey(
    const std::string& parent_dispatch_id,
    const std::string& child_operation_id);

/// Digest of the immutable child-authorization snapshot issued by Agent
/// Dispatch after routing. Orchestrator and SubAgent callers may not supply
/// this seal themselves.
std::string dispatchChildAuthorizationDigest(
    const std::string& agent_id, std::uint64_t agent_epoch,
    const std::string& manifest_digest, const std::string& action,
    const std::vector<std::string>& granted_permissions,
    const std::vector<std::string>& allowed_child_capabilities);

struct DispatchTask {
    CallerModuleId caller_module_id =
        CallerModuleId::TaskOrchestrationEngine;
    std::string request_id;
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::uint32_t attempt_no = 1;
    std::string operation_id;
    std::string task_id;
    std::string action;
    std::string target_agent;
    bool allow_agent_fallback = false;
    nlohmann::json params = nlohmann::json::object();
    std::uint32_t input_schema_version = 1;
    std::uint32_t expected_output_schema_version = 1;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::string idempotency_key;
    std::uint64_t fencing_token = 0;
    std::string capability_digest;
    std::uint64_t capacity_epoch = 1;
    std::vector<std::string> resource_lease_refs;
    // Frozen Admission claims used only to validate child operations made by
    // the selected SubAgent; Providers cannot expand either set.
    std::vector<std::string> granted_permissions;
    std::vector<std::string> allowed_child_capabilities;
    // Empty on submit. Agent Dispatch fills this after selecting the
    // registered Manifest and reducing Admission to least privilege.
    std::string child_authorization_digest;
    std::string principal_id_hash;
    std::string authorization_ref;
    std::string trace_id;
};

struct AgentRouteDecision {
    bool routed = false;
    std::string agent_id;
    std::uint64_t agent_epoch = 0;
    std::string manifest_digest;
    std::string capability_version;
    std::string lease_id;
    std::string reason_code;
    std::int64_t route_decided_at_utc_ms = 0;
};

enum class DispatchState : std::uint8_t {
    Accepted,
    Queued,
    Running,
    Suspended,
    Succeeded,
    Failed,
    Cancelled,
    Unknown
};

struct DispatchAcceptance {
    bool accepted = false;
    bool existing = false;
    std::string dispatch_id;
    std::string operation_id;
    std::string reject_code;
};

struct DispatchEvent {
    std::string event_id;
    std::string event_type;
    std::string dispatch_id;
    std::string request_id;
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::uint32_t attempt_no = 0;
    std::string operation_id;
    std::string agent_id;
    std::uint64_t agent_epoch = 0;
    std::string lease_id;
    DispatchState state = DispatchState::Accepted;
    std::uint64_t fencing_token = 0;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    nlohmann::json result = nlohmann::json::object();
    std::uint32_t output_schema_version = 0;
    std::string error_code;
    bool safe_point = false;
    std::string checkpoint_ref;
    bool resource_released = false;
    std::int64_t occurred_at_utc_ms = 0;
    std::string trace_id;
};

struct DispatchSnapshot {
    std::string dispatch_id;
    DispatchTask task;
    AgentRouteDecision route;
    DispatchState state = DispatchState::Accepted;
    nlohmann::json result = nlohmann::json::object();
    std::string error_code;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    std::string checkpoint_ref;
    std::uint64_t control_epoch = 0;
    std::uint64_t enqueue_sequence = 0;
    bool retryable_hint = false;
};

struct AgentDispatchCapacity {
    std::string target_id = "agent_dispatch";
    std::uint64_t capacity_epoch = 1;
    std::uint32_t available_credits = 0;
    std::uint32_t max_inflight = 0;
    std::uint32_t reserved_p0_credits = 0;
    std::map<TaskPriority, std::size_t> queue_depth_by_priority;
    std::string health_state = "READY";
};

struct AgentCapabilityQuery {
    std::optional<std::string> agent_id;
    std::optional<std::string> capability;
    std::optional<std::string> expected_snapshot_digest;
};

struct AgentCapabilitySnapshot {
    std::uint64_t registry_generation = 0;
    std::string snapshot_digest;
    std::vector<sub_agents::AgentManifest> manifests;
};

struct DispatchControlAck {
    bool accepted = false;
    bool existing = false;
    std::uint64_t control_epoch = 0;
    std::string error_code;
};

struct DispatchEventSubscription {
    std::string consumer_id;
    std::uint64_t consumer_epoch = 1;
    std::uint64_t cursor = 0;
    std::optional<std::string> dispatch_id;
    std::uint32_t max_events = 100;
};

struct DispatchEventPage {
    std::vector<DispatchEvent> events;
    std::uint64_t next_cursor = 0;
    bool has_more = false;
};

struct DispatchEventAck {
    std::string consumer_id;
    std::uint64_t consumer_epoch = 1;
    std::uint64_t cursor = 0;
};

struct CapacitySubscription {
    std::string consumer_id;
    std::uint64_t consumer_epoch = 1;
    std::uint64_t cursor = 0;
    std::uint32_t max_events = 100;
};

struct CapacityEventPage {
    std::vector<AgentDispatchCapacity> events;
    std::uint64_t next_cursor = 0;
    bool has_more = false;
};

struct AgentDispatchHealth {
    bool healthy = false;
    bool accepting = false;
    bool durable = false;
    std::size_t registered_agents = 0;
    std::size_t healthy_agents = 0;
    std::size_t active_dispatches = 0;
    std::size_t unknown_dispatches = 0;
    std::size_t event_backlog = 0;
    std::uint64_t registry_generation = 0;
    std::string status_code;
};

/**
 * @brief Routes governed work to registered sub-agents and tracks its lifecycle.
 *
 * submitDispatch acknowledges durable admission, not completion. Callers use
 * queryDispatch or reconcileDispatch to determine the terminal outcome.
 */
class IAgentDispatch {
public:
    virtual ~IAgentDispatch() = default;

    /**
     * Registers one agent generation from its manifest. Replacement creates a
     * new epoch so callbacks from the previous process cannot commit.
     */
    virtual Status registerAgent(
        std::shared_ptr<sub_agents::ISubAgent> agent,
        const CallContext& call) = 0;

    virtual Result<AgentCapabilitySnapshot> snapshotAgentCapabilities(
        const AgentCapabilityQuery&, const CallContext&) const {
        return Result<AgentCapabilitySnapshot>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CAPABILITY_SNAPSHOT_UNSUPPORTED",
            "capability snapshots are not implemented"));
    }

    /// Durably admits a dispatch; acceptance is not terminal completion.
    virtual DispatchAcceptance submitDispatch(
        const DispatchTask& task, const CallContext& call) = 0;

    /// Returns the authoritative snapshot by dispatch or operation identity.
    virtual Result<DispatchSnapshot> queryDispatch(
        const std::string& dispatch_or_operation_id,
        const CallContext& call) const = 0;

    /// Resolve an UNKNOWN execution only from an exact Agent epoch/lease/
    /// fencing/schema-sealed provider observation.
    virtual Result<DispatchSnapshot> reconcileDispatch(
        const std::string& operation_id,
        std::uint64_t expected_agent_epoch,
        const CallContext& call) = 0;

    /// Requests cooperative preemption under a monotonic control epoch.
    virtual Status requestPreempt(const std::string& dispatch_id,
                                  TaskPriority arriving_priority,
                                  std::uint64_t control_epoch,
                                  const CallContext& call) = 0;

    virtual DispatchControlAck updateDispatchPriority(
        const std::string&, TaskPriority, const std::string&,
        std::uint64_t, const CallContext&) {
        return {};
    }

    virtual DispatchControlAck restoreDispatch(
        const std::string&, const std::string&, std::uint64_t,
        const CallContext&) {
        return {};
    }

    virtual DispatchControlAck cancelDispatch(
        const std::string&, const std::string&, std::uint64_t,
        std::int64_t, const CallContext&) {
        return {};
    }

    virtual Result<DispatchEventPage> subscribeEvents(
        const DispatchEventSubscription&, const CallContext&) const {
        return Result<DispatchEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_SUBSCRIPTION_UNSUPPORTED",
            "event subscription is not implemented"));
    }

    virtual Status ackDispatchEvent(
        const DispatchEventAck&, const CallContext&) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_EVENT_ACK_UNSUPPORTED",
            "event acknowledgement is not implemented");
    }

    virtual Status unregisterAgent(
        const std::string&, std::uint64_t, std::int64_t,
        const CallContext&) {
        return Status::Error(
            "agent_dispatch", "AGENT_UNREGISTER_UNSUPPORTED",
            "agent unregistration is not implemented");
    }

    virtual Result<AgentCapabilitySnapshot> getRegisteredAgents(
        const AgentCapabilityQuery& query,
        const CallContext& call) const {
        return snapshotAgentCapabilities(query, call);
    }

    /// Advances at most one owner-loop scheduling transition.
    virtual bool pumpOne() = 0;

    /// Drives the deterministic scheduler until quiescent or max_steps is reached.
    virtual Status runUntilIdle(std::size_t max_steps = 10000) = 0;

    virtual std::vector<DispatchEvent> events() const = 0;

    /// Returns a read-only capacity snapshot for admission decisions.
    virtual AgentDispatchCapacity getCapacity(
        const CallContext& call) const = 0;

    virtual Result<CapacityEventPage> subscribeCapacity(
        const CapacitySubscription&, const CallContext&) const {
        return Result<CapacityEventPage>::Failure(Status::Error(
            "agent_dispatch", "DISPATCH_CAPACITY_SUBSCRIPTION_UNSUPPORTED",
            "capacity subscription is not implemented"));
    }

    virtual AgentDispatchHealth health(
        const CallContext&) const { return {}; }

    virtual Status drain(
        std::int64_t, const CallContext&) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_DRAIN_UNSUPPORTED",
            "drain is not implemented");
    }
};

class AgentDispatch final
    : public IAgentDispatch,
      public atomic_service::IAtomicParentLineageValidator,
      public inference::IInferenceParentLineageValidator {
public:
    AgentDispatch(std::shared_ptr<IRuntimeClock> clock,
                     std::shared_ptr<IdGenerator> ids);

    AgentDispatch(std::filesystem::path storage_directory,
                  std::shared_ptr<IRuntimeClock> clock,
                  std::shared_ptr<IdGenerator> ids);

    Status initialize();

    Status registerAgent(
        std::shared_ptr<sub_agents::ISubAgent> agent,
        const CallContext& call) override;

    Result<AgentCapabilitySnapshot> snapshotAgentCapabilities(
        const AgentCapabilityQuery& query,
        const CallContext& call) const override;

    DispatchAcceptance submitDispatch(
        const DispatchTask& task, const CallContext& call) override;

    Result<DispatchSnapshot> queryDispatch(
        const std::string& dispatch_or_operation_id,
        const CallContext& call) const override;

    Result<DispatchSnapshot> reconcileDispatch(
        const std::string& operation_id,
        std::uint64_t expected_agent_epoch,
        const CallContext& call) override;

    Status requestPreempt(const std::string& dispatch_id,
                          TaskPriority arriving_priority,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;

    DispatchControlAck updateDispatchPriority(
        const std::string& dispatch_id,
        TaskPriority effective_priority,
        const std::string& inheritance_source,
        std::uint64_t control_epoch,
        const CallContext& call) override;

    DispatchControlAck restoreDispatch(
        const std::string& dispatch_id,
        const std::string& checkpoint_ref,
        std::uint64_t control_epoch,
        const CallContext& call) override;

    DispatchControlAck cancelDispatch(
        const std::string& dispatch_id,
        const std::string& reason,
        std::uint64_t control_epoch,
        std::int64_t deadline_mono_ns,
        const CallContext& call) override;

    Result<DispatchEventPage> subscribeEvents(
        const DispatchEventSubscription& request,
        const CallContext& call) const override;

    Status ackDispatchEvent(
        const DispatchEventAck& ack,
        const CallContext& call) override;

    Status unregisterAgent(
        const std::string& agent_id,
        std::uint64_t expected_epoch,
        std::int64_t drain_deadline_mono_ns,
        const CallContext& call) override;

    bool pumpOne() override;

    Status runUntilIdle(std::size_t max_steps = 10000) override;

    std::vector<DispatchEvent> events() const override;

    AgentDispatchCapacity getCapacity(
        const CallContext& call) const override;

    Result<CapacityEventPage> subscribeCapacity(
        const CapacitySubscription& request,
        const CallContext& call) const override;

    AgentDispatchHealth health(
        const CallContext& call) const override;

    Status drain(
        std::int64_t deadline_mono_ns,
        const CallContext& call) override;

    Status validateAtomicParentLineage(
        const atomic_service::AtomicMcpCallEnvelope& request,
        const CallContext& call) const override;

    Result<std::string> acquireAtomicParentInvocationLease(
        const atomic_service::AtomicMcpCallEnvelope& request,
        const CallContext& call) override;

    Status releaseAtomicParentInvocationLease(
        const std::string& reservation_id) override;

    /// Validate a SubAgent model job against its live AgentLease.
    Status validateInferenceParentLineage(
        const inference::InferenceRequest& request,
        const CallContext& call) const override;

    /// Pin the parent AgentLease across the physical model invocation.
    Result<std::string> acquireInferenceParentInvocationLease(
        const inference::InferenceRequest& request,
        const CallContext& call) override;

    /// Release an idempotent model invocation reservation.
    Status releaseInferenceParentInvocationLease(
        const std::string& reservation_id) override;

private:
    struct AgentRecord {
        sub_agents::AgentManifest manifest;
        std::shared_ptr<sub_agents::ISubAgent> provider;
        bool healthy = true;
    };

    struct AtomicChildInvocationReservation {
        std::string reservation_id;
        std::string child_execution_id;
        std::string child_operation_id;
        std::string child_idempotency_key;
        std::string parent_dispatch_id;
        std::string parent_operation_id;
        std::string parent_agent_id;
        std::uint64_t parent_agent_epoch = 0;
        std::string parent_lease_id;
        std::uint64_t parent_fencing_token = 0;
        std::uint64_t parent_control_epoch = 0;
    };

    struct InferenceChildInvocationReservation {
        std::string reservation_id;
        std::string child_job_id;
        std::string child_idempotency_key;
        std::string parent_dispatch_id;
        std::string parent_operation_id;
        std::string parent_agent_id;
        std::uint64_t parent_agent_epoch = 0;
        std::string parent_lease_id;
        std::uint64_t parent_fencing_token = 0;
        std::uint64_t parent_control_epoch = 0;
    };

    struct DeferredParentTransition {
        DispatchState state = DispatchState::Running;
        std::string event_type;
        bool safe_point = false;
        bool resource_released = false;
    };

    /// Caller must hold mutex_.
    Status validateAtomicParentLineageUnlocked(
        const atomic_service::AtomicMcpCallEnvelope& request,
        const CallContext& call) const;

    /// Caller must hold mutex_.
    Status validateInferenceParentLineageUnlocked(
        const inference::InferenceRequest& request,
        const CallContext& call) const;

    /// Caller must hold mutex_.
    bool hasActiveChildInvocationUnlocked(
        const std::string& dispatch_id) const;

    /// Caller must hold mutex_. Applies a transition held while an Atomic
    /// child invocation pinned the parent AgentLease.
    void applyDeferredParentTransitionUnlocked(
        const std::string& dispatch_id);

    AgentRouteDecision route(const DispatchTask& task) const;

    std::optional<std::string> selectQueued() const;

    std::optional<std::string> selectVictim(
        const std::string& agent_id,
        TaskPriority arriving_priority) const;

    void emit(DispatchSnapshot& snapshot,
              const std::string& event_type,
              bool safe_point = false,
              bool resource_released = false);

    /// Caller must hold mutex_. UNKNOWN retains its Agent lease/capacity
    /// because the external Provider may still have executed.
    void markUnknownUnlocked(const std::string& dispatch_id,
                             const std::string& error_code);

    /// Applies an authoritative lifecycle transition and advances the
    /// capacity generation exactly when the transition acquires or releases
    /// an AgentLease credit.
    void setStateUnlocked(DispatchSnapshot& snapshot,
                          DispatchState state);

    AgentDispatchCapacity capacityUnlocked() const;
    void recordCapacityUnlocked();
    Status persistStateUnlocked() const;
    Status recoverStateUnlocked();

    /// Scheduler-internal preemption for an already admitted queued task.
    /// This path avoids fabricating a public Orchestrator caller identity.
    Status requestPreemptInternal(
        const std::string& dispatch_id,
        TaskPriority arriving_priority,
        std::uint64_t control_epoch,
        std::int64_t control_deadline_mono_ns,
        const std::string& authorization_ref);

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::filesystem::path storage_directory_;
    // pumpOne performs external submit/preempt/restore callbacks.  A single
    // scheduler driver prevents duplicate physical calls from concurrent
    // host threads.
    mutable std::mutex pump_mutex_;
    mutable std::mutex mutex_;
    std::map<std::string, AgentRecord> agents_;
    std::map<std::string, DispatchSnapshot> dispatches_;
    std::map<std::string,
             std::shared_ptr<sub_agents::ISubAgent>>
        dispatch_providers_;
    std::map<std::string, std::string> operation_to_dispatch_;
    std::map<std::string, std::string> execution_to_dispatch_;
    std::map<std::string, std::string> idempotency_to_dispatch_;
    std::map<std::string, std::string> idempotency_digest_;
    // A reservation closes the validation-to-Provider TOCTOU window. Once
    // granted, later parent preemption cannot retroactively invalidate the
    // exact child invocation that already reached its physical boundary.
    std::map<std::string, AtomicChildInvocationReservation>
        atomic_child_reservations_;
    std::map<std::string, std::string>
        atomic_child_execution_to_reservation_;
    std::map<std::string, InferenceChildInvocationReservation>
        inference_child_reservations_;
    std::map<std::string, std::string>
        inference_child_job_to_reservation_;
    std::map<std::string, DeferredParentTransition>
        deferred_parent_transitions_;
    // Dispatches whose request may already be owned by the external
    // provider. They consume provider capacity even while the authenticated
    // provider snapshot still reports Accepted/Queued.
    std::set<std::string> provider_submitted_;
    std::set<std::string> reconcile_inflight_;
    std::vector<DispatchEvent> events_;
    std::vector<AgentDispatchCapacity> capacity_events_;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
        event_acks_;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
        capacity_acks_;
    std::map<std::string, std::uint64_t> recovered_agent_epochs_;
    // Durable manifests are retained without Provider pointers. A restarted
    // Provider must prove the same epoch/digest (or a strictly newer epoch)
    // before the registry becomes routable again.
    std::map<std::string, sub_agents::AgentManifest>
        recovered_manifests_;
    bool accepting_ = true;
    bool durable_ = true;
    std::uint64_t enqueue_sequence_ = 0;
    std::uint64_t control_sequence_ = 0;
    std::uint64_t capacity_epoch_ = 1;
};

}  // namespace master_agent::agent_dispatch
