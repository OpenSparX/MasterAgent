#pragma once

/**
 * @file atomic_service.h
 * @brief Defines MCP tool admission, execution, preemption, and recovery contracts.
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

#include "master_agent/common/types.h"

namespace master_agent::atomic_service {

struct McpToolAnnotations {
    std::string title;
    bool read_only_hint = false;
    bool destructive_hint = false;
    bool idempotent_hint = false;
    bool open_world_hint = false;
};

struct McpToolDefinition {
    std::string name;
    std::string title;
    std::string description;
    nlohmann::json input_schema;
    nlohmann::json output_schema;
    McpToolAnnotations annotations;
};

enum class CompletionPolicy : std::uint8_t {
    ReturnConfirmed,
    ProviderAccepted,
    EventConfirmed,
    StateVerified,
    TransactionReceipt
};

enum class CompletionEvidence : std::uint8_t {
    None,
    ReturnConfirmed,
    ProviderAccepted,
    EventConfirmed,
    StateVerified,
    TransactionReceipt
};

struct AtomicToolRuntimePolicy {
    std::string tool_name;
    std::string tool_contract_version = "1";
    std::string tool_digest;
    std::string policy_digest;
    std::vector<std::string> required_permissions;
    std::vector<std::string> resource_argument_fields;
    std::string idempotency_policy = "TARGET_STATE";
    std::vector<std::string> retryable_errors;
    CompletionPolicy completion_policy = CompletionPolicy::ReturnConfirmed;
    std::string cancel_model = "SAFE_POINT";
    bool supports_preemption = true;
    bool supports_reconcile = true;
    std::uint32_t max_concurrency = 1;
    std::uint32_t simulated_work_units = 1;
};

struct McpToolCatalogSnapshot {
    std::string snapshot_id;
    std::string mcp_protocol_version = "2025-06-18";
    std::uint64_t catalog_generation = 0;
    std::string catalog_digest;
    std::int64_t created_at_utc_ms = 0;
    std::vector<McpToolDefinition> tools;
    std::map<std::string, std::string> tool_digests;
    std::map<std::string, std::string> policy_digests;
    std::map<std::string, std::string> idempotency_policies;
    std::map<std::string, std::vector<std::string>>
        retryable_errors;
    std::map<std::string, std::string> provider_ids;
    std::map<std::string, std::uint64_t> provider_epochs;
};

struct McpCallToolRequest {
    std::string jsonrpc = "2.0";
    std::string id;
    std::string method = "tools/call";
    std::string name;
    nlohmann::json arguments = nlohmann::json::object();
};

struct AtomicRuntimeContext {
    CallerModuleId caller_module_id =
        CallerModuleId::TaskOrchestrationEngine;
    std::string request_id;
    std::string trace_id;
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::uint32_t attempt_no = 1;
    std::string operation_id;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::string idempotency_key;
    std::uint64_t fencing_token = 0;
    std::string tool_catalog_snapshot_id;
    std::string tool_digest;
    std::string policy_digest;
    // Frozen authorization claims resolved by AgentService Admission.  The
    // target platform replaces this host claim transport with IAM evidence.
    std::vector<std::string> granted_permissions;
    std::vector<std::string> resource_lease_refs;
    std::string principal_id_hash;
    std::string authorization_ref;
    std::optional<std::string> parent_operation_id;
    // Required only for SubAgent child calls. These fields echo the frozen
    // parent Dispatch/AgentLease so Agent Dispatch can validate lineage.
    std::string parent_dispatch_id;
    std::string parent_agent_id;
    std::uint64_t parent_agent_epoch = 0;
    std::string parent_lease_id;
    std::uint64_t parent_fencing_token = 0;
};

struct AtomicMcpCallEnvelope {
    McpCallToolRequest mcp_request;
    AtomicRuntimeContext runtime;
};

/// Read-only parent-owner seam used before a SubAgent child Tool is admitted.
/// A multi-process integration may use authenticated IPC or signed lease
/// claims; the in-process runtime binds it directly to Agent Dispatch.
class IAtomicParentLineageValidator {
public:
    virtual ~IAtomicParentLineageValidator() = default;

    virtual Status validateAtomicParentLineage(
        const AtomicMcpCallEnvelope& request,
        const CallContext& call) const = 0;

    /// Atomically validates the live parent AgentLease and freezes a
    /// short-lived authorization for one physical Provider invocation.
    /// The default only proves lineage; AgentDispatch overrides it with an
    /// actual reservation ledger.
    virtual Result<std::string> acquireAtomicParentInvocationLease(
        const AtomicMcpCallEnvelope& request,
        const CallContext& call) {
        const auto validated =
            validateAtomicParentLineage(request, call);
        if (!validated.ok) {
            return Result<std::string>::Failure(validated);
        }
        return Result<std::string>::Success(
            "validated-parent-lease|" +
            request.runtime.execution_id);
    }

    /// Idempotent release hook for the short-lived invocation reservation.
    virtual Status releaseAtomicParentInvocationLease(
        const std::string&) {
        return Status::Ok();
    }
};

/// Immutable identity passed across the ProviderAdapter boundary and echoed
/// by every callback. It prevents a late or misrouted Provider response from
/// being committed to a different attempt, catalog generation, policy, or
/// fencing lease.
struct AtomicProviderInvocationSeal {
    std::string invocation_id;
    std::string provider_id;
    std::uint64_t provider_epoch = 0;
    std::string operation_id;
    std::string execution_id;
    std::uint32_t attempt_no = 0;
    std::string tool_name;
    std::string tool_catalog_snapshot_id;
    std::string tool_digest;
    std::string policy_digest;
    std::uint64_t fencing_token = 0;
    std::string request_digest;
};

struct CallToolResult {
    std::vector<std::string> text_content;
    nlohmann::json structured_content = nlohmann::json::object();
    bool is_error = false;
};

/**
 * @brief Bounded side-effect-free query used by the Intent QUERY_BATCH path.
 *
 * The target must be a registered MCP Tool whose annotations and immutable
 * runtime policy both declare READ_ONLY. `expected_catalog_digest` prevents a
 * model decision made against one catalog from being executed against another.
 */
struct AtomicReadOnlyMcpRequest {
    McpCallToolRequest mcp_request;
    std::string expected_catalog_digest;
    std::vector<std::string> granted_permissions;
};

struct AtomicReadOnlyResult {
    std::string query_id;
    std::string tool_name;
    CallToolResult result;
    std::string catalog_snapshot_id;
    std::string tool_digest;
    std::string policy_digest;
    std::string provider_id;
    std::uint64_t provider_epoch = 0;
    std::int64_t observed_at_utc_ms = 0;
};

enum class AtomicExecutionState : std::uint8_t {
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
    std::string operation_id;
    std::string execution_id;
    std::string reject_code;
    std::string executor_id = "atomic-service-manager";
    std::uint64_t executor_epoch = 1;
};

struct AtomicExecutionEvent {
    std::string event_id;
    std::string event_type;
    std::string request_id;
    std::string trace_id;
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::uint32_t attempt_no = 0;
    std::string operation_id;
    std::string mcp_request_id;
    std::string tool_name;
    AtomicExecutionState state = AtomicExecutionState::Accepted;
    std::uint64_t fencing_token = 0;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    CompletionEvidence completion_evidence = CompletionEvidence::None;
    std::optional<CallToolResult> call_tool_result;
    std::string error_code;
    bool retryable_hint = false;
    bool safe_point = false;
    bool resource_released = false;
    std::int64_t occurred_at_utc_ms = 0;
};

struct AtomicExecutionSnapshot {
    AtomicMcpCallEnvelope envelope;
    AtomicExecutionState state = AtomicExecutionState::Accepted;
    std::optional<CallToolResult> result;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    CompletionEvidence completion_evidence = CompletionEvidence::None;
    std::string error_code;
    std::string resource_key;
    std::uint32_t remaining_work_units = 0;
    std::uint64_t control_epoch = 0;
    std::optional<AtomicProviderInvocationSeal> provider_invocation;
    bool retryable_hint = false;
};

enum class ReconcileStatus : std::uint8_t {
    ConfirmedSuccess,
    ConfirmedNotExecuted,
    ConfirmedFailure,
    StillUnknown
};

struct AtomicReconcileResult {
    std::string operation_id;
    std::string execution_id;
    std::string tool_name;
    ReconcileStatus status = ReconcileStatus::StillUnknown;
    nlohmann::json observed_state;
    std::uint64_t fencing_token = 0;
    std::optional<CallToolResult> call_tool_result;
    CompletionEvidence completion_evidence = CompletionEvidence::None;
    SideEffectState side_effect_state = SideEffectState::Unknown;
    AtomicProviderInvocationSeal invocation_seal;
    bool retryable_hint = false;
};

enum class ProviderInvocationState : std::uint8_t {
    Succeeded,
    Failed,
    Unknown
};

struct ProviderInvocationResult {
    ProviderInvocationState state = ProviderInvocationState::Failed;
    CallToolResult result;
    SideEffectState side_effect_state = SideEffectState::NotStarted;
    std::string error_code;
    CompletionEvidence completion_evidence = CompletionEvidence::None;
    AtomicProviderInvocationSeal invocation_seal;
    bool retryable_hint = false;
};

/**
 * @brief Platform adapter for one registered atomic capability.
 *
 * Provider callbacks must echo the immutable invocation seal. A late callback
 * from an older attempt, provider epoch, or fencing token must not be committed.
 */
class IAtomicProvider {
public:
    virtual ~IAtomicProvider() = default;

    /**
     * Performs one physical provider invocation.
     *
     * The returned side-effect state and completion evidence are authoritative.
     * Implementations must echo invocation_seal unchanged.
     */
    virtual ProviderInvocationResult call(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) = 0;

    /**
     * Observes the outcome of an ambiguous invocation without executing it again.
     *
     * Reconciliation is mandatory when call() returns Unknown because transport
     * failure alone cannot prove whether the external side effect occurred.
     */
    virtual AtomicReconcileResult reconcile(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) = 0;
};

class DeterministicClimateProvider final : public IAtomicProvider {
public:
    ProviderInvocationResult call(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) override;

    AtomicReconcileResult reconcile(
        const AtomicMcpCallEnvelope& envelope,
        const AtomicProviderInvocationSeal& invocation_seal) override;

    void setNextInvocationState(ProviderInvocationState state);

    void setUnknownReconcileStatus(ReconcileStatus status);

    std::size_t invocationCount() const;

    std::size_t reconciliationCount() const;

private:
    mutable std::mutex mutex_;
    ProviderInvocationState next_state_ = ProviderInvocationState::Succeeded;
    ReconcileStatus unknown_reconcile_status_ =
        ReconcileStatus::ConfirmedSuccess;
    std::map<std::string, CallToolResult> operation_results_;
    std::size_t invocation_count_ = 0;
    std::size_t reconciliation_count_ = 0;
};

/**
 * @brief Governs MCP tool discovery, admission, execution, and reconciliation.
 *
 * callTool returns an admission record, not proof of completion. Unknown provider
 * outcomes require reconciliation and must never trigger an unconditional retry.
 */
class IAtomicServiceManager {
public:
    virtual ~IAtomicServiceManager() = default;

    /// Returns the currently published MCP definitions without side effects.
    virtual Result<std::vector<McpToolDefinition>> listTools(
        const CallContext& call) const = 0;

    /// Freezes the catalog generation used to validate a later callTool request.
    virtual Result<McpToolCatalogSnapshot> getToolCatalogSnapshot(
        const CallContext& call) const = 0;

    /**
     * Executes exactly one registered read-only MCP capability. This surface
     * is intentionally separate from callTool: it cannot admit a physical
     * control operation or create an Orchestrator execution identity.
     */
    virtual Result<AtomicReadOnlyResult> queryReadOnly(
        const AtomicReadOnlyMcpRequest& query,
        const CallContext& call) {
        (void)query;
        (void)call;
        return Result<AtomicReadOnlyResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_READ_ONLY_QUERY_UNSUPPORTED",
            "read-only query is not implemented"));
    }

    /**
     * Durably admits an MCP call after schema, policy, lineage, and deadline
     * validation. An accepted response identifies the execution; it does not
     * assert that the provider has run or that the tool succeeded.
     */
    virtual DispatchAcceptance callTool(
        const AtomicMcpCallEnvelope& request,
        const CallContext& call) = 0;

    /// Returns the authoritative execution state by execution or operation ID.
    virtual Result<AtomicExecutionSnapshot> queryExecution(
        const std::string& execution_or_operation_id,
        const CallContext& call) const = 0;

    /**
     * Requests cooperative preemption at the next tool-defined safe point.
     * control_epoch must increase monotonically; stale requests are rejected.
     */
    virtual Status requestPreempt(const std::string& execution_id,
                                  TaskPriority arriving_priority,
                                  std::uint64_t control_epoch,
                                  const CallContext& call) = 0;

    /**
     * Resolves an Unknown execution from provider evidence. This operation must
     * not turn an ambiguous side effect into an unconditional provider retry.
     */
    virtual Result<AtomicReconcileResult> reconcileExecution(
        const std::string& operation_id,
        const CallContext& call) = 0;

    /// Advances at most one scheduler transition; intended for owner-loop driving.
    virtual bool pumpOne() = 0;

    /// Drives the deterministic scheduler until quiescent or max_steps is reached.
    virtual Status runUntilIdle(std::size_t max_steps = 10000) = 0;

    virtual std::vector<AtomicExecutionEvent> events() const = 0;
};

class AtomicServiceManager final : public IAtomicServiceManager {
public:
    AtomicServiceManager(std::shared_ptr<IRuntimeClock> clock,
                            std::shared_ptr<IdGenerator> ids,
                            std::size_t max_inflight = 1,
                            std::shared_ptr<
                                IAtomicParentLineageValidator>
                                lineage_validator = nullptr,
                            std::filesystem::path storage_directory = {});

    Status registerTools(
        const std::vector<McpToolDefinition>& tools,
        const std::vector<AtomicToolRuntimePolicy>& policies,
        std::shared_ptr<IAtomicProvider> provider,
        const CallContext& call);

    Result<std::vector<McpToolDefinition>> listTools(
        const CallContext& call) const override;

    Result<McpToolCatalogSnapshot> getToolCatalogSnapshot(
        const CallContext& call) const override;

    Result<AtomicReadOnlyResult> queryReadOnly(
        const AtomicReadOnlyMcpRequest& query,
        const CallContext& call) override;

    DispatchAcceptance callTool(
        const AtomicMcpCallEnvelope& request,
        const CallContext& call) override;

    Result<AtomicExecutionSnapshot> queryExecution(
        const std::string& execution_or_operation_id,
        const CallContext& call) const override;

    Status requestPreempt(const std::string& execution_id,
                          TaskPriority arriving_priority,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;

    Result<AtomicReconcileResult> reconcileExecution(
        const std::string& operation_id,
        const CallContext& call) override;

    bool pumpOne() override;

    Status runUntilIdle(std::size_t max_steps = 10000) override;

    std::vector<AtomicExecutionEvent> events() const override;

private:
    struct ToolRecord {
        McpToolDefinition definition;
        AtomicToolRuntimePolicy policy;
        std::shared_ptr<IAtomicProvider> provider;
        std::string provider_id;
        std::uint64_t provider_epoch = 0;
    };

    struct ResourceFenceOwnership {
        std::uint64_t fencing_token = 0;
        std::string parent_dispatch_id;
        std::string parent_lease_id;
    };

    struct DurableExecutionRecord {
        AtomicExecutionSnapshot snapshot;
        std::string ledger_key;
        std::string request_digest;
        std::uint64_t queue_sequence = 0;
    };

    static Status validateArguments(const nlohmann::json& schema,
                                    const nlohmann::json& arguments);

    static std::string resourceKey(
        const AtomicToolRuntimePolicy& policy,
        const nlohmann::json& arguments);

    Status validateCall(const AtomicMcpCallEnvelope& request,
                        const CallContext& call,
                        const ToolRecord** record,
                        bool allow_recovered_catalog_snapshot =
                            false) const;

    std::optional<std::string> selectQueued() const;

    std::optional<std::string> selectVictim(
        TaskPriority arriving,
        const std::optional<std::string>& tool_name = std::nullopt) const;

    void emit(AtomicExecutionSnapshot& snapshot,
              const std::string& event_type,
              bool safe_point = false,
              bool resource_released = false);

    /// Appends and fsyncs a checksum-protected execution snapshot. Admission
    /// and the Provider invocation seal use this before their respective
    /// externally visible/physical boundaries.
    Status persistExecutionUnlocked(
        const AtomicExecutionSnapshot& snapshot,
        const std::string& ledger_key,
        const std::string& request_digest,
        std::uint64_t queue_sequence);

    /// Reloads the checksum chain. A torn/corrupt final record is truncated;
    /// corruption before the final record poisons the manager fail-closed.
    Status recoverDurableState();

    /// Binds recovered immutable Tool digests to the newly registered
    /// Provider adapter and rebuilds idempotency, fencing, and operation
    /// indexes. A sealed non-terminal invocation is always recovered UNKNOWN.
    Status activateRecoveredExecutionsUnlocked(
        const std::map<std::string, ToolRecord>& registered_tools);

    Status persistCurrentExecutionUnlocked(
        const std::string& execution_id);

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<IAtomicParentLineageValidator>
        lineage_validator_;
    std::size_t max_inflight_;
    mutable std::mutex mutex_;
    std::map<std::string, ToolRecord> tools_;
    McpToolCatalogSnapshot catalog_;
    std::map<std::string, AtomicExecutionSnapshot> executions_;
    std::map<std::string, ToolRecord> execution_tools_;
    std::map<std::string, std::string> operation_to_execution_;
    std::map<std::string, std::string> idempotency_to_execution_;
    std::map<std::string, std::string> idempotency_digest_;
    std::map<std::string, ResourceFenceOwnership>
        highest_fencing_by_resource_;
    std::set<std::string> provider_inflight_;
    // Synchronous read-only calls use the same per-Tool capacity contract but
    // never enter the durable side-effect execution ledger.
    std::map<std::string, std::uint32_t> read_only_inflight_;
    std::set<std::string> reconcile_inflight_;
    std::vector<AtomicExecutionEvent> events_;
    std::uint64_t enqueue_sequence_ = 0;
    std::map<std::string, std::uint64_t> queue_sequence_;
    std::filesystem::path storage_directory_;
    std::filesystem::path durable_wal_path_;
    Status durability_status_;
    std::uint64_t durable_sequence_ = 0;
    std::string durable_chain_head_ = "GENESIS";
    std::map<std::string, DurableExecutionRecord>
        recovered_execution_records_;
};

std::vector<McpToolDefinition> defaultClimateMcpTools();

std::vector<AtomicToolRuntimePolicy> defaultClimateRuntimePolicies(
    std::uint32_t work_units = 1);

}  // namespace master_agent::atomic_service
