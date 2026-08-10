#pragma once

/**
 * @file remote_services.h
 * @brief Defines typed remote proxies for cross-process module calls.
 */

#include <atomic>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/transport/ipc/ipc.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/orchestrator/orchestrator.h"

namespace master_agent::ipc {

struct RpcReply {
    Status status;
    nlohmann::json value = nullptr;
};

/**
 * Authenticated request builder shared by module-specific proxies.
 *
 * Remote service classes contain no business policy. They perform only
 * encode/call/decode translation and preserve the underlying module contract.
 */
class RpcClient {
public:
    RpcClient(IpcEndpoint source,
                 IpcEndpoint target,
                 std::shared_ptr<IRuntimeClock> clock);

    /// Performs one deadline-bounded RPC and returns the remote business status.
    RpcReply call(
        const std::string& operation,
        const nlohmann::json& value,
        const CallContext& call,
        std::uint64_t fencing_token = 0) const;

private:
    IpcEndpoint source_;
    IpcEndpoint target_;
    std::shared_ptr<IRuntimeClock> clock_;
    mutable std::atomic<std::uint64_t> sequence_{1};
};

/// Builds the standard response payload and immutable response identity.
IpcEnvelope makeRpcResponse(
    const IpcEnvelope& request,
    std::uint64_t source_process_epoch,
    const Status& status,
    nlohmann::json value = nullptr);

/// Authenticates source endpoint/epoch through the Supervisor registry,
/// verifies the operation-specific endpoint/module allowlist and reissues a
/// local trusted CallContext for existing module guards.
Result<CallContext> authorizeRpcCall(
    const IpcEnvelope& request,
    const ProcessRegistry& registry,
    const std::set<std::string>& allowed_source_endpoints,
    const std::set<CallerModuleId>& allowed_callers,
    bool preserve_observability_producer_identity = false);

class RemoteMemoryService final
    : public memory::IMemoryService {
public:
    explicit RemoteMemoryService(
        std::shared_ptr<RpcClient> rpc);
    Result<memory::MemoryContext> getContext(
        const interaction::StandardRequest& request,
        const std::string& normalized_query,
        const CallContext& call) override;
    Status writeTurn(const memory::CompletedTurn& turn,
                     const CallContext& call) override;
private:
    std::shared_ptr<RpcClient> rpc_;
};

class RemoteAtomicService final
    : public atomic_service::IAtomicServiceManager {
public:
    explicit RemoteAtomicService(
        std::shared_ptr<RpcClient> rpc);
    Result<std::vector<atomic_service::McpToolDefinition>>
    listTools(const CallContext& call) const override;
    Result<atomic_service::McpToolCatalogSnapshot>
    getToolCatalogSnapshot(
        const CallContext& call) const override;
    atomic_service::DispatchAcceptance callTool(
        const atomic_service::AtomicMcpCallEnvelope& request,
        const CallContext& call) override;
    Result<atomic_service::AtomicExecutionSnapshot>
    queryExecution(const std::string& id,
                   const CallContext& call) const override;
    Status requestPreempt(const std::string& execution_id,
                          TaskPriority arriving_priority,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;
    Result<atomic_service::AtomicReconcileResult>
    reconcileExecution(const std::string& operation_id,
                       const CallContext& call) override;
    bool pumpOne() override;
    Status runUntilIdle(std::size_t max_steps = 10000) override;
    std::vector<atomic_service::AtomicExecutionEvent>
    events() const override;
private:
    std::shared_ptr<RpcClient> rpc_;
};

class RemoteAgentDispatch final
    : public agent_dispatch::IAgentDispatch {
public:
    explicit RemoteAgentDispatch(
        std::shared_ptr<RpcClient> rpc);
    Status registerAgent(
        std::shared_ptr<sub_agents::ISubAgent> agent,
        const CallContext& call) override;
    agent_dispatch::DispatchAcceptance submitDispatch(
        const agent_dispatch::DispatchTask& task,
        const CallContext& call) override;
    Result<agent_dispatch::DispatchSnapshot> queryDispatch(
        const std::string& id,
        const CallContext& call) const override;
    Result<agent_dispatch::DispatchSnapshot> reconcileDispatch(
        const std::string& operation_id,
        std::uint64_t expected_agent_epoch,
        const CallContext& call) override;
    Status requestPreempt(const std::string& dispatch_id,
                          TaskPriority arriving_priority,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;
    bool pumpOne() override;
    Status runUntilIdle(std::size_t max_steps = 10000) override;
    std::vector<agent_dispatch::DispatchEvent>
    events() const override;
    agent_dispatch::AgentDispatchCapacity getCapacity(
        const CallContext& call) const override;
private:
    std::shared_ptr<RpcClient> rpc_;
};

class RemoteInferenceFramework final
    : public inference::IInferenceFramework {
public:
    explicit RemoteInferenceFramework(
        std::shared_ptr<RpcClient> rpc);
    inference::InferenceAcceptance submitInference(
        const inference::InferenceRequest& request,
        const CallContext& call) override;
    Result<inference::InferenceJobSnapshot> queryInference(
        const std::string& job_id,
        const CallContext& call) const override;
    Status cancelInference(const std::string& job_id,
                           std::uint64_t control_epoch,
                           const CallContext& call) override;
    Status requestPreempt(const std::string& job_id,
                          TaskPriority arriving_priority,
                          std::uint64_t control_epoch,
                          const CallContext& call) override;
    Status rebuildReplica(const std::string& replica_id,
                          const CallContext& call) override;
    bool pumpOne() override;
    Status runUntilIdle(std::size_t max_steps = 10000) override;
    std::vector<inference::InferenceEvent> events() const override;
private:
    std::shared_ptr<RpcClient> rpc_;
};

class RemoteOrchestrator final
    : public orchestrator::IOrchestrator {
public:
    explicit RemoteOrchestrator(
        std::shared_ptr<RpcClient> rpc);
    orchestrator::ValidationResult validateDAG(
        const orchestrator::IntentDAG& dag,
        const orchestrator::AdmissionContext& admission,
        const CallContext& call) const override;
    orchestrator::PlanCommitResult submit(
        const orchestrator::OrchestratorSubmitRequest& request,
        const CallContext& call) override;
    Result<orchestrator::TaskPlanSnapshot> getPlan(
        const std::string& plan_id,
        const CallContext& call) const override;
    bool pumpOne() override;
    Status runUntilPlanTerminal(
        const std::string& plan_id,
        std::size_t max_steps = 10000) override;
    std::vector<orchestrator::TaskEvent> events() const override;
private:
    std::shared_ptr<RpcClient> rpc_;
};

class RemoteDataLogService final
    : public data_log::IDataLogService {
public:
    explicit RemoteDataLogService(
        std::shared_ptr<RpcClient> rpc);
    Result<data_log::LogAppendResult> appendEvents(
        const data_log::LogEventBatch& batch,
        const CallContext& call) override;
    Result<data_log::AuditAppendResult> appendAudit(
        const data_log::AuditBatch& batch,
        const CallContext& call) override;
    Result<data_log::TracePage> queryTrace(
        const data_log::TraceQuery& query,
        const CallContext& call) const override;
    Status flush(const CallContext& call) override;
    data_log::LogHealth getHealth(
        const CallContext& call) const override;
private:
    std::shared_ptr<RpcClient> rpc_;
};

class RemoteExceptionManager final
    : public exception::IExceptionManager {
public:
    explicit RemoteExceptionManager(
        std::shared_ptr<RpcClient> rpc);
    Result<exception::ExceptionReportResult> report(
        const exception::ExceptionReportRequest& request,
        const CallContext& call) override;
    Result<exception::ExceptionGroup> getException(
        const std::string& exception_id,
        const CallContext& call) const override;
    Result<exception::ExceptionMutationResult> acknowledge(
        const exception::ExceptionMutationRequest& request,
        const CallContext& call) override;
    Result<exception::ExceptionMutationResult> markMitigating(
        const exception::ExceptionMutationRequest& request,
        const CallContext& call) override;
    Result<exception::ExceptionMutationResult> resolve(
        const exception::ExceptionMutationRequest& request,
        const CallContext& call) override;
private:
    Result<exception::ExceptionMutationResult> mutate(
        const std::string& operation,
        const exception::ExceptionMutationRequest& request,
        const CallContext& call);
    std::shared_ptr<RpcClient> rpc_;
};

}  // namespace master_agent::ipc
