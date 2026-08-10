#pragma once

/**
 * @file manifest_validation.h
 * @brief Private validation helpers for manifests and delegated claims.
 *
 * Manifest declarations and child grants are bounded, unique, and scoped to a
 * declared action. The helpers in this header support registry admission and
 * child-call lineage validation without exposing policy internals publicly.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

#include <algorithm>
#include <set>

namespace master_agent::agent_dispatch {
namespace {

bool validBoundedUniqueClaims(
    const std::vector<std::string>& claims) {
    if (claims.size() > 64) return false;
    std::set<std::string> unique;
    for (const auto& claim : claims) {
        if (claim.empty() || claim.size() > 256 ||
            !unique.insert(claim).second) {
            return false;
        }
    }
    return true;
}

// A requirement is bound to one declared action. This prevents an auxiliary
// grant from silently becoming available to every capability of the Agent.
[[maybe_unused]] bool validManifestRequirements(
    const sub_agents::AgentManifest& manifest,
    const std::map<std::string, std::vector<std::string>>& requirements) {
    if (requirements.size() > manifest.capabilities.size()) {
        return false;
    }
    for (const auto& [action, claims] : requirements) {
        if (std::find(manifest.capabilities.begin(),
                      manifest.capabilities.end(),
                      action) == manifest.capabilities.end() ||
            !validBoundedUniqueClaims(claims)) {
            return false;
        }
    }
    return true;
}

[[maybe_unused]] std::vector<std::string> manifestClaims(
    const std::map<std::string, std::vector<std::string>>& requirements,
    const std::string& action) {
    const auto found = requirements.find(action);
    if (found == requirements.end()) return {};
    auto claims = found->second;
    std::sort(claims.begin(), claims.end());
    return claims;
}

[[maybe_unused]] bool claimsContainAll(
    const std::vector<std::string>& admission,
    const std::vector<std::string>& required) {
    const std::set<std::string> available(
        admission.begin(), admission.end());
    return std::all_of(
        required.begin(), required.end(),
        [&available](const auto& claim) {
            return available.count(claim) != 0;
        });
}

}  // namespace
}  // namespace master_agent::agent_dispatch

