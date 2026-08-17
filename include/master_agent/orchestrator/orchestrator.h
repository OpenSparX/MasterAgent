#pragma once

/**
 * @file orchestrator.h
 * @brief Defines deterministic DAG admission, scheduling, reconciliation, and recovery.
 */

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"

namespace master_agent::orchestrator {

enum class TriggerType : std::uint8_t {
    Immediate,
    Timer,
    Signal
};

enum class RepeatPolicy : std::uint8_t {
    Once,
    FixedRate,
    FixedDelay,
    BoundedCron
};

enum class MissedFirePolicy : std::uint8_t {
    Skip,
    Coalesce,
    CatchUpBounded
};

enum class OverlapPolicy : std::uint8_t {
    Serial,
    DropIfRunning,
    CoalesceLatest,
    ParallelBounded
};

enum class JoinKind : std::uint8_t {
    All,
    Any,
    Quorum
};

/**
 * @brief Deterministic external trigger declared by the submitted DAG.
 *
 * Design trace: Task Orchestration Engine Detailed Design, sections 2.6 and 3.4.
 * `expression` is structured data. Natural-language polling expressions are
 * intentionally rejected by validation.
 */
struct TriggerCondition {
    TriggerType type = TriggerType::Immediate;
    std::string source;
    nlohmann::json expression = nlohmann::json::object();
    RepeatPolicy repeat_policy = RepeatPolicy::Once;
    std::int64_t next_fire_at_utc_ms = 0;
    MissedFirePolicy missed_fire_policy = MissedFirePolicy::Skip;
    OverlapPolicy overlap_policy = OverlapPolicy::Serial;
    std::uint32_t catch_up_limit = 1;
    std::uint32_t max_parallel = 1;
};

struct JoinPolicy {
    JoinKind kind = JoinKind::All;
    std::uint32_t quorum = 0;
};

/**
 * @brief Typed, declarative binding from a successful upstream result.
 *
 * Only registered pure transforms are accepted. This delivery registers
 * `IDENTITY`; script text and natural-language transforms are rejected.
 */
struct ResultBinding {
    std::string source_node_id;
    std::string source_field_path;
    std::string target_param;
    std::string transform_id = "IDENTITY";
    std::string on_missing = "FAIL_NODE";
    nlohmann::json default_value;
    std::uint32_t source_schema_version = 1;
    std::uint32_t target_schema_version = 1;
};

struct LifecyclePolicy {
    std::uint32_t max_activations = 1;
    std::int64_t repeat_interval_ms = 0;
    std::int64_t end_at_utc_ms = 0;
};

struct CompensationSpec {
    bool enabled = false;
    std::string executor = "atomic_service";
    std::string action;
    std::string target_agent;
    nlohmann::json params = nlohmann::json::object();
    std::vector<ResultBinding> result_bindings;
};

struct DAGNode {
    std::string node_id;
    std::string node_type = "ATOMIC";
    std::string executor;  // atomic_service / agent_dispatch
    std::string action;
    std::string target_agent;
    bool allow_agent_fallback = false;
    nlohmann::json params = nlohmann::json::object();
    std::vector<std::string> dependencies;
    std::uint32_t input_schema_version = 1;
    std::uint32_t expected_output_schema_version = 1;
    TaskPriority base_priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::uint32_t max_attempts = 1;
    std::vector<std::string> resource_requirements;
    TriggerCondition trigger;
    JoinPolicy join_policy;
    std::vector<ResultBinding> result_bindings;
    LifecyclePolicy lifecycle;
    CompensationSpec compensation;
};

struct DAGEdge {
    std::string edge_id;
    std::string from_node_id;
    std::string to_node_id;
    std::string edge_type = "SUCCESS";
    nlohmann::json predicate = nlohmann::json::object();
    bool required = true;

    DAGEdge() = default;

    // Preserves the original five-field contract after predicate support was
    // added. Existing callers that supplied `required` as field five retain
    // their exact meaning instead of silently constructing a JSON boolean.
    DAGEdge(std::string id, std::string from, std::string to,
            std::string type, bool is_required)
        : edge_id(std::move(id)),
          from_node_id(std::move(from)),
          to_node_id(std::move(to)),
          edge_type(std::move(type)),
          required(is_required) {}

    DAGEdge(std::string id, std::string from, std::string to,
            std::string type, nlohmann::json condition,
            bool is_required)
        : edge_id(std::move(id)),
          from_node_id(std::move(from)),
          to_node_id(std::move(to)),
          edge_type(std::move(type)),
          predicate(condition.is_null()
                        ? nlohmann::json::object()
                        : std::move(condition)),
          required(is_required) {}
};

struct IntentDAG {
    std::string dag_id;
    std::string request_id;
    std::vector<DAGNode> nodes;
    std::vector<DAGEdge> edges;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::uint32_t schema_version = 2;
    std::string idempotency_key;
};

/// Trusted Admission-issued retry contract. Intent/LLM output cannot
/// manufacture or broaden this policy.
struct CapabilityRetryPolicy {
    std::string idempotency_policy = "NON_RETRYABLE";
    std::set<std::string> retryable_errors;
    std::int64_t base_backoff_ns = 0;
    std::int64_t max_backoff_ns = 0;
};

/// Canonical digest bound into Admission and the submit idempotency ledger.
std::string retryPoliciesDigest(
    const std::map<std::string, CapabilityRetryPolicy>& policies);

struct AdmissionContext {
    std::string principal_id_hash;
    std::string source_type = "USER_INTERACTION";
    TaskPriority granted_priority = TaskPriority::P1;
    bool p0_authorization = false;
    std::set<std::string> p0_allowed_capabilities;
    std::string policy_snapshot_id = "admission-policy-v1";
    std::string policy_digest;
    std::string authorization_ref;
    std::size_t max_nodes = 32;
    std::set<std::string> allowed_capabilities;
    std::set<std::string> granted_permissions;
    std::map<std::string, CapabilityRetryPolicy> retry_policies;
    std::string retry_policy_digest;
    std::int64_t deadline_mono_ns = 0;
};

struct OrchestratorSubmitRequest {
    IntentDAG dag;
    AdmissionContext admission;
    std::string idempotency_key;
    std::string expected_capability_digest;
    std::string trace_id;
    std::int64_t submitted_at_utc_ms = 0;
};

struct ValidationResult {
    bool valid = false;
    std::string reject_code;
    std::vector<std::string> details;
};

struct PlanCommitResult {
    bool accepted = false;
    bool existing = false;
    std::string plan_id;
    std::map<std::string, std::string> node_id_to_pid;
    TaskPriority summary_priority = TaskPriority::P2;
    std::int64_t committed_at_utc_ms = 0;
    std::string reject_code;
};

enum class PlanState : std::uint8_t {
    Committed = 0,
    Running = 1,
    Succeeded = 2,
    Failed = 3,
    Cancelled = 4,
    Unknown = 5,
    Suspended = 6,
    Compensating = 7
};

enum class ActivationState : std::uint8_t {
    Blocked,
    Ready,
    DispatchPending,
    Queued,
    Running,
    Reconciling,
    Succeeded,
    Failed,
    Skipped,
    Cancelled,
    Unknown
};

/**
 * @brief Immutable summary of one completed activation of a stable PID.
 *
 * Design trace: Task Orchestration Engine Detailed Design, sections 2.5.3,
 * 2.7, and 3.5. A recurring node keeps one PID while every real trigger gets
 * a different activation, execution, and operation identity. Completed
 * activations are retained here so getNodeStatus never overwrites history.
 */
struct ActivationHistoryEntry {
    std::string activation_id;
    std::string trigger_instance_id;
    ActivationState state = ActivationState::Blocked;
    std::uint32_t attempt_count = 0;
    std::string execution_id;
    std::string operation_id;
    nlohmann::json result = nlohmann::json::object();
    std::string error_code;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    std::int64_t terminal_at_utc_ms = 0;
};

struct NodeRuntimeSnapshot {
    DAGNode definition;
    std::string pid;
    std::string activation_id;
    ActivationState state = ActivationState::Blocked;
    TaskPriority effective_priority = TaskPriority::P1;
    std::uint32_t attempt_count = 0;
    std::string execution_id;
    std::string operation_id;
    std::uint64_t fencing_token = 0;
    nlohmann::json result = nlohmann::json::object();
    std::string error_code;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    bool retryable_hint = false;
    // Absolute monotonic wake-up. A scheduled retry remains Blocked until
    // this point, so pumpOne cannot spin it as runnable work.
    std::int64_t retry_at_mono_ns = 0;
    bool trigger_satisfied = true;
    std::string trigger_instance_id;
    std::vector<std::string> blockers;
    nlohmann::json bound_params = nlohmann::json::object();
    /// Number of real triggers created for this PID, including the current
    /// activation when activation_id is non-empty.
    std::uint32_t activation_count = 0;
    /// Runtime timer cursor. This is persisted separately from the immutable
    /// submitted node definition and is restored across process restart.
    std::int64_t next_fire_at_utc_ms = 0;
    /// Trigger instances queued by SERIAL overlap handling.
    std::uint32_t pending_trigger_count = 0;
    std::string pending_trigger_instance_id;
    std::vector<ActivationHistoryEntry> activation_history;
};

struct TaskPlanSnapshot {
    std::string plan_id;
    std::string request_id;
    PlanState state = PlanState::Committed;
    TaskPriority summary_priority = TaskPriority::P2;
    TaskPriority active_priority = TaskPriority::P2;
    std::int64_t effective_deadline_mono_ns = 0;
    std::string capability_snapshot_id;
    std::string policy_snapshot_id;
    std::uint64_t control_epoch = 0;
    std::uint64_t version = 1;
    std::map<std::string, NodeRuntimeSnapshot> nodes;
    std::string trace_id;
    bool cancellation_requested = false;
    std::string parent_plan_id;
};

struct TaskEvent {
    std::string event_id;
    std::string event_type;
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::uint64_t plan_version = 0;
    std::uint64_t orchestrator_epoch = 1;
    std::int64_t occurred_at_utc_ms = 0;
    std::string payload_digest;
    std::string trace_id;
};

struct DispatchCapacity {
    std::string target_id;
    std::uint64_t capacity_epoch = 0;
    std::uint32_t available_credits = 0;
    std::uint32_t max_inflight = 0;
    std::int64_t valid_until_mono_ns = 0;
};

struct CapacityAck {
    bool accepted = false;
    std::uint64_t applied_epoch = 0;
    std::string reject_code;
};

struct DispatchAcceptance {
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::string operation_id;
    std::uint32_t attempt_no = 0;
    std::uint64_t fencing_token = 0;
    bool accepted = false;
    std::string reject_code;
    std::string executor_id;
    std::uint64_t executor_epoch = 0;
    std::int64_t accepted_at_utc_ms = 0;
};

struct ExecutionStarted {
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::string operation_id;
    std::uint32_t attempt_no = 0;
    std::uint64_t fencing_token = 0;
    std::uint64_t executor_epoch = 0;
    std::int64_t started_at_utc_ms = 0;
};

struct NodeExecutionResult {
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::string operation_id;
    std::uint32_t attempt_no = 0;
    std::uint64_t fencing_token = 0;
    std::uint64_t executor_epoch = 0;
    TerminalStatus status = TerminalStatus::Pending;
    nlohmann::json result = nlohmann::json::object();
    std::string error_code;
    bool retryable_hint = false;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    std::int64_t started_at_utc_ms = 0;
    std::int64_t completed_at_utc_ms = 0;
};

struct ExecutionEventAck {
    bool accepted = false;
    bool duplicate = false;
    std::uint64_t plan_version = 0;
    std::string reject_code;
};

using ExecutionCommitResult = ExecutionEventAck;

struct SignalEvent {
    std::string event_id;
    std::string source;
    std::string event_name;
    nlohmann::json payload = nlohmann::json::object();
    std::uint64_t source_epoch = 0;
    std::uint64_t cursor = 0;
    std::int64_t occurred_at_utc_ms = 0;
    std::int64_t received_at_utc_ms = 0;
    std::string signature_ref;
};

struct SignalCommitResult {
    bool accepted = false;
    bool duplicate = false;
    std::size_t activated_count = 0;
    std::string reject_code;
};

struct PlanControlRequest {
    std::string plan_id;
    std::uint64_t control_epoch = 0;
    std::uint64_t expected_plan_version = 0;
    std::string reason;
    std::int64_t deadline_mono_ns = 0;
};

struct PlanCancelRequest : PlanControlRequest {};

struct ActivationCancelRequest : PlanControlRequest {
    std::string pid;
    std::string activation_id;
};

struct RollbackRequest : PlanControlRequest {
    std::string idempotency_key;
};

struct ControlCommitResult {
    bool accepted = false;
    bool existing = false;
    std::uint64_t plan_version = 0;
    std::string reject_code;
};

struct CompensationCommitResult {
    bool accepted = false;
    bool existing = false;
    std::uint64_t plan_version = 0;
    std::string reject_code;
    std::string compensation_plan_id;
};

struct EventSubscriptionRequest {
    std::string consumer_id;
    std::uint64_t consumer_epoch = 0;
    std::uint64_t cursor = 0;
    std::size_t max_events = 128;
    std::optional<std::string> plan_id;
};

struct TaskEventPage {
    std::vector<TaskEvent> events;
    std::uint64_t next_cursor = 0;
    bool has_more = false;
};

struct EventAckRequest {
    std::string consumer_id;
    std::uint64_t consumer_epoch = 0;
    std::uint64_t cursor = 0;
};

enum class HealthDetailLevel : std::uint8_t {
    Summary,
    Detailed
};

struct OrchestratorHealth {
    bool healthy = false;
    bool durable = false;
    bool scheduler_available = false;
    std::size_t plan_count = 0;
    std::size_t ready_count = 0;
    std::size_t running_count = 0;
    std::size_t timer_wait_count = 0;
    std::size_t signal_wait_count = 0;
    std::size_t event_backlog = 0;
    std::string status_code;
};

/**
 * @brief Validates deterministic DAGs and owns durable plan state.
 *
 * submit commits a plan but does not imply terminal execution. Unknown side
 * effects remain reconcilable facts and are never converted into safe retries.
 */
class IOrchestrator {
public:
    virtual ~IOrchestrator() = default;

    /// Performs pure structural and policy validation; no plan is committed.
    virtual ValidationResult validateDAG(
        const IntentDAG& dag, const AdmissionContext& admission,
        const CallContext& call) const = 0;

    /**
     * Commits a validated plan before acknowledging it. A committed response
     * establishes durable ownership, not terminal completion.
     */
    virtual PlanCommitResult submit(
        const OrchestratorSubmitRequest& request,
        const CallContext& call) = 0;

    virtual CapacityAck updateDispatchCapacity(
        const DispatchCapacity& capacity,
        const CallContext& call) = 0;

    virtual ExecutionEventAck ackDispatch(
        const DispatchAcceptance& acceptance,
        const CallContext& call) = 0;

    virtual ExecutionEventAck ackExecutionStarted(
        const ExecutionStarted& started,
        const CallContext& call) = 0;

    virtual ExecutionCommitResult completeExecution(
        const NodeExecutionResult& result,
        const CallContext& call) = 0;

    virtual SignalCommitResult publishSignal(
        const SignalEvent& signal,
        const CallContext& call) = 0;

    virtual ControlCommitResult suspend(
        const PlanControlRequest& request,
        const CallContext& call) = 0;

    virtual ControlCommitResult resume(
        const PlanControlRequest& request,
        const CallContext& call) = 0;

    virtual ControlCommitResult cancelPlan(
        const PlanCancelRequest& request,
        const CallContext& call) = 0;

    virtual ControlCommitResult cancelActivation(
        const ActivationCancelRequest& request,
        const CallContext& call) = 0;

    virtual CompensationCommitResult rollback(
        const RollbackRequest& request,
        const CallContext& call) = 0;

    /// Returns the authoritative plan and activation snapshot.
    virtual Result<TaskPlanSnapshot> getPlan(
        const std::string& plan_id, const CallContext& call) const = 0;

    virtual Result<NodeRuntimeSnapshot> getNodeStatus(
        const std::string& plan_id, const std::string& pid_or_node_id,
        const CallContext& call) const = 0;

    virtual Result<TaskEventPage> subscribeEvents(
        const EventSubscriptionRequest& request,
        const CallContext& call) const = 0;

    virtual Status ackEvents(
        const EventAckRequest& request,
        const CallContext& call) = 0;

    virtual OrchestratorHealth health(
        HealthDetailLevel detail,
        const CallContext& call) const = 0;

    /// Advances at most one logical scheduler transition.
    virtual bool pumpOne() = 0;

    /// Drives a plan to a terminal state or fails when max_steps is exhausted.
    virtual Status runUntilPlanTerminal(
        const std::string& plan_id,
        std::size_t max_steps = 10000) = 0;

    virtual std::vector<TaskEvent> events() const = 0;
};

class Orchestrator final : public IOrchestrator {
public:
    Orchestrator(
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
        std::shared_ptr<agent_dispatch::IAgentDispatch> dispatch,
        std::filesystem::path storage_directory = {});

    ValidationResult validateDAG(
        const IntentDAG& dag, const AdmissionContext& admission,
        const CallContext& call) const override;

    PlanCommitResult submit(
        const OrchestratorSubmitRequest& request,
        const CallContext& call) override;

    CapacityAck updateDispatchCapacity(
        const DispatchCapacity& capacity,
        const CallContext& call) override;

    ExecutionEventAck ackDispatch(
        const DispatchAcceptance& acceptance,
        const CallContext& call) override;

    ExecutionEventAck ackExecutionStarted(
        const ExecutionStarted& started,
        const CallContext& call) override;

    ExecutionCommitResult completeExecution(
        const NodeExecutionResult& result,
        const CallContext& call) override;

    SignalCommitResult publishSignal(
        const SignalEvent& signal,
        const CallContext& call) override;

    ControlCommitResult suspend(
        const PlanControlRequest& request,
        const CallContext& call) override;

    ControlCommitResult resume(
        const PlanControlRequest& request,
        const CallContext& call) override;

    ControlCommitResult cancelPlan(
        const PlanCancelRequest& request,
        const CallContext& call) override;

    ControlCommitResult cancelActivation(
        const ActivationCancelRequest& request,
        const CallContext& call) override;

    CompensationCommitResult rollback(
        const RollbackRequest& request,
        const CallContext& call) override;

    Result<TaskPlanSnapshot> getPlan(
        const std::string& plan_id,
        const CallContext& call) const override;

    Result<NodeRuntimeSnapshot> getNodeStatus(
        const std::string& plan_id, const std::string& pid_or_node_id,
        const CallContext& call) const override;

    Result<TaskEventPage> subscribeEvents(
        const EventSubscriptionRequest& request,
        const CallContext& call) const override;

    Status ackEvents(
        const EventAckRequest& request,
        const CallContext& call) override;

    OrchestratorHealth health(
        HealthDetailLevel detail,
        const CallContext& call) const override;

    bool pumpOne() override;

    Status runUntilPlanTerminal(
        const std::string& plan_id,
        std::size_t max_steps = 10000) override;

    std::vector<TaskEvent> events() const override;

private:
    struct PlanRuntime {
        TaskPlanSnapshot snapshot;
        AdmissionContext admission;
        atomic_service::McpToolCatalogSnapshot catalog;
        std::string submit_digest;
        std::string ledger_key;
        std::uint64_t ready_sequence = 0;
        std::map<std::string, std::uint64_t> node_ready_sequence;
        std::vector<DAGEdge> edges;
        std::vector<TaskEvent> event_history;
    };

    struct DurablePlanRecord {
        PlanRuntime runtime;
        std::uint64_t ready_sequence = 0;
        std::uint64_t fencing_sequence = 1;
    };

    struct Observation {
        std::string plan_id;
        std::string node_id;
        std::string pid;
        std::string activation_id;
        std::string execution_id;
        std::string operation_id;
        std::uint32_t attempt_count = 0;
        std::uint64_t fencing_token = 0;
        ActivationState state = ActivationState::Running;
        nlohmann::json result = nlohmann::json::object();
        std::string error_code;
        SideEffectState side_effect_state = SideEffectState::NotStarted;
        bool retryable_hint = false;
    };

    void refreshBlockers(PlanRuntime& plan);

    std::optional<std::pair<std::string, std::string>>
    selectReady() const;

    void applyObservation(const Observation& observation);

    void settlePlan(PlanRuntime& plan);

    /// Archives terminal activations and schedules the next bounded trigger.
    /// Caller must hold mutex_. Returns true when at least one node was
    /// returned to a non-terminal waiting state.
    bool advanceRecurringActivationsUnlocked(PlanRuntime& plan);

    void emit(PlanRuntime& plan, const NodeRuntimeSnapshot* node,
              const std::string& event_type);

    Status validateControlRequest(
        const PlanControlRequest& request, const CallContext& call,
        const PlanRuntime& plan) const;

    std::optional<std::pair<std::string, std::string>>
    findExecutionUnlocked(const std::string& execution_id) const;

    bool bindResultsUnlocked(
        const PlanRuntime& plan, const NodeRuntimeSnapshot& node,
        nlohmann::json* params, std::string* error_code) const;

    /// Durable Plan/Activation snapshot store. Each state transition is
    /// appended and fsynced before it can be observed as committed.
    Status persistPlanUnlocked(const PlanRuntime& plan);
    Status recoverDurableState();

    static bool acyclic(const IntentDAG& dag);

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic_;
    std::shared_ptr<agent_dispatch::IAgentDispatch> dispatch_;
    // The scheduler is a single logical actor.  Serializing pump turns
    // keeps the prepare/external-dispatch/publish generation reservation
    // valid while getPlan and submit remain independently concurrent.
    mutable std::mutex pump_mutex_;
    mutable std::mutex mutex_;
    std::map<std::string, PlanRuntime> plans_;
    std::map<std::string, std::string> idempotency_to_plan_;
    std::map<std::string, std::string> idempotency_digest_;
    std::vector<TaskEvent> events_;
    std::map<std::string, DispatchCapacity> dispatch_capacity_;
    std::set<std::string> seen_signal_events_;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
        signal_cursors_;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>>
        event_acks_;
    std::uint64_t ready_sequence_ = 0;
    std::uint64_t fencing_sequence_ = 1;
    std::filesystem::path storage_directory_;
    std::filesystem::path durable_wal_path_;
    Status durability_status_;
    std::uint64_t durable_sequence_ = 0;
    std::string durable_chain_head_ = "GENESIS";
    std::map<std::string, DurablePlanRecord>
        recovered_plan_records_;
};

}  // namespace master_agent::orchestrator
