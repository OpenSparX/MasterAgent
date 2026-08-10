/**
 * @file admission_validator.cpp
 * @brief Validates MCP arguments against registered input schemas.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

Status AtomicServiceManager::validateArguments(
    const nlohmann::json& schema, const nlohmann::json& arguments) {

    const auto schema_status =
        validateObjectSchemaSubset(schema);
    if (!schema_status.ok) {
        return Status::Error("atomic_service",
                             "ATOMIC_ARGUMENT_SCHEMA_INVALID",
                             "registered argument schema is invalid");
    }
    try {
        if (!arguments.is_object() ||
            arguments.size() > 256 ||
            arguments.dump().size() > 1024U * 1024U) {
            return Status::Error(
                "atomic_service",
                "ATOMIC_ARGUMENT_SCHEMA_INVALID",
                "arguments must be a bounded object");
        }
        const nlohmann::json empty_properties =
            nlohmann::json::object();
        const auto& properties =
            schema.contains("properties")
                ? schema.at("properties")
                : empty_properties;
        if (schema.contains("required")) {
            for (const auto& required :
                 schema.at("required")) {
                const auto name =
                    required.get<std::string>();
                if (!arguments.contains(name)) {
                    return Status::Error(
                        "atomic_service",
                        "ATOMIC_ARGUMENT_REQUIRED",
                        "required argument is missing");
                }
            }
        }
        // treats omitted additionalProperties as fail-closed. This is
        // intentionally stricter than generic JSON Schema and matches the
        // security boundary of a physical capability call.
        for (auto item = arguments.begin(); item != arguments.end(); ++item) {
            if (!properties.contains(item.key())) {
                return Status::Error("atomic_service",
                                     "ATOMIC_ARGUMENT_UNKNOWN",
                                     "unknown argument is not allowed");
            }
        }
        for (auto item = arguments.begin();
             item != arguments.end(); ++item) {
            const auto& property =
                properties.at(item.key());
            const auto type =
                property.at("type").get<std::string>();
            if (!jsonMatchesType(item.value(), type)) {
                return Status::Error(
                    "atomic_service",
                    "ATOMIC_ARGUMENT_TYPE_INVALID",
                    "argument type does not match schema");
            }
            if (item.value().is_string() &&
                !safeSchemaText(
                    item.value().get<std::string>(),
                    16U * 1024U)) {
                return Status::Error(
                    "atomic_service",
                    "ATOMIC_ARGUMENT_VALUE_INVALID",
                    "argument string exceeds the runtime boundary");
            }
            if (property.contains("enum") &&
                std::find(
                    property.at("enum").begin(),
                    property.at("enum").end(),
                    item.value()) ==
                    property.at("enum").end()) {
                return Status::Error(
                    "atomic_service",
                    "ATOMIC_ARGUMENT_ENUM_INVALID",
                    "argument is outside enum");
            }
        }
        return Status::Ok();
    } catch (...) {
        return Status::Error(
            "atomic_service",
            "ATOMIC_ARGUMENT_SCHEMA_INVALID",
            "argument validation failed safely");
    }
}

std::string AtomicServiceManager::resourceKey(
    const AtomicToolRuntimePolicy& policy,
    const nlohmann::json& arguments) {
    std::string key = policy.tool_name;
    for (const auto& field : policy.resource_argument_fields) {
        key += "|" + field + "=";
        if (arguments.contains(field)) key += arguments.at(field).dump();
    }
    return key;
}

Status AtomicServiceManager::validateCall(
    const AtomicMcpCallEnvelope& request, const CallContext& call,
    const ToolRecord** record,
    bool allow_recovered_catalog_snapshot) const {

    if (call.caller != request.runtime.caller_module_id ||
        !hasHostModuleIdentity(call, call.caller) ||
        (call.caller != CallerModuleId::TaskOrchestrationEngine &&
         call.caller != CallerModuleId::SubAgent)) {
        return Status::Error("atomic_service",
                             "ATOMIC_CALLER_MODULE_NOT_ALLOWED",
                             "MCP tools/call requires Orchestrator or SubAgent");
    }
    if (call.caller == CallerModuleId::SubAgent &&
        (!request.runtime.parent_operation_id ||
         request.runtime.parent_operation_id->empty() ||
         request.runtime.parent_dispatch_id.empty() ||
         request.runtime.parent_agent_id.empty() ||
         request.runtime.parent_agent_epoch == 0 ||
         request.runtime.parent_lease_id.empty() ||
         request.runtime.parent_fencing_token == 0)) {
        return Status::Error("atomic_service",
                             "ATOMIC_PARENT_OPERATION_REQUIRED",
                             "SubAgent call requires parent lineage");
    }
    if (request.mcp_request.jsonrpc != "2.0" ||
        request.mcp_request.method != "tools/call" ||
        request.mcp_request.id.empty() ||
        request.mcp_request.id != request.runtime.operation_id) {
        return Status::Error("atomic_service", "ATOMIC_MCP_REQUEST_INVALID",
                             "invalid MCP tools/call envelope");
    }
    const auto found = tools_.find(request.mcp_request.name);
    if (found == tools_.end()) {
        return Status::Error("atomic_service", "ATOMIC_TOOL_NOT_FOUND",
                             "tool is not in frozen catalog");
    }
    if (request.runtime.request_id.empty() ||
        request.runtime.trace_id.empty() ||
        request.runtime.plan_id.empty() ||
        request.runtime.pid.empty() ||
        request.runtime.activation_id.empty() ||
        request.runtime.execution_id.empty() ||
        request.runtime.operation_id.empty() ||
        request.runtime.idempotency_key.empty() ||
        request.runtime.fencing_token == 0 ||
        request.runtime.attempt_no == 0 ||
        request.runtime.deadline_mono_ns <= 0 ||
        request.runtime.principal_id_hash.empty() ||
        request.runtime.authorization_ref.empty() ||
        !isValidTaskPriority(request.runtime.priority) ||
        !isValidTaskPriority(call.priority) ||
        (!allow_recovered_catalog_snapshot &&
         request.runtime.tool_catalog_snapshot_id !=
             catalog_.snapshot_id) ||
        request.runtime.tool_digest != found->second.policy.tool_digest ||
        request.runtime.policy_digest != found->second.policy.policy_digest) {
        return Status::Error("atomic_service",
                             "ATOMIC_RUNTIME_CONTEXT_INVALID",
                             "trusted runtime context is incomplete or stale");
    }
    if (call.request_id != request.runtime.request_id ||
        call.trace_id != request.runtime.trace_id ||
        call.principal_id_hash != request.runtime.principal_id_hash ||
        call.priority != request.runtime.priority ||
        call.authorization_ref !=
            request.runtime.authorization_ref ||
        call.deadline_mono_ns !=
            request.runtime.deadline_mono_ns) {
        return Status::Error("atomic_service",
                             "ATOMIC_CALL_IDENTITY_MISMATCH",
                             "IPC call identity does not bind runtime envelope");
    }
    if (request.runtime.priority == TaskPriority::P0 &&
        request.runtime.authorization_ref.rfind(
            "trusted-safety:", 0) != 0) {
        return Status::Error("atomic_service",
                             "ATOMIC_P0_AUTHORIZATION_REQUIRED",
                             "P0 requires a trusted safety authorization ref");
    }
    const std::set<std::string> granted_permissions{
        request.runtime.granted_permissions.begin(),
        request.runtime.granted_permissions.end()};
    const auto missing_permission = std::find_if(
        found->second.policy.required_permissions.begin(),
        found->second.policy.required_permissions.end(),
        [&](const std::string& permission) {
            return granted_permissions.count(permission) == 0;
        });
    if (missing_permission !=
        found->second.policy.required_permissions.end()) {
        return Status::Error(
            "atomic_service", "ATOMIC_PERMISSION_DENIED",
            "authorization claims do not grant every Tool permission");
    }
    if (std::any_of(
            request.runtime.resource_lease_refs.begin(),
            request.runtime.resource_lease_refs.end(),
            [](const std::string& lease_ref) {
                return lease_ref.rfind("lease-v2:", 0) != 0 ||
                       lease_ref.size() <=
                           std::string{"lease-v2:"}.size();
            })) {
        return Status::Error(
            "atomic_service", "ATOMIC_RESOURCE_LEASE_INVALID",
            "resource lease references must be issued lease IDs");
    }
    if (deadlineExpired(request.runtime.deadline_mono_ns, *clock_)) {
        return Status::Error("atomic_service", "ATOMIC_DEADLINE_EXPIRED",
                             "atomic deadline expired");
    }
    const auto schema = validateArguments(found->second.definition.input_schema,
                                          request.mcp_request.arguments);
    if (!schema.ok) return schema;
    *record = &found->second;
    return Status::Ok();
}


}  // namespace master_agent::atomic_service
