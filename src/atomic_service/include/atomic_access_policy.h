#pragma once

/**
 * @file atomic_access_policy.h
 * @brief Private caller policy, request identity, and completion-contract helpers.
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

bool canList(CallerModuleId caller) {
    return caller == CallerModuleId::IntentRecognitionEngine ||
           caller == CallerModuleId::SkillEngine ||
           caller == CallerModuleId::PromptEngine ||
           caller == CallerModuleId::TaskOrchestrationEngine;
}

bool canGetSnapshot(CallerModuleId caller) {
    return caller == CallerModuleId::TaskOrchestrationEngine ||
           caller == CallerModuleId::AgentService;
}

bool canQuery(CallerModuleId caller) {
    return caller == CallerModuleId::TaskOrchestrationEngine ||
           caller == CallerModuleId::AgentService ||
           caller == CallerModuleId::AgentDispatch;
}

bool canControl(CallerModuleId caller) {
    return caller == CallerModuleId::TaskOrchestrationEngine ||
           caller == CallerModuleId::AgentDispatch;
}

std::string definitionDigest(const McpToolDefinition& definition) {
    nlohmann::json serialized{
        {"name", definition.name},
        {"title", definition.title},
        {"description", definition.description},
        {"inputSchema", definition.input_schema},
        {"outputSchema", definition.output_schema},
        {"annotations",
         {{"title", definition.annotations.title},
          {"readOnlyHint", definition.annotations.read_only_hint},
          {"destructiveHint", definition.annotations.destructive_hint},
          {"idempotentHint", definition.annotations.idempotent_hint},
          {"openWorldHint", definition.annotations.open_world_hint}}}};
    return secureDigest(serialized.dump());
}

std::string completionPolicyName(CompletionPolicy policy) {
    switch (policy) {
        case CompletionPolicy::ReturnConfirmed:
            return "RETURN_CONFIRMED";
        case CompletionPolicy::ProviderAccepted:
            return "PROVIDER_ACCEPTED";
        case CompletionPolicy::EventConfirmed:
            return "EVENT_CONFIRMED";
        case CompletionPolicy::StateVerified:
            return "STATE_VERIFIED";
        case CompletionPolicy::TransactionReceipt:
            return "TRANSACTION_RECEIPT";
    }
    return "INVALID";
}

// Completion evidence is deliberately not ordered.  A durable Provider ACK
// cannot stand in for a state read-back, and a terminal event cannot stand in
// for an immutable transaction receipt.
bool satisfiesCompletionPolicy(CompletionPolicy policy,
                               CompletionEvidence evidence) {
    switch (policy) {
        case CompletionPolicy::ReturnConfirmed:
            return evidence == CompletionEvidence::ReturnConfirmed;
        case CompletionPolicy::ProviderAccepted:
            return evidence == CompletionEvidence::ProviderAccepted;
        case CompletionPolicy::EventConfirmed:
            return evidence == CompletionEvidence::EventConfirmed;
        case CompletionPolicy::StateVerified:
            return evidence == CompletionEvidence::StateVerified;
        case CompletionPolicy::TransactionReceipt:
            return evidence == CompletionEvidence::TransactionReceipt;
    }
    return false;
}

std::string policyDigest(const AtomicToolRuntimePolicy& policy) {
    nlohmann::json serialized{
        {"tool_name", policy.tool_name},
        {"tool_contract_version", policy.tool_contract_version},
        {"required_permissions", policy.required_permissions},
        {"resource_argument_fields", policy.resource_argument_fields},
        {"idempotency_policy", policy.idempotency_policy},
        {"retryable_errors", policy.retryable_errors},
        {"completion_policy",
         completionPolicyName(policy.completion_policy)},
        {"cancel_model", policy.cancel_model},
        {"supports_preemption", policy.supports_preemption},
        {"supports_reconcile", policy.supports_reconcile},
        {"max_concurrency", policy.max_concurrency}};
    return secureDigest(serialized.dump());
}

std::string callDigest(const AtomicMcpCallEnvelope& envelope) {
    nlohmann::json encoded{
        {"jsonrpc", envelope.mcp_request.jsonrpc},
        {"mcp_request_id", envelope.mcp_request.id},
        {"method", envelope.mcp_request.method},
        {"name", envelope.mcp_request.name},
        {"arguments", envelope.mcp_request.arguments},
        {"caller_module_id",
         toString(envelope.runtime.caller_module_id)},
        {"request_id", envelope.runtime.request_id},
        {"trace_id", envelope.runtime.trace_id},
        {"plan_id", envelope.runtime.plan_id},
        {"pid", envelope.runtime.pid},
        {"activation_id", envelope.runtime.activation_id},
        {"execution_id", envelope.runtime.execution_id},
        {"attempt_no", envelope.runtime.attempt_no},
        {"operation_id", envelope.runtime.operation_id},
        {"priority", toString(envelope.runtime.priority)},
        {"deadline_mono_ns", envelope.runtime.deadline_mono_ns},
        {"fencing_token", envelope.runtime.fencing_token},
        {"tool_catalog_snapshot_id",
         envelope.runtime.tool_catalog_snapshot_id},
        {"tool_digest", envelope.runtime.tool_digest},
        {"policy_digest", envelope.runtime.policy_digest},
        {"granted_permissions",
         envelope.runtime.granted_permissions},
        {"resource_lease_refs",
         envelope.runtime.resource_lease_refs},
        {"principal_id_hash",
         envelope.runtime.principal_id_hash},
        {"authorization_ref",
         envelope.runtime.authorization_ref},
        {"parent_operation_id",
         envelope.runtime.parent_operation_id.value_or("")},
        {"parent_dispatch_id",
         envelope.runtime.parent_dispatch_id},
        {"parent_agent_id",
         envelope.runtime.parent_agent_id},
        {"parent_agent_epoch",
         envelope.runtime.parent_agent_epoch},
        {"parent_lease_id",
         envelope.runtime.parent_lease_id},
        {"parent_fencing_token",
         envelope.runtime.parent_fencing_token}};
    return secureDigest(encoded.dump());
}

// Provider callbacks are accepted only when every field of the immutable
// invocation identity is echoed exactly.
bool invocationSealMatches(
    const AtomicProviderInvocationSeal& actual,
    const AtomicProviderInvocationSeal& expected) {
    return actual.invocation_id == expected.invocation_id &&
           actual.provider_id == expected.provider_id &&
           actual.provider_epoch == expected.provider_epoch &&
           actual.operation_id == expected.operation_id &&
           actual.execution_id == expected.execution_id &&
           actual.attempt_no == expected.attempt_no &&
           actual.tool_name == expected.tool_name &&
           actual.tool_catalog_snapshot_id ==
               expected.tool_catalog_snapshot_id &&
           actual.tool_digest == expected.tool_digest &&
           actual.policy_digest == expected.policy_digest &&
           actual.fencing_token == expected.fencing_token &&
           actual.request_digest == expected.request_digest;
}

CallToolResult errorResult(const std::string& code,
                           const std::string& message) {
    CallToolResult result;
    result.is_error = true;
    result.structured_content =
        nlohmann::json{{"success", false},
                       {"errorCode", code},
                       {"message", message}};
    result.text_content.push_back(result.structured_content.dump());
    return result;
}

}  // namespace
}  // namespace master_agent::atomic_service

