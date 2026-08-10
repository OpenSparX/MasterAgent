/**
 * @file remote_services.cpp
 * @brief Implements typed module proxies over the authenticated IPC transport.
 */

#include "master_agent/transport/ipc/remote_services.h"

#include <utility>

#include "master_agent/transport/ipc/wire_codec.h"

namespace master_agent::ipc {
namespace {

CallContext driverCall(CallerModuleId caller) {
    SystemRuntimeClock clock;
    return {caller,
            "rpc-driver",
            "rpc-driver-trace",
            "rpc-driver-principal",
            TaskPriority::P1,
            clock.monotonicNowNs() + 30'000'000'000LL};
}

template <typename T, typename Decoder>
Result<T> decodeReply(const RpcReply& reply,
                      Decoder decoder) {

    if (!reply.status.ok) {
        return Result<T>::Failure(reply.status);
    }
    try {
        return Result<T>::Success(decoder(reply.value));
    } catch (...) {
        return Result<T>::Failure(Status::Error(
            "ipc", "RPC_VALUE_DECODE_FAILED",
            "module response payload failed decoding"));
    }
}

Status statusReply(const RpcReply& reply) {
    return reply.status;
}

}  // namespace

RpcClient::RpcClient(
    IpcEndpoint source, IpcEndpoint target,
    std::shared_ptr<IRuntimeClock> clock)
    : source_(std::move(source)),
      target_(std::move(target)),
      clock_(std::move(clock)) {}

RpcReply RpcClient::call(
    const std::string& operation,
    const nlohmann::json& value,
    const CallContext& call,
    std::uint64_t fencing_token) const {
    if (!clock_ || source_.endpoint_id.empty() ||
        source_.process_epoch == 0 ||
        target_.endpoint_id.empty() ||
        target_.process_epoch == 0) {
        return {Status::Error(
                    "ipc", "RPC_CLIENT_IDENTITY_INVALID",
                    "RPC client source/target identity is incomplete"),
                nullptr};
    }
    const auto sequence = sequence_.fetch_add(1);
    IpcEnvelope request;
    request.message_id =
        source_.endpoint_id + "-" + std::to_string(sequence) +
        "-" + secureDigest(
                  operation + "|" + call.request_id + "|" +
                  std::to_string(sequence))
                  .substr(0, 16);
    request.correlation_id.clear();
    request.source_endpoint_id = source_.endpoint_id;
    request.source_process_epoch = source_.process_epoch;
    request.target_endpoint_id = target_.endpoint_id;
    request.operation = operation;
    request.request_id =
        call.request_id.empty()
            ? "rpc-" + std::to_string(sequence)
            : call.request_id;
    request.trace_id =
        call.trace_id.empty()
            ? "rpc-trace-" + std::to_string(sequence)
            : call.trace_id;
    request.priority = call.priority;
    request.deadline_mono_ns =
        call.deadline_mono_ns > 0
            ? call.deadline_mono_ns
            : clock_->monotonicNowNs() + 30'000'000'000LL;
    request.idempotency_key =
        "rpc|" + source_.endpoint_id + "|" + operation + "|" +
        request.request_id + "|" + std::to_string(sequence);
    request.fencing_token = fencing_token;
    request.reality = "SIMULATED";
    request.payload = {
        {"call", wire::encodeCallContext(call)},
        {"value", value}};
    sealIpcEnvelope(request);
    const auto response =
        IpcClient(target_).call(request);
    if (!response.status.ok || !response.value) {
        return {response.status, nullptr};
    }
    try {
        const auto& payload = response.value->payload;
        if (!payload.is_object() ||
            !payload.contains("status") ||
            !payload.contains("value")) {
            return {Status::Error(
                        "ipc", "RPC_RESPONSE_SCHEMA_INVALID",
                        "RPC response payload is incomplete"),
                    nullptr};
        }
        return {wire::decodeStatus(payload.at("status")),
                payload.at("value")};
    } catch (...) {
        return {Status::Error(
                    "ipc", "RPC_RESPONSE_SCHEMA_INVALID",
                    "RPC response payload failed closed decoding"),
                nullptr};
    }
}

IpcEnvelope makeRpcResponse(
    const IpcEnvelope& request,
    std::uint64_t source_process_epoch,
    const Status& status,
    nlohmann::json value) {
    IpcEnvelope response;
    response.message_id =
        "response-" +
        secureDigest(request.message_id + "|" +
                     std::to_string(source_process_epoch))
            .substr(0, 24);
    response.correlation_id = request.message_id;
    response.source_endpoint_id = request.target_endpoint_id;
    response.source_process_epoch = source_process_epoch;
    response.target_endpoint_id = request.source_endpoint_id;
    response.operation = request.operation + ".response";
    response.request_id = request.request_id;
    response.trace_id = request.trace_id;
    response.session_id = request.session_id;
    response.priority = request.priority;
    response.deadline_mono_ns = request.deadline_mono_ns;
    response.idempotency_key = request.idempotency_key;
    response.fencing_token = request.fencing_token;
    response.reality = request.reality;
    response.payload = {
        {"status", wire::encodeStatus(status)},
        {"value", std::move(value)}};
    sealIpcEnvelope(response);
    return response;
}

Result<CallContext> authorizeRpcCall(
    const IpcEnvelope& request,
    const ProcessRegistry& registry,
    const std::set<std::string>& allowed_source_endpoints,
    const std::set<CallerModuleId>& allowed_callers,
    bool preserve_observability_producer_identity) {
    const auto authenticated = registry.authenticate(
        request.source_endpoint_id,
        request.source_process_epoch);
    if (!authenticated.ok) {
        return Result<CallContext>::Failure(authenticated);
    }
    if (allowed_source_endpoints.count(
            request.source_endpoint_id) == 0) {
        return Result<CallContext>::Failure(Status::Error(
            "ipc", "RPC_SOURCE_NOT_ALLOWED",
            "source process is not allowed for this operation"));
    }
    try {
        const auto semantic = wire::decodeCallContext(
            request.payload.at("call"));
        if (allowed_callers.count(semantic.caller) == 0 ||
            semantic.request_id != request.request_id ||
            semantic.trace_id != request.trace_id ||
            semantic.priority != request.priority ||
            semantic.deadline_mono_ns !=
                request.deadline_mono_ns) {
            return Result<CallContext>::Failure(Status::Error(
                "ipc", "RPC_SEMANTIC_IDENTITY_MISMATCH",
                "semantic caller does not match the IPC envelope"));
        }
        CallContext trusted{
            semantic.caller,
            semantic.request_id,
            semantic.trace_id,
            semantic.principal_id_hash,
            semantic.priority,
            semantic.deadline_mono_ns,
            preserve_observability_producer_identity
                ? semantic.caller_endpoint_id
                : std::string{},
            preserve_observability_producer_identity
                ? semantic.caller_process_epoch
                : 0,
            preserve_observability_producer_identity
                ? "trusted-observability-proxy"
                : semantic.authorization_ref};
        return Result<CallContext>::Success(std::move(trusted));
    } catch (...) {
        return Result<CallContext>::Failure(Status::Error(
            "ipc", "RPC_CALL_CONTEXT_INVALID",
            "RPC semantic call context is malformed"));
    }
}

RemoteMemoryService::RemoteMemoryService(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

Result<memory::MemoryContext>
RemoteMemoryService::getContext(
    const interaction::StandardRequest& request,
    const std::string& normalized_query,
    const CallContext& call) {
    return decodeReply<memory::MemoryContext>(
        rpc_->call(
            "memory.get_context",
            {{"request", wire::encode(request)},
             {"normalized_query", normalized_query}},
            call),
        wire::decodeMemoryContext);
}

Status RemoteMemoryService::writeTurn(
    const memory::CompletedTurn& turn,
    const CallContext& call) {
    return statusReply(rpc_->call(
        "memory.write_turn", wire::encode(turn), call));
}

RemoteAtomicService::RemoteAtomicService(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

Result<std::vector<atomic_service::McpToolDefinition>>
RemoteAtomicService::listTools(
    const CallContext& call) const {
    const auto reply =
        rpc_->call("atomic.list_tools", nlohmann::json::object(),
                   call);
    if (!reply.status.ok) {
        return Result<std::vector<
            atomic_service::McpToolDefinition>>::Failure(
            reply.status);
    }
    try {
        std::vector<atomic_service::McpToolDefinition> result;
        for (const auto& item : reply.value) {
            result.push_back(
                wire::decodeMcpToolDefinition(item));
        }
        return Result<std::vector<
            atomic_service::McpToolDefinition>>::Success(
            std::move(result));
    } catch (...) {
        return Result<std::vector<
            atomic_service::McpToolDefinition>>::Failure(
            Status::Error(
                "ipc", "RPC_VALUE_DECODE_FAILED",
                "atomic tool list failed decoding"));
    }
}

Result<atomic_service::McpToolCatalogSnapshot>
RemoteAtomicService::getToolCatalogSnapshot(
    const CallContext& call) const {
    return decodeReply<
        atomic_service::McpToolCatalogSnapshot>(
        rpc_->call("atomic.get_catalog",
                   nlohmann::json::object(), call),
        wire::decodeToolCatalog);
}

atomic_service::DispatchAcceptance
RemoteAtomicService::callTool(
    const atomic_service::AtomicMcpCallEnvelope& request,
    const CallContext& call) {
    const auto reply = rpc_->call(
        "atomic.call_tool", wire::encode(request), call,
        request.runtime.fencing_token);
    if (!reply.status.ok) {
        return {false, false,
                request.runtime.operation_id,
                request.runtime.execution_id,
                reply.status.error.code};
    }
    try {
        return wire::decodeAtomicAcceptance(reply.value);
    } catch (...) {
        return {false, false,
                request.runtime.operation_id,
                request.runtime.execution_id,
                "RPC_VALUE_DECODE_FAILED"};
    }
}

Result<atomic_service::AtomicExecutionSnapshot>
RemoteAtomicService::queryExecution(
    const std::string& id,
    const CallContext& call) const {
    return decodeReply<
        atomic_service::AtomicExecutionSnapshot>(
        rpc_->call("atomic.query",
                   {{"execution_or_operation_id", id}}, call),
        wire::decodeAtomicSnapshot);
}

Status RemoteAtomicService::requestPreempt(
    const std::string& execution_id,
    TaskPriority arriving_priority,
    std::uint64_t control_epoch,
    const CallContext& call) {
    return statusReply(rpc_->call(
        "atomic.preempt",
        {{"execution_id", execution_id},
         {"arriving_priority", static_cast<int>(arriving_priority)},
         {"control_epoch", control_epoch}},
        call));
}

Result<atomic_service::AtomicReconcileResult>
RemoteAtomicService::reconcileExecution(
    const std::string& operation_id,
    const CallContext& call) {
    return decodeReply<
        atomic_service::AtomicReconcileResult>(
        rpc_->call("atomic.reconcile",
                   {{"operation_id", operation_id}}, call),
        wire::decodeAtomicReconcile);
}

bool RemoteAtomicService::pumpOne() {
    const auto reply = rpc_->call(
        "atomic.pump", nlohmann::json::object(),
        driverCall(CallerModuleId::TaskOrchestrationEngine));
    return reply.status.ok && reply.value.get<bool>();
}

Status RemoteAtomicService::runUntilIdle(
    std::size_t max_steps) {
    return statusReply(rpc_->call(
        "atomic.run", {{"max_steps", max_steps}},
        driverCall(CallerModuleId::TaskOrchestrationEngine)));
}

std::vector<atomic_service::AtomicExecutionEvent>
RemoteAtomicService::events() const {
    // Events remain owned by the vehicle process and are exported through
    // observability, not copied back into the orchestrator heap.
    return {};
}

RemoteAgentDispatch::RemoteAgentDispatch(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

Status RemoteAgentDispatch::registerAgent(
    std::shared_ptr<sub_agents::ISubAgent>,
    const CallContext&) {
    return Status::Error(
        "agent_dispatch", "REMOTE_AGENT_REGISTRATION_FORBIDDEN",
        "provider registration belongs to the dispatch process");
}

agent_dispatch::DispatchAcceptance
RemoteAgentDispatch::submitDispatch(
    const agent_dispatch::DispatchTask& task,
    const CallContext& call) {
    const auto reply = rpc_->call(
        "dispatch.submit", wire::encode(task), call,
        task.fencing_token);
    if (!reply.status.ok) {
        return {false, false, {}, task.operation_id,
                reply.status.error.code};
    }
    try {
        return wire::decodeDispatchAcceptance(reply.value);
    } catch (...) {
        return {false, false, {}, task.operation_id,
                "RPC_VALUE_DECODE_FAILED"};
    }
}

Result<agent_dispatch::DispatchSnapshot>
RemoteAgentDispatch::queryDispatch(
    const std::string& id,
    const CallContext& call) const {
    return decodeReply<agent_dispatch::DispatchSnapshot>(
        rpc_->call("dispatch.query",
                   {{"dispatch_or_operation_id", id}}, call),
        wire::decodeDispatchSnapshot);
}

Result<agent_dispatch::DispatchSnapshot>
RemoteAgentDispatch::reconcileDispatch(
    const std::string& operation_id,
    std::uint64_t expected_agent_epoch,
    const CallContext& call) {
    return decodeReply<agent_dispatch::DispatchSnapshot>(
        rpc_->call(
            "dispatch.reconcile",
            {{"operation_id", operation_id},
             {"expected_agent_epoch", expected_agent_epoch}},
            call),
        wire::decodeDispatchSnapshot);
}

Status RemoteAgentDispatch::requestPreempt(
    const std::string& dispatch_id,
    TaskPriority arriving_priority,
    std::uint64_t control_epoch,
    const CallContext& call) {
    return statusReply(rpc_->call(
        "dispatch.preempt",
        {{"dispatch_id", dispatch_id},
         {"arriving_priority", static_cast<int>(arriving_priority)},
         {"control_epoch", control_epoch}},
        call));
}

bool RemoteAgentDispatch::pumpOne() {
    const auto reply = rpc_->call(
        "dispatch.pump", nlohmann::json::object(),
        driverCall(CallerModuleId::TaskOrchestrationEngine));
    return reply.status.ok && reply.value.get<bool>();
}

Status RemoteAgentDispatch::runUntilIdle(
    std::size_t max_steps) {
    return statusReply(rpc_->call(
        "dispatch.run", {{"max_steps", max_steps}},
        driverCall(CallerModuleId::TaskOrchestrationEngine)));
}

std::vector<agent_dispatch::DispatchEvent>
RemoteAgentDispatch::events() const {
    return {};
}

agent_dispatch::AgentDispatchCapacity
RemoteAgentDispatch::getCapacity(
    const CallContext& call) const {
    const auto reply = rpc_->call(
        "dispatch.capacity", nlohmann::json::object(), call);
    if (!reply.status.ok) {
        agent_dispatch::AgentDispatchCapacity result;
        result.health_state = "UNAVAILABLE";
        return result;
    }
    try {
        return wire::decodeDispatchCapacity(reply.value);
    } catch (...) {
        agent_dispatch::AgentDispatchCapacity result;
        result.health_state = "INVALID_RESPONSE";
        return result;
    }
}

RemoteInferenceFramework::RemoteInferenceFramework(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

inference::InferenceAcceptance
RemoteInferenceFramework::submitInference(
    const inference::InferenceRequest& request,
    const CallContext& call) {
    const auto reply = rpc_->call(
        "inference.submit", wire::encode(request), call);
    if (!reply.status.ok) {
        return {false, false, request.job_id,
                reply.status.error.code};
    }
    try {
        return wire::decodeInferenceAcceptance(reply.value);
    } catch (...) {
        return {false, false, request.job_id,
                "RPC_VALUE_DECODE_FAILED"};
    }
}

Result<inference::InferenceJobSnapshot>
RemoteInferenceFramework::queryInference(
    const std::string& job_id,
    const CallContext& call) const {
    return decodeReply<inference::InferenceJobSnapshot>(
        rpc_->call("inference.query", {{"job_id", job_id}},
                   call),
        wire::decodeInferenceSnapshot);
}

Status RemoteInferenceFramework::cancelInference(
    const std::string& job_id,
    std::uint64_t control_epoch,
    const CallContext& call) {
    return statusReply(rpc_->call(
        "inference.cancel",
        {{"job_id", job_id},
         {"control_epoch", control_epoch}},
        call));
}

Status RemoteInferenceFramework::requestPreempt(
    const std::string& job_id,
    TaskPriority arriving_priority,
    std::uint64_t control_epoch,
    const CallContext& call) {
    return statusReply(rpc_->call(
        "inference.preempt",
        {{"job_id", job_id},
         {"arriving_priority", static_cast<int>(arriving_priority)},
         {"control_epoch", control_epoch}},
        call));
}

Status RemoteInferenceFramework::rebuildReplica(
    const std::string& replica_id,
    const CallContext& call) {
    return statusReply(rpc_->call(
        "inference.rebuild_replica",
        {{"replica_id", replica_id}}, call));
}

bool RemoteInferenceFramework::pumpOne() {
    const auto reply = rpc_->call(
        "inference.pump", nlohmann::json::object(),
        driverCall(CallerModuleId::IntentRecognitionEngine));
    return reply.status.ok && reply.value.get<bool>();
}

Status RemoteInferenceFramework::runUntilIdle(
    std::size_t max_steps) {
    return statusReply(rpc_->call(
        "inference.run", {{"max_steps", max_steps}},
        driverCall(CallerModuleId::IntentRecognitionEngine)));
}

std::vector<inference::InferenceEvent>
RemoteInferenceFramework::events() const {
    return {};
}

RemoteOrchestrator::RemoteOrchestrator(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

orchestrator::ValidationResult
RemoteOrchestrator::validateDAG(
    const orchestrator::IntentDAG& dag,
    const orchestrator::AdmissionContext& admission,
    const CallContext& call) const {
    const auto reply = rpc_->call(
        "orchestrator.validate",
        {{"dag", wire::encode(dag)},
         {"admission", wire::encode(admission)}},
        call);
    if (!reply.status.ok) {
        return {false, reply.status.error.code, {}};
    }
    try {
        return wire::decodeValidationResult(reply.value);
    } catch (...) {
        return {false, "RPC_VALUE_DECODE_FAILED", {}};
    }
}

orchestrator::PlanCommitResult
RemoteOrchestrator::submit(
    const orchestrator::OrchestratorSubmitRequest& request,
    const CallContext& call) {
    const auto reply = rpc_->call(
        "orchestrator.submit", wire::encode(request), call);
    if (!reply.status.ok) {
        orchestrator::PlanCommitResult result;
        result.reject_code = reply.status.error.code;
        return result;
    }
    try {
        return wire::decodePlanCommit(reply.value);
    } catch (...) {
        orchestrator::PlanCommitResult result;
        result.reject_code = "RPC_VALUE_DECODE_FAILED";
        return result;
    }
}

Result<orchestrator::TaskPlanSnapshot>
RemoteOrchestrator::getPlan(
    const std::string& plan_id,
    const CallContext& call) const {
    return decodeReply<orchestrator::TaskPlanSnapshot>(
        rpc_->call("orchestrator.get_plan",
                   {{"plan_id", plan_id}}, call),
        wire::decodePlanSnapshot);
}

bool RemoteOrchestrator::pumpOne() {
    const auto reply = rpc_->call(
        "orchestrator.pump", nlohmann::json::object(),
        driverCall(CallerModuleId::AgentService));
    return reply.status.ok && reply.value.get<bool>();
}

Status RemoteOrchestrator::runUntilPlanTerminal(
    const std::string& plan_id,
    std::size_t max_steps) {
    return statusReply(rpc_->call(
        "orchestrator.run",
        {{"plan_id", plan_id}, {"max_steps", max_steps}},
        driverCall(CallerModuleId::AgentService)));
}

std::vector<orchestrator::TaskEvent>
RemoteOrchestrator::events() const {
    return {};
}

RemoteDataLogService::RemoteDataLogService(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

Result<data_log::LogAppendResult>
RemoteDataLogService::appendEvents(
    const data_log::LogEventBatch& batch,
    const CallContext& call) {
    return decodeReply<data_log::LogAppendResult>(
        rpc_->call("observability.append_events",
                   wire::encode(batch), call),
        wire::decodeLogAppendResult);
}

Result<data_log::AuditAppendResult>
RemoteDataLogService::appendAudit(
    const data_log::AuditBatch& batch,
    const CallContext& call) {
    return decodeReply<data_log::AuditAppendResult>(
        rpc_->call("observability.append_audit",
                   wire::encode(batch), call),
        wire::decodeAuditAppendResult);
}

Result<data_log::TracePage>
RemoteDataLogService::queryTrace(
    const data_log::TraceQuery& query,
    const CallContext& call) const {
    return decodeReply<data_log::TracePage>(
        rpc_->call("observability.query_trace",
                   wire::encode(query), call),
        wire::decodeTracePage);
}

Status RemoteDataLogService::flush(
    const CallContext& call) {
    return statusReply(rpc_->call(
        "observability.flush", nlohmann::json::object(), call));
}

data_log::LogHealth RemoteDataLogService::getHealth(
    const CallContext& call) const {
    const auto reply = rpc_->call(
        "observability.log_health", nlohmann::json::object(),
        call);
    if (!reply.status.ok) return {};
    try {
        return wire::decodeLogHealth(reply.value);
    } catch (...) {
        return {};
    }
}

RemoteExceptionManager::RemoteExceptionManager(
    std::shared_ptr<RpcClient> rpc)
    : rpc_(std::move(rpc)) {}

Result<exception::ExceptionReportResult>
RemoteExceptionManager::report(
    const exception::ExceptionReportRequest& request,
    const CallContext& call) {
    return decodeReply<exception::ExceptionReportResult>(
        rpc_->call("observability.report_exception",
                   wire::encode(request), call),
        wire::decodeExceptionReportResult);
}

Result<exception::ExceptionGroup>
RemoteExceptionManager::getException(
    const std::string& exception_id,
    const CallContext& call) const {
    return decodeReply<exception::ExceptionGroup>(
        rpc_->call("observability.get_exception",
                   {{"exception_id", exception_id}}, call),
        wire::decodeExceptionGroup);
}

Result<exception::ExceptionMutationResult>
RemoteExceptionManager::mutate(
    const std::string& operation,
    const exception::ExceptionMutationRequest& request,
    const CallContext& call) {
    return decodeReply<exception::ExceptionMutationResult>(
        rpc_->call(operation, wire::encode(request), call),
        wire::decodeExceptionMutationResult);
}

Result<exception::ExceptionMutationResult>
RemoteExceptionManager::acknowledge(
    const exception::ExceptionMutationRequest& request,
    const CallContext& call) {
    return mutate("observability.acknowledge_exception",
                  request, call);
}

Result<exception::ExceptionMutationResult>
RemoteExceptionManager::markMitigating(
    const exception::ExceptionMutationRequest& request,
    const CallContext& call) {
    return mutate("observability.mitigate_exception",
                  request, call);
}

Result<exception::ExceptionMutationResult>
RemoteExceptionManager::resolve(
    const exception::ExceptionMutationRequest& request,
    const CallContext& call) {
    return mutate("observability.resolve_exception",
                  request, call);
}

}  // namespace master_agent::ipc
