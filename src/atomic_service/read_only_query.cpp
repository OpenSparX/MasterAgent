/**
 * @file read_only_query.cpp
 * @brief Executes the bounded MCP read path used by Intent QUERY_BATCH.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

Result<AtomicReadOnlyResult> AtomicServiceManager::queryReadOnly(
    const AtomicReadOnlyMcpRequest& query,
    const CallContext& call) {
    ToolRecord tool;
    AtomicMcpCallEnvelope envelope;
    AtomicProviderInvocationSeal seal;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!hasHostModuleIdentity(
                call, CallerModuleId::IntentRecognitionEngine) ||
            !clock_ || !ids_ || query.mcp_request.jsonrpc != "2.0" ||
            query.mcp_request.id.empty() ||
            query.mcp_request.method != "tools/call" ||
            query.mcp_request.name.empty() ||
            !query.mcp_request.arguments.is_object() ||
            call.request_id.empty() || call.trace_id.empty() ||
            call.principal_id_hash.empty() ||
            !isValidTaskPriority(call.priority) ||
            call.deadline_mono_ns <= 0 ||
            deadlineExpired(call.deadline_mono_ns, *clock_)) {
            return Result<AtomicReadOnlyResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_READ_ONLY_QUERY_INVALID",
                "read-only query identity or deadline is invalid"));
        }
        if (!durability_status_.ok) {
            return Result<AtomicReadOnlyResult>::Failure(
                durability_status_);
        }
        if (query.expected_catalog_digest.empty() ||
            query.expected_catalog_digest != catalog_.catalog_digest) {
            return Result<AtomicReadOnlyResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_READ_ONLY_CATALOG_STALE",
                "read-only query was planned against another Tool catalog"));
        }
        const auto found = tools_.find(query.mcp_request.name);
        if (found == tools_.end() ||
            !found->second.definition.annotations.read_only_hint ||
            found->second.definition.annotations.destructive_hint ||
            found->second.policy.idempotency_policy != "READ_ONLY") {
            return Result<AtomicReadOnlyResult>::Failure(Status::Error(
                "atomic_service",
                "ATOMIC_READ_ONLY_CAPABILITY_REQUIRED",
                "target Tool is not sealed as READ_ONLY"));
        }
        const auto arguments = validateArguments(
            found->second.definition.input_schema,
            query.mcp_request.arguments);
        if (!arguments.ok) {
            return Result<AtomicReadOnlyResult>::Failure(arguments);
        }
        const auto has_permissions = std::all_of(
            found->second.policy.required_permissions.begin(),
            found->second.policy.required_permissions.end(),
            [&query](const std::string& required) {
                return std::find(
                           query.granted_permissions.begin(),
                           query.granted_permissions.end(), required) !=
                       query.granted_permissions.end();
            });
        if (!has_permissions) {
            return Result<AtomicReadOnlyResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_PERMISSION_DENIED",
                "read-only query lacks a required permission"));
        }
        const auto asynchronous_inflight = std::count_if(
            provider_inflight_.begin(), provider_inflight_.end(),
            [this, &query](const std::string& execution_id) {
                const auto execution = executions_.find(execution_id);
                return execution != executions_.end() &&
                       execution->second.envelope.mcp_request.name ==
                           query.mcp_request.name;
            });
        std::uint64_t total_read_only_inflight = 0;
        for (const auto& item : read_only_inflight_) {
            total_read_only_inflight += item.second;
        }
        const auto current_read_only = read_only_inflight_.find(
            query.mcp_request.name);
        const auto tool_read_only_inflight =
            current_read_only == read_only_inflight_.end()
                ? 0U
                : current_read_only->second;
        if (tool_read_only_inflight + asynchronous_inflight >=
                found->second.policy.max_concurrency ||
            provider_inflight_.size() + total_read_only_inflight >=
                max_inflight_) {
            return Result<AtomicReadOnlyResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_READ_ONLY_CAPACITY_EXHAUSTED",
                "read-only Tool capacity is currently exhausted", true));
        }
        ++read_only_inflight_[query.mcp_request.name];
        tool = found->second;

        envelope.mcp_request = query.mcp_request;
        envelope.runtime.caller_module_id =
            CallerModuleId::IntentRecognitionEngine;
        envelope.runtime.request_id = call.request_id;
        envelope.runtime.trace_id = call.trace_id;
        envelope.runtime.execution_id = ids_->next("atomic-read");
        envelope.runtime.operation_id =
            "read-only:" + query.mcp_request.id;
        envelope.runtime.priority = call.priority;
        envelope.runtime.deadline_mono_ns = call.deadline_mono_ns;
        envelope.runtime.idempotency_key =
            "read-only:" + call.request_id + ":" +
            query.mcp_request.id;
        envelope.runtime.tool_catalog_snapshot_id = catalog_.snapshot_id;
        envelope.runtime.tool_digest = tool.policy.tool_digest;
        envelope.runtime.policy_digest = tool.policy.policy_digest;
        envelope.runtime.granted_permissions = query.granted_permissions;
        envelope.runtime.principal_id_hash = call.principal_id_hash;
        envelope.runtime.authorization_ref = call.authorization_ref;

        seal.invocation_id = ids_->next("atomic-read-invocation");
        seal.provider_id = tool.provider_id;
        seal.provider_epoch = tool.provider_epoch;
        seal.operation_id = envelope.runtime.operation_id;
        seal.execution_id = envelope.runtime.execution_id;
        seal.attempt_no = 1;
        seal.tool_name = query.mcp_request.name;
        seal.tool_catalog_snapshot_id = catalog_.snapshot_id;
        seal.tool_digest = tool.policy.tool_digest;
        seal.policy_digest = tool.policy.policy_digest;
        seal.fencing_token = 0;
        seal.request_digest = callDigest(envelope);
    }

    ProviderInvocationResult invoked;
    try {
        invoked = tool.provider->call(envelope, seal);
    } catch (...) {
        invoked.state = ProviderInvocationState::Failed;
        invoked.error_code = "ATOMIC_READ_ONLY_PROVIDER_EXCEPTION";
        invoked.invocation_seal = seal;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = read_only_inflight_.find(
            query.mcp_request.name);
        if (found != read_only_inflight_.end()) {
            if (found->second > 0) --found->second;
            if (found->second == 0) read_only_inflight_.erase(found);
        }
    }

    if (deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<AtomicReadOnlyResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_READ_ONLY_RESULT_AFTER_DEADLINE",
            "late read-only evidence was discarded", false,
            SideEffectState::NotApplicable));
    }
    if (!invocationSealMatches(invoked.invocation_seal, seal)) {
        return Result<AtomicReadOnlyResult>::Failure(Status::Error(
            "atomic_service", "ATOMIC_READ_ONLY_SEAL_MISMATCH",
            "Provider response does not match the frozen invocation"));
    }
    if (invoked.state != ProviderInvocationState::Succeeded ||
        invoked.result.is_error ||
        invoked.side_effect_state == SideEffectState::Unknown) {
        return Result<AtomicReadOnlyResult>::Failure(Status::Error(
            "atomic_service",
            invoked.error_code.empty()
                ? "ATOMIC_READ_ONLY_PROVIDER_FAILED"
                : invoked.error_code,
            "read-only Provider did not return a confirmed result",
            invoked.retryable_hint, invoked.side_effect_state));
    }
    if (!tool.definition.output_schema.empty()) {
        const auto output = validateArguments(
            tool.definition.output_schema,
            invoked.result.structured_content);
        if (!output.ok) {
            return Result<AtomicReadOnlyResult>::Failure(Status::Error(
                "atomic_service", "ATOMIC_READ_ONLY_OUTPUT_INVALID",
                "read-only Provider result violates outputSchema"));
        }
    }

    AtomicReadOnlyResult result;
    result.query_id = query.mcp_request.id;
    result.tool_name = query.mcp_request.name;
    result.result = std::move(invoked.result);
    result.catalog_snapshot_id =
        envelope.runtime.tool_catalog_snapshot_id;
    result.tool_digest = tool.policy.tool_digest;
    result.policy_digest = tool.policy.policy_digest;
    result.provider_id = tool.provider_id;
    result.provider_epoch = tool.provider_epoch;
    result.observed_at_utc_ms = clock_->utcNowMs();
    return Result<AtomicReadOnlyResult>::Success(std::move(result));
}

}  // namespace master_agent::atomic_service
