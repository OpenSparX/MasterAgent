#pragma once

/**
 * @file model_profile_loader.h
 * @brief Loads the model-selection slice of a MasterAgent config bundle.
 */

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "master_agent/common/types.h"

namespace master_agent::config {

struct ModelProfile {
    std::string id;
    bool enabled = false;
    std::string role;
    std::string location;
    std::string reality;
    std::string runtime_type;
    std::string runtime_mode;
    std::string endpoint_ref;
    std::string default_endpoint;
    std::string credential_ref;
    std::string artifact_path_ref;
    std::string model_name_ref;
    std::uint32_t context_length = 4096;
};

struct ModelSelection {
    std::filesystem::path bundle_root;
    std::string bundle_name;
    std::string bundle_version;
    std::string routing_policy_id;
    ModelProfile intent;
    ModelProfile classifier;
    bool cloud_fallback_enabled = false;
    std::optional<ModelProfile> cloud_intent;
};

/**
 * Loads and cross-checks the YAML resources under models/ and the configured
 * ModelRoutingPolicy.
 *
 * This is intentionally the first, narrow ConfigBundle runtime slice. It does
 * not claim that rules, skills, capabilities, prompts, or policies have moved
 * off the legacy config path yet. The full bundle compiler will replace this
 * constrained YAML reader with a schema-validated immutable snapshot.
 */
Result<ModelSelection> loadModelSelection(
    const std::filesystem::path& bundle_root,
    const std::optional<std::string>& intent_profile_override = std::nullopt,
    const std::optional<std::string>& cloud_profile_override = std::nullopt);

}  // namespace master_agent::config
