#pragma once

/**
 * @file wire_codec.h
 * @brief Defines strict JSON codecs for cross-process contracts.
 */

#include <nlohmann/json.hpp>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/agent_service/agent_service.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/orchestrator/orchestrator.h"

namespace master_agent::ipc::wire {

/// The functions in this file are the only supported IPC representation of
/// module-owned C++ types. Decoders use required-field access and closed enum
/// checks; malformed payloads throw and are converted to a bounded IPC error
/// by the service host.

nlohmann::json encodeStatus(const Status& value);
Status decodeStatus(const nlohmann::json& value);
nlohmann::json encodeCallContext(const CallContext& value);
CallContext decodeCallContext(const nlohmann::json& value);

nlohmann::json encode(
    const interaction::TextInput& value);
interaction::TextInput decodeTextInput(
    const nlohmann::json& value);
nlohmann::json encode(
    const interaction::StandardRequest& value);
interaction::StandardRequest decodeStandardRequest(
    const nlohmann::json& value);
nlohmann::json encode(
    const agent_service::TurnResult& value);
agent_service::TurnResult decodeTurnResult(
    const nlohmann::json& value);

nlohmann::json encode(
    const memory::MemoryContext& value);
memory::MemoryContext decodeMemoryContext(
    const nlohmann::json& value);
nlohmann::json encode(
    const memory::CompletedTurn& value);
memory::CompletedTurn decodeCompletedTurn(
    const nlohmann::json& value);

nlohmann::json encode(
    const atomic_service::McpToolDefinition& value);
atomic_service::McpToolDefinition decodeMcpToolDefinition(
    const nlohmann::json& value);
nlohmann::json encode(
    const atomic_service::McpToolCatalogSnapshot& value);
atomic_service::McpToolCatalogSnapshot decodeToolCatalog(
    const nlohmann::json& value);
nlohmann::json encode(
    const atomic_service::AtomicMcpCallEnvelope& value);
atomic_service::AtomicMcpCallEnvelope decodeAtomicCall(
    const nlohmann::json& value);
nlohmann::json encode(
    const atomic_service::DispatchAcceptance& value);
atomic_service::DispatchAcceptance decodeAtomicAcceptance(
    const nlohmann::json& value);
nlohmann::json encode(
    const atomic_service::AtomicExecutionSnapshot& value);
atomic_service::AtomicExecutionSnapshot decodeAtomicSnapshot(
    const nlohmann::json& value);
nlohmann::json encode(
    const atomic_service::AtomicReconcileResult& value);
atomic_service::AtomicReconcileResult decodeAtomicReconcile(
    const nlohmann::json& value);

nlohmann::json encode(
    const agent_dispatch::DispatchTask& value);
agent_dispatch::DispatchTask decodeDispatchTask(
    const nlohmann::json& value);
nlohmann::json encode(
    const agent_dispatch::DispatchAcceptance& value);
agent_dispatch::DispatchAcceptance decodeDispatchAcceptance(
    const nlohmann::json& value);
nlohmann::json encode(
    const agent_dispatch::DispatchSnapshot& value);
agent_dispatch::DispatchSnapshot decodeDispatchSnapshot(
    const nlohmann::json& value);
nlohmann::json encode(
    const agent_dispatch::AgentDispatchCapacity& value);
agent_dispatch::AgentDispatchCapacity decodeDispatchCapacity(
    const nlohmann::json& value);

nlohmann::json encode(
    const inference::InferenceRequest& value);
inference::InferenceRequest decodeInferenceRequest(
    const nlohmann::json& value);
nlohmann::json encode(
    const inference::InferenceAcceptance& value);
inference::InferenceAcceptance decodeInferenceAcceptance(
    const nlohmann::json& value);
nlohmann::json encode(
    const inference::InferenceJobSnapshot& value);
inference::InferenceJobSnapshot decodeInferenceSnapshot(
    const nlohmann::json& value);

nlohmann::json encode(
    const orchestrator::IntentDAG& value);
orchestrator::IntentDAG decodeIntentDAG(
    const nlohmann::json& value);
nlohmann::json encode(
    const orchestrator::AdmissionContext& value);
orchestrator::AdmissionContext decodeAdmission(
    const nlohmann::json& value);
nlohmann::json encode(
    const orchestrator::OrchestratorSubmitRequest& value);
orchestrator::OrchestratorSubmitRequest decodeOrchestratorSubmit(
    const nlohmann::json& value);
nlohmann::json encode(
    const orchestrator::ValidationResult& value);
orchestrator::ValidationResult decodeValidationResult(
    const nlohmann::json& value);
nlohmann::json encode(
    const orchestrator::PlanCommitResult& value);
orchestrator::PlanCommitResult decodePlanCommit(
    const nlohmann::json& value);
nlohmann::json encode(
    const orchestrator::TaskPlanSnapshot& value);
orchestrator::TaskPlanSnapshot decodePlanSnapshot(
    const nlohmann::json& value);

nlohmann::json encode(
    const data_log::LogEventBatch& value);
data_log::LogEventBatch decodeLogEventBatch(
    const nlohmann::json& value);
nlohmann::json encode(
    const data_log::LogAppendResult& value);
data_log::LogAppendResult decodeLogAppendResult(
    const nlohmann::json& value);
nlohmann::json encode(
    const data_log::AuditBatch& value);
data_log::AuditBatch decodeAuditBatch(
    const nlohmann::json& value);
nlohmann::json encode(
    const data_log::AuditAppendResult& value);
data_log::AuditAppendResult decodeAuditAppendResult(
    const nlohmann::json& value);
nlohmann::json encode(
    const data_log::TraceQuery& value);
data_log::TraceQuery decodeTraceQuery(
    const nlohmann::json& value);
nlohmann::json encode(
    const data_log::TracePage& value);
data_log::TracePage decodeTracePage(
    const nlohmann::json& value);
nlohmann::json encode(
    const data_log::LogHealth& value);
data_log::LogHealth decodeLogHealth(
    const nlohmann::json& value);

nlohmann::json encode(
    const exception::ExceptionReportRequest& value);
exception::ExceptionReportRequest decodeExceptionReport(
    const nlohmann::json& value);
nlohmann::json encode(
    const exception::ExceptionReportResult& value);
exception::ExceptionReportResult decodeExceptionReportResult(
    const nlohmann::json& value);
nlohmann::json encode(
    const exception::ExceptionGroup& value);
exception::ExceptionGroup decodeExceptionGroup(
    const nlohmann::json& value);
nlohmann::json encode(
    const exception::ExceptionMutationRequest& value);
exception::ExceptionMutationRequest decodeExceptionMutation(
    const nlohmann::json& value);
nlohmann::json encode(
    const exception::ExceptionMutationResult& value);
exception::ExceptionMutationResult decodeExceptionMutationResult(
    const nlohmann::json& value);

}  // namespace master_agent::ipc::wire
