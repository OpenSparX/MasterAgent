#pragma once

/**
 * @file atomic_wal_codec.h
 * @brief Private atomic WAL constants and JSON serialization helpers.
 *
 * This header is private to Atomic Service and is not part of the installed API.
 */

#include "master_agent/atomic_service/atomic_service.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace master_agent::atomic_service {
namespace {

constexpr std::uint32_t kAtomicWalSchemaVersion = 1;
constexpr const char* kAtomicWalGenesis = "GENESIS";

nlohmann::json callToolResultJson(const CallToolResult& result) {
    return nlohmann::json{
        {"text_content", result.text_content},
        {"structured_content", result.structured_content},
        {"is_error", result.is_error}};
}

CallToolResult callToolResultFromJson(const nlohmann::json& encoded) {
    CallToolResult result;
    result.text_content =
        encoded.at("text_content").get<std::vector<std::string>>();
    result.structured_content = encoded.at("structured_content");
    result.is_error = encoded.at("is_error").get<bool>();
    return result;
}

nlohmann::json invocationSealJson(
    const AtomicProviderInvocationSeal& seal) {
    return nlohmann::json{
        {"invocation_id", seal.invocation_id},
        {"provider_id", seal.provider_id},
        {"provider_epoch", seal.provider_epoch},
        {"operation_id", seal.operation_id},
        {"execution_id", seal.execution_id},
        {"attempt_no", seal.attempt_no},
        {"tool_name", seal.tool_name},
        {"tool_catalog_snapshot_id",
         seal.tool_catalog_snapshot_id},
        {"tool_digest", seal.tool_digest},
        {"policy_digest", seal.policy_digest},
        {"fencing_token", seal.fencing_token},
        {"request_digest", seal.request_digest}};
}

AtomicProviderInvocationSeal invocationSealFromJson(
    const nlohmann::json& encoded) {
    AtomicProviderInvocationSeal seal;
    seal.invocation_id =
        encoded.at("invocation_id").get<std::string>();
    seal.provider_id =
        encoded.at("provider_id").get<std::string>();
    seal.provider_epoch =
        encoded.at("provider_epoch").get<std::uint64_t>();
    seal.operation_id =
        encoded.at("operation_id").get<std::string>();
    seal.execution_id =
        encoded.at("execution_id").get<std::string>();
    seal.attempt_no =
        encoded.at("attempt_no").get<std::uint32_t>();
    seal.tool_name =
        encoded.at("tool_name").get<std::string>();
    seal.tool_catalog_snapshot_id =
        encoded.at("tool_catalog_snapshot_id").get<std::string>();
    seal.tool_digest =
        encoded.at("tool_digest").get<std::string>();
    seal.policy_digest =
        encoded.at("policy_digest").get<std::string>();
    seal.fencing_token =
        encoded.at("fencing_token").get<std::uint64_t>();
    seal.request_digest =
        encoded.at("request_digest").get<std::string>();
    if (seal.invocation_id.empty() || seal.provider_id.empty() ||
        seal.provider_epoch == 0 || seal.operation_id.empty() ||
        seal.execution_id.empty() || seal.attempt_no == 0 ||
        seal.tool_name.empty() ||
        seal.tool_catalog_snapshot_id.empty() ||
        seal.tool_digest.empty() || seal.policy_digest.empty() ||
        seal.fencing_token == 0 || seal.request_digest.empty()) {
        throw std::runtime_error("invalid atomic Provider seal");
    }
    return seal;
}

nlohmann::json atomicEnvelopeJson(
    const AtomicMcpCallEnvelope& envelope) {
    const auto& request = envelope.mcp_request;
    const auto& runtime = envelope.runtime;
    return nlohmann::json{
        {"mcp_request",
         {{"jsonrpc", request.jsonrpc},
          {"id", request.id},
          {"method", request.method},
          {"name", request.name},
          {"arguments", request.arguments}}},
        {"runtime",
         {{"caller_module_id",
           static_cast<std::uint8_t>(runtime.caller_module_id)},
          {"request_id", runtime.request_id},
          {"trace_id", runtime.trace_id},
          {"plan_id", runtime.plan_id},
          {"pid", runtime.pid},
          {"activation_id", runtime.activation_id},
          {"execution_id", runtime.execution_id},
          {"attempt_no", runtime.attempt_no},
          {"operation_id", runtime.operation_id},
          {"priority", static_cast<std::uint8_t>(runtime.priority)},
          {"deadline_mono_ns", runtime.deadline_mono_ns},
          {"idempotency_key", runtime.idempotency_key},
          {"fencing_token", runtime.fencing_token},
          {"tool_catalog_snapshot_id",
           runtime.tool_catalog_snapshot_id},
          {"tool_digest", runtime.tool_digest},
          {"policy_digest", runtime.policy_digest},
          {"granted_permissions", runtime.granted_permissions},
          {"resource_lease_refs", runtime.resource_lease_refs},
          {"principal_id_hash", runtime.principal_id_hash},
          {"authorization_ref", runtime.authorization_ref},
          {"parent_operation_id",
           runtime.parent_operation_id
               ? nlohmann::json(*runtime.parent_operation_id)
               : nlohmann::json(nullptr)},
          {"parent_dispatch_id", runtime.parent_dispatch_id},
          {"parent_agent_id", runtime.parent_agent_id},
          {"parent_agent_epoch", runtime.parent_agent_epoch},
          {"parent_lease_id", runtime.parent_lease_id},
          {"parent_fencing_token",
           runtime.parent_fencing_token}}}};
}

AtomicMcpCallEnvelope atomicEnvelopeFromJson(
    const nlohmann::json& encoded) {
    AtomicMcpCallEnvelope envelope;
    const auto& request = encoded.at("mcp_request");
    envelope.mcp_request.jsonrpc =
        request.at("jsonrpc").get<std::string>();
    envelope.mcp_request.id =
        request.at("id").get<std::string>();
    envelope.mcp_request.method =
        request.at("method").get<std::string>();
    envelope.mcp_request.name =
        request.at("name").get<std::string>();
    envelope.mcp_request.arguments = request.at("arguments");

    const auto& runtime = encoded.at("runtime");
    const auto caller =
        runtime.at("caller_module_id").get<std::uint8_t>();
    if (caller == 0 ||
        caller > static_cast<std::uint8_t>(
                     CallerModuleId::ExceptionManager)) {
        throw std::runtime_error("invalid atomic caller");
    }
    envelope.runtime.caller_module_id =
        static_cast<CallerModuleId>(caller);
    envelope.runtime.request_id =
        runtime.at("request_id").get<std::string>();
    envelope.runtime.trace_id =
        runtime.at("trace_id").get<std::string>();
    envelope.runtime.plan_id =
        runtime.at("plan_id").get<std::string>();
    envelope.runtime.pid =
        runtime.at("pid").get<std::string>();
    envelope.runtime.activation_id =
        runtime.at("activation_id").get<std::string>();
    envelope.runtime.execution_id =
        runtime.at("execution_id").get<std::string>();
    envelope.runtime.attempt_no =
        runtime.at("attempt_no").get<std::uint32_t>();
    envelope.runtime.operation_id =
        runtime.at("operation_id").get<std::string>();
    envelope.runtime.priority = static_cast<TaskPriority>(
        runtime.at("priority").get<std::uint8_t>());
    if (!isValidTaskPriority(envelope.runtime.priority)) {
        throw std::runtime_error("invalid atomic priority");
    }
    envelope.runtime.deadline_mono_ns =
        runtime.at("deadline_mono_ns").get<std::int64_t>();
    envelope.runtime.idempotency_key =
        runtime.at("idempotency_key").get<std::string>();
    envelope.runtime.fencing_token =
        runtime.at("fencing_token").get<std::uint64_t>();
    envelope.runtime.tool_catalog_snapshot_id =
        runtime.at("tool_catalog_snapshot_id").get<std::string>();
    envelope.runtime.tool_digest =
        runtime.at("tool_digest").get<std::string>();
    envelope.runtime.policy_digest =
        runtime.at("policy_digest").get<std::string>();
    envelope.runtime.granted_permissions =
        runtime.at("granted_permissions")
            .get<std::vector<std::string>>();
    envelope.runtime.resource_lease_refs =
        runtime.at("resource_lease_refs")
            .get<std::vector<std::string>>();
    envelope.runtime.principal_id_hash =
        runtime.at("principal_id_hash").get<std::string>();
    envelope.runtime.authorization_ref =
        runtime.at("authorization_ref").get<std::string>();
    if (!runtime.at("parent_operation_id").is_null()) {
        envelope.runtime.parent_operation_id =
            runtime.at("parent_operation_id").get<std::string>();
    }
    envelope.runtime.parent_dispatch_id =
        runtime.at("parent_dispatch_id").get<std::string>();
    envelope.runtime.parent_agent_id =
        runtime.at("parent_agent_id").get<std::string>();
    envelope.runtime.parent_agent_epoch =
        runtime.at("parent_agent_epoch").get<std::uint64_t>();
    envelope.runtime.parent_lease_id =
        runtime.at("parent_lease_id").get<std::string>();
    envelope.runtime.parent_fencing_token =
        runtime.at("parent_fencing_token").get<std::uint64_t>();
    if (envelope.mcp_request.jsonrpc != "2.0" ||
        envelope.mcp_request.method != "tools/call" ||
        envelope.mcp_request.id.empty() ||
        envelope.mcp_request.name.empty() ||
        !envelope.mcp_request.arguments.is_object() ||
        envelope.runtime.request_id.empty() ||
        envelope.runtime.trace_id.empty() ||
        envelope.runtime.plan_id.empty() ||
        envelope.runtime.pid.empty() ||
        envelope.runtime.activation_id.empty() ||
        envelope.runtime.execution_id.empty() ||
        envelope.runtime.attempt_no == 0 ||
        envelope.runtime.operation_id.empty() ||
        envelope.runtime.deadline_mono_ns <= 0 ||
        envelope.runtime.idempotency_key.empty() ||
        envelope.runtime.fencing_token == 0 ||
        envelope.runtime.tool_catalog_snapshot_id.empty() ||
        envelope.runtime.tool_digest.empty() ||
        envelope.runtime.policy_digest.empty() ||
        envelope.runtime.principal_id_hash.empty() ||
        envelope.runtime.authorization_ref.empty()) {
        throw std::runtime_error("invalid recovered atomic envelope");
    }
    return envelope;
}

nlohmann::json atomicSnapshotJson(
    const AtomicExecutionSnapshot& snapshot) {
    return nlohmann::json{
        {"envelope", atomicEnvelopeJson(snapshot.envelope)},
        {"state", static_cast<std::uint8_t>(snapshot.state)},
        {"result",
         snapshot.result
             ? callToolResultJson(*snapshot.result)
             : nlohmann::json(nullptr)},
        {"side_effect_state",
         static_cast<std::uint8_t>(snapshot.side_effect_state)},
        {"completion_evidence",
         static_cast<std::uint8_t>(snapshot.completion_evidence)},
        {"error_code", snapshot.error_code},
        {"resource_key", snapshot.resource_key},
        {"remaining_work_units", snapshot.remaining_work_units},
        {"control_epoch", snapshot.control_epoch},
        {"provider_invocation",
         snapshot.provider_invocation
             ? invocationSealJson(*snapshot.provider_invocation)
             : nlohmann::json(nullptr)},
        {"retryable_hint", snapshot.retryable_hint}};
}

AtomicExecutionSnapshot atomicSnapshotFromJson(
    const nlohmann::json& encoded) {
    AtomicExecutionSnapshot snapshot;
    snapshot.envelope =
        atomicEnvelopeFromJson(encoded.at("envelope"));
    const auto state =
        encoded.at("state").get<std::uint8_t>();
    if (state >
        static_cast<std::uint8_t>(AtomicExecutionState::Unknown)) {
        throw std::runtime_error("invalid atomic state");
    }
    snapshot.state = static_cast<AtomicExecutionState>(state);
    if (!encoded.at("result").is_null()) {
        snapshot.result =
            callToolResultFromJson(encoded.at("result"));
    }
    snapshot.side_effect_state = static_cast<SideEffectState>(
        encoded.at("side_effect_state").get<std::uint8_t>());
    if (!isValidSideEffectState(snapshot.side_effect_state)) {
        throw std::runtime_error("invalid atomic side effect");
    }
    const auto evidence =
        encoded.at("completion_evidence").get<std::uint8_t>();
    if (evidence >
        static_cast<std::uint8_t>(
            CompletionEvidence::TransactionReceipt)) {
        throw std::runtime_error("invalid atomic completion evidence");
    }
    snapshot.completion_evidence =
        static_cast<CompletionEvidence>(evidence);
    snapshot.error_code =
        encoded.at("error_code").get<std::string>();
    snapshot.resource_key =
        encoded.at("resource_key").get<std::string>();
    snapshot.remaining_work_units =
        encoded.at("remaining_work_units").get<std::uint32_t>();
    snapshot.control_epoch =
        encoded.at("control_epoch").get<std::uint64_t>();
    if (!encoded.at("provider_invocation").is_null()) {
        snapshot.provider_invocation =
            invocationSealFromJson(
                encoded.at("provider_invocation"));
    }
    snapshot.retryable_hint =
        encoded.at("retryable_hint").get<bool>();
    if (snapshot.resource_key.empty()) {
        throw std::runtime_error("invalid atomic resource key");
    }
    return snapshot;
}

}  // namespace
}  // namespace master_agent::atomic_service

