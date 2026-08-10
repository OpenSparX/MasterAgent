#pragma once

/**
 * @file provider_identity.h
 * @brief Private identity and lineage checks for SubAgent observations.
 *
 * A provider result is accepted only when it is bound to the current dispatch,
 * route, lease, schema, authorization, and control epoch. This prevents stale
 * or cross-dispatch observations from being committed as current results.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

#include <optional>

namespace master_agent::agent_dispatch {
namespace {

bool providerIdentityMatches(
    const sub_agents::SubAgentSnapshot& provider,
    const DispatchSnapshot& dispatch,
    const std::optional<std::uint64_t>& expected_control_epoch =
        std::nullopt) {
    const auto& request = provider.request;
    const auto& task = dispatch.task;
    const auto& route = dispatch.route;
    const bool output_schema_proof =
        provider.state != sub_agents::SubAgentState::Succeeded ||
        (provider.result.is_object() &&
         provider.result.contains("output_schema_version") &&
         provider.result["output_schema_version"].is_number_unsigned() &&
         provider.result["output_schema_version"]
                 .get<std::uint32_t>() ==
             task.expected_output_schema_version);
    return output_schema_proof &&
           request.dispatch_id == dispatch.dispatch_id &&
           request.request_id == task.request_id &&
           request.pid == task.pid &&
           request.activation_id == task.activation_id &&
           request.attempt_no == task.attempt_no &&
           request.operation_id == task.operation_id &&
           request.execution_id == task.execution_id &&
           request.agent_id == route.agent_id &&
           request.agent_epoch == route.agent_epoch &&
           request.manifest_digest == route.manifest_digest &&
           request.lease_id == route.lease_id &&
           request.action == task.action &&
           request.params == task.params &&
           request.capability_digest == task.capability_digest &&
           request.expected_output_schema_version ==
               task.expected_output_schema_version &&
           request.priority == task.priority &&
           request.deadline_mono_ns == task.deadline_mono_ns &&
           request.fencing_token == task.fencing_token &&
           request.trace_id == task.trace_id &&
           request.principal_id_hash == task.principal_id_hash &&
           request.authorization_ref == task.authorization_ref &&
           provider.output_schema_version ==
               task.expected_output_schema_version &&
           provider.control_epoch ==
               expected_control_epoch.value_or(dispatch.control_epoch);
}

}  // namespace
}  // namespace master_agent::agent_dispatch

