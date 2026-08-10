/**
 * @file dispatch_contracts.cpp
 * @brief Computes stable dispatch capability and child authorization digests.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

#include <algorithm>

namespace master_agent::agent_dispatch {

std::string dispatchCapabilityDigest(
    const std::string& action, std::uint32_t input_schema_version,
    std::uint32_t output_schema_version) {
    return secureDigest(
        "agent-capability-v1|" + action + "|" +
        std::to_string(input_schema_version) + "|" +
        std::to_string(output_schema_version));
}

std::string atomicChildIdempotencyKey(
    const std::string& parent_dispatch_id,
    const std::string& child_operation_id) {
    return secureDigest(
        "atomic-child|" + parent_dispatch_id + "|" +
        child_operation_id);
}

std::string dispatchChildAuthorizationDigest(
    const std::string& agent_id, std::uint64_t agent_epoch,
    const std::string& manifest_digest, const std::string& action,
    const std::vector<std::string>& granted_permissions,
    const std::vector<std::string>& allowed_child_capabilities) {
    auto permissions = granted_permissions;
    auto tools = allowed_child_capabilities;
    std::sort(permissions.begin(), permissions.end());
    std::sort(tools.begin(), tools.end());
    const nlohmann::json contract{
        {"contract", "dispatch-child-authorization-v1"},
        {"agent_id", agent_id},
        {"agent_epoch", agent_epoch},
        {"manifest_digest", manifest_digest},
        {"action", action},
        {"granted_permissions", permissions},
        {"allowed_child_capabilities", tools}};
    return secureDigest(contract.dump());
}


}  // namespace master_agent::agent_dispatch
