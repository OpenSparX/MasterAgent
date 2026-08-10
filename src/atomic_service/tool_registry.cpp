/**
 * @file tool_registry.cpp
 * @brief Registers MCP tool definitions and manages registry snapshots.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

Status AtomicServiceManager::registerTools(
    const std::vector<McpToolDefinition>& tools,
    const std::vector<AtomicToolRuntimePolicy>& policies,
    std::shared_ptr<IAtomicProvider> provider,
    const CallContext& call) {

    std::lock_guard<std::mutex> lock(mutex_);
    if (!durability_status_.ok) {
        return durability_status_;
    }
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        return Status::Error("atomic_service",
                             "ATOMIC_REGISTER_CALLER_NOT_ALLOWED",
                             "only AgentService may register tools");
    }
    if (!clock_ || !ids_ || call.request_id.empty() ||
        call.trace_id.empty() || call.principal_id_hash.empty() ||
        !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        !provider || tools.empty() ||
        tools.size() > 512 ||
        tools.size() != policies.size()) {
        return Status::Error("atomic_service", "ATOMIC_REGISTRATION_INVALID",
                             "tool registration is invalid");
    }
    for (const auto& execution : executions_) {
        const auto state = execution.second.state;
        if (state != AtomicExecutionState::Succeeded &&
            state != AtomicExecutionState::Failed &&
            state != AtomicExecutionState::Cancelled) {
            return Status::Error(
                "atomic_service", "ATOMIC_CATALOG_UPDATE_BUSY",
                "catalog cannot change while execution or reconciliation is active",
                true);
        }
    }
    std::map<std::string, ToolRecord> next;
    const auto next_catalog_generation =
        catalog_.catalog_generation + 1;
    const auto provider_id = ids_->next("atomic-provider");
    for (std::size_t i = 0; i < tools.size(); ++i) {
        auto definition = tools[i];
        auto policy = policies[i];
        const auto input_schema_status =
            validateObjectSchemaSubset(
                definition.input_schema);
        const auto output_schema_status =
            definition.output_schema.empty()
                ? Status::Ok()
                : validateObjectSchemaSubset(
                      definition.output_schema);
        if (!safeSchemaIdentifier(definition.name, 128) ||
            !safeSchemaText(definition.title, 512) ||
            !safeSchemaText(
                definition.description, 8192, false) ||
            !safeSchemaText(
                definition.annotations.title, 512) ||
            (definition.annotations.read_only_hint &&
             definition.annotations.destructive_hint) ||
            (definition.annotations.read_only_hint &&
             policy.idempotency_policy != "READ_ONLY") ||
            (!definition.annotations.read_only_hint &&
             policy.idempotency_policy == "READ_ONLY") ||
            policy.tool_name != definition.name ||
            next.count(definition.name) != 0 ||
            !input_schema_status.ok ||
            !output_schema_status.ok ||
            !safeSchemaIdentifier(
                policy.tool_contract_version, 64) ||
            !safeSchemaIdentifier(
                policy.idempotency_policy, 64) ||
            !safeSchemaIdentifier(policy.cancel_model, 64) ||
            completionPolicyName(
                policy.completion_policy) == "INVALID" ||
            !validBoundedUniquePolicyValues(
                policy.required_permissions) ||
            !validBoundedUniquePolicyValues(
                policy.resource_argument_fields) ||
            !validBoundedUniquePolicyValues(
                policy.retryable_errors) ||
            policy.max_concurrency == 0 ||
            policy.max_concurrency > 1024 ||
            policy.simulated_work_units == 0 ||
            policy.simulated_work_units > 1'000'000) {
            return Status::Error("atomic_service",
                                 "ATOMIC_REGISTRATION_INVALID",
                                 "tool name and policy binding are invalid");
        }
        const auto& input_properties =
            definition.input_schema.contains("properties")
                ? definition.input_schema.at("properties")
                : nlohmann::json::object();
        if (std::any_of(
                policy.resource_argument_fields.begin(),
                policy.resource_argument_fields.end(),
                [&input_properties](
                    const std::string& field) {
                    return !input_properties.contains(field);
                })) {
            return Status::Error(
                "atomic_service",
                "ATOMIC_REGISTRATION_INVALID",
                "resource key fields must exist in inputSchema");
        }
        try {
            policy.tool_digest =
                definitionDigest(definition);
            policy.policy_digest = policyDigest(policy);
        } catch (...) {
            return Status::Error(
                "atomic_service",
                "ATOMIC_REGISTRATION_INVALID",
                "tool definition cannot be canonically encoded");
        }
        // Keep the key before moving definition.  C++17 does not guarantee
        // evaluation order between emplace arguments; reading definition.name
        // after the move can otherwise collapse all entries onto an empty key.
        const auto tool_name = definition.name;
        next.emplace(tool_name,
                     ToolRecord{std::move(definition), std::move(policy),
                                provider, provider_id,
                                next_catalog_generation});
    }
    tools_ = std::move(next);
    ++catalog_.catalog_generation;
    catalog_.snapshot_id = ids_->next("tool-catalog");
    catalog_.created_at_utc_ms = clock_->utcNowMs();
    catalog_.tools.clear();
    catalog_.tool_digests.clear();
    catalog_.policy_digests.clear();
    catalog_.idempotency_policies.clear();
    catalog_.retryable_errors.clear();
    catalog_.provider_ids.clear();
    catalog_.provider_epochs.clear();
    std::string aggregate;
    for (const auto& pair : tools_) {
        catalog_.tools.push_back(pair.second.definition);
        catalog_.tool_digests[pair.first] =
            pair.second.policy.tool_digest;
        catalog_.policy_digests[pair.first] =
            pair.second.policy.policy_digest;
        catalog_.idempotency_policies[pair.first] =
            pair.second.policy.idempotency_policy;
        catalog_.retryable_errors[pair.first] =
            pair.second.policy.retryable_errors;
        catalog_.provider_ids[pair.first] =
            pair.second.provider_id;
        catalog_.provider_epochs[pair.first] =
            pair.second.provider_epoch;
        aggregate += pair.first + "|" + pair.second.policy.tool_digest +
                     "|" + pair.second.policy.policy_digest + "|" +
                     pair.second.provider_id + "|" +
                     std::to_string(pair.second.provider_epoch) + ";";
    }
    catalog_.catalog_digest = secureDigest(aggregate);
    const auto recovered =
        activateRecoveredExecutionsUnlocked(tools_);
    if (!recovered.ok) {
        durability_status_ = recovered;
        return recovered;
    }
    return Status::Ok();
}

Result<std::vector<McpToolDefinition>> AtomicServiceManager::listTools(
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!canList(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) ||
        !clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<std::vector<McpToolDefinition>>::Failure(Status::Error(
            "atomic_service", "ATOMIC_LIST_CALLER_MODULE_NOT_ALLOWED",
            "caller may not use MCP tools/list"));
    }
    return Result<std::vector<McpToolDefinition>>::Success(catalog_.tools);
}

Result<McpToolCatalogSnapshot>
AtomicServiceManager::getToolCatalogSnapshot(
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!canGetSnapshot(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) ||
        !clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<McpToolCatalogSnapshot>::Failure(Status::Error(
            "atomic_service", "ATOMIC_SNAPSHOT_CALLER_NOT_ALLOWED",
            "caller may not get trusted catalog snapshot"));
    }
    return Result<McpToolCatalogSnapshot>::Success(catalog_);
}

// Admission validates the MCP schema and freezes catalog, policy, lineage,
// idempotency, and fencing identity before the request becomes schedulable.

}  // namespace master_agent::atomic_service
