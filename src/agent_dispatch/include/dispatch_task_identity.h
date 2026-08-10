#pragma once

/**
 * @file dispatch_task_identity.h
 * @brief Private canonical identity calculation for admitted dispatch tasks.
 *
 * The digest covers every immutable field that participates in idempotency and
 * recovery. Persistence and admission share this definition so that restored
 * work is checked against exactly the same task identity as newly admitted
 * work.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

namespace master_agent::agent_dispatch {
namespace {

std::string taskDigest(const DispatchTask& task) {
    nlohmann::json encoded{
        {"caller_module_id", toString(task.caller_module_id)},
        {"request_id", task.request_id},
        {"plan_id", task.plan_id},
        {"pid", task.pid},
        {"activation_id", task.activation_id},
        {"execution_id", task.execution_id},
        {"attempt_no", task.attempt_no},
        {"operation_id", task.operation_id},
        {"task_id", task.task_id},
        {"action", task.action},
        {"target_agent", task.target_agent},
        {"allow_agent_fallback", task.allow_agent_fallback},
        {"params", task.params},
        {"input_schema_version", task.input_schema_version},
        {"expected_output_schema_version",
         task.expected_output_schema_version},
        {"priority", toString(task.priority)},
        {"deadline_mono_ns", task.deadline_mono_ns},
        {"fencing_token", task.fencing_token},
        {"capability_digest", task.capability_digest},
        {"capacity_epoch", task.capacity_epoch},
        {"resource_lease_refs", task.resource_lease_refs},
        {"granted_permissions", task.granted_permissions},
        {"allowed_child_capabilities",
         task.allowed_child_capabilities},
        {"child_authorization_digest",
         task.child_authorization_digest},
        {"principal_id_hash", task.principal_id_hash},
        {"authorization_ref", task.authorization_ref},
        {"trace_id", task.trace_id}};
    return secureDigest(encoded.dump());
}

}  // namespace
}  // namespace master_agent::agent_dispatch
