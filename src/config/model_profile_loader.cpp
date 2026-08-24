#include "master_agent/config/model_profile_loader.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace master_agent::config {
namespace {

struct ParsedYaml {
    std::map<std::string, std::string> scalars;
    std::map<std::string, std::vector<std::string>> arrays;
};

struct PathPart {
    std::size_t indent = 0;
    std::string key;
};

Status configError(std::string code, std::string message) {
    return Status::Error("config", std::move(code), std::move(message));
}

std::string trim(std::string value) {
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [](unsigned char character) { return std::isspace(character) != 0; });
    if (first == value.end()) return {};
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(),
        [](unsigned char character) { return std::isspace(character) != 0; })
                          .base();
    return std::string(first, last);
}

std::string stripComment(const std::string& line) {
    bool single_quoted = false;
    bool double_quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (character == '\'' && !double_quoted) single_quoted = !single_quoted;
        if (character == '"' && !single_quoted &&
            (index == 0 || line[index - 1] != '\\')) {
            double_quoted = !double_quoted;
        }
        if (character == '#' && !single_quoted && !double_quoted &&
            (index == 0 || std::isspace(
                               static_cast<unsigned char>(line[index - 1])))) {
            return line.substr(0, index);
        }
    }
    return line;
}

std::size_t colonOutsideQuotes(const std::string& value) {
    bool single_quoted = false;
    bool double_quoted = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character == '\'' && !double_quoted) single_quoted = !single_quoted;
        if (character == '"' && !single_quoted &&
            (index == 0 || value[index - 1] != '\\')) {
            double_quoted = !double_quoted;
        }
        if (character == ':' && !single_quoted && !double_quoted) return index;
    }
    return std::string::npos;
}

std::string unquote(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string pathOf(const std::vector<PathPart>& path,
                   const std::string& leaf = {}) {
    std::string result;
    for (const auto& part : path) {
        if (!result.empty()) result += ".";
        result += part.key;
    }
    if (!leaf.empty()) {
        if (!result.empty()) result += ".";
        result += leaf;
    }
    return result;
}

std::vector<std::string> parseInlineArray(const std::string& encoded) {
    std::vector<std::string> result;
    if (encoded.size() < 2 || encoded.front() != '[' ||
        encoded.back() != ']') {
        return result;
    }
    std::string item;
    bool single_quoted = false;
    bool double_quoted = false;
    const auto body = encoded.substr(1, encoded.size() - 2);
    for (std::size_t index = 0; index <= body.size(); ++index) {
        const char character = index < body.size() ? body[index] : ',';
        if (character == '\'' && !double_quoted) single_quoted = !single_quoted;
        if (character == '"' && !single_quoted &&
            (index == 0 || body[index - 1] != '\\')) {
            double_quoted = !double_quoted;
        }
        if (character == ',' && !single_quoted && !double_quoted) {
            const auto parsed = unquote(item);
            if (!parsed.empty()) result.push_back(parsed);
            item.clear();
        } else {
            item += character;
        }
    }
    return result;
}

Result<ParsedYaml> parseYamlSubset(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return Result<ParsedYaml>::Failure(configError(
            "CONFIG_RESOURCE_NOT_READABLE", path.string()));
    }
    ParsedYaml parsed;
    std::vector<PathPart> current_path;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find('\t') != std::string::npos) {
            return Result<ParsedYaml>::Failure(configError(
                "CONFIG_YAML_TAB_INDENTATION",
                path.string() + ":" + std::to_string(line_number)));
        }
        line = stripComment(line);
        const auto content = trim(line);
        if (content.empty() || content == "---") continue;
        const auto first_content = line.find_first_not_of(' ');
        const std::size_t indent =
            first_content == std::string::npos ? 0 : first_content;
        while (!current_path.empty() &&
               current_path.back().indent >= indent) {
            current_path.pop_back();
        }
        if (content.rfind("- ", 0) == 0) {
            if (current_path.empty()) {
                return Result<ParsedYaml>::Failure(configError(
                    "CONFIG_YAML_SEQUENCE_WITHOUT_KEY",
                    path.string() + ":" + std::to_string(line_number)));
            }
            parsed.arrays[pathOf(current_path)].push_back(
                unquote(content.substr(2)));
            continue;
        }
        const auto separator = colonOutsideQuotes(content);
        if (separator == std::string::npos) {
            return Result<ParsedYaml>::Failure(configError(
                "CONFIG_YAML_UNSUPPORTED_LINE",
                path.string() + ":" + std::to_string(line_number)));
        }
        const auto key = trim(content.substr(0, separator));
        const auto value = trim(content.substr(separator + 1));
        if (key.empty()) {
            return Result<ParsedYaml>::Failure(configError(
                "CONFIG_YAML_EMPTY_KEY",
                path.string() + ":" + std::to_string(line_number)));
        }
        if (value.empty()) {
            current_path.push_back({indent, key});
        } else if (value.front() == '[' && value.back() == ']') {
            parsed.arrays[pathOf(current_path, key)] =
                parseInlineArray(value);
        } else {
            parsed.scalars[pathOf(current_path, key)] = unquote(value);
        }
    }
    return Result<ParsedYaml>::Success(std::move(parsed));
}

std::string scalar(const ParsedYaml& parsed, const std::string& key) {
    const auto found = parsed.scalars.find(key);
    return found == parsed.scalars.end() ? std::string{} : found->second;
}

bool boolean(const ParsedYaml& parsed, const std::string& key,
             bool default_value = false) {
    const auto value = scalar(parsed, key);
    if (value == "true") return true;
    if (value == "false") return false;
    return default_value;
}

Result<ModelProfile> loadProfile(const std::filesystem::path& path) {
    const auto loaded = parseYamlSubset(path);
    if (!loaded.status.ok || !loaded.value) {
        return Result<ModelProfile>::Failure(loaded.status);
    }
    const auto& parsed = *loaded.value;
    if (scalar(parsed, "kind") != "ModelProfile") {
        return Result<ModelProfile>::Failure(configError(
            "CONFIG_MODEL_KIND_INVALID", path.string()));
    }
    ModelProfile profile;
    profile.id = scalar(parsed, "metadata.id");
    profile.enabled = boolean(parsed, "spec.enabled");
    profile.role = scalar(parsed, "spec.role");
    profile.location = scalar(parsed, "spec.location");
    profile.reality = scalar(parsed, "spec.reality");
    profile.runtime_type = scalar(parsed, "spec.runtime.type");
    profile.runtime_mode = scalar(parsed, "spec.runtime.mode");
    profile.endpoint_ref = scalar(parsed, "spec.runtime.endpoint_ref");
    profile.default_endpoint = scalar(parsed, "spec.runtime.default_endpoint");
    profile.credential_ref = scalar(parsed, "spec.runtime.credential_ref");
    profile.artifact_path_ref = scalar(parsed, "spec.artifact.path_ref");
    profile.model_name_ref = scalar(parsed, "spec.model.name_ref");
    const auto encoded_context = scalar(parsed, "spec.inference.context_length");
    if (!encoded_context.empty()) {
        try {
            const auto decoded = std::stoull(encoded_context);
            if (decoded == 0 || decoded >
                    static_cast<unsigned long long>(
                        std::numeric_limits<std::uint32_t>::max())) {
                throw std::out_of_range("context_length");
            }
            profile.context_length = static_cast<std::uint32_t>(decoded);
        } catch (...) {
            return Result<ModelProfile>::Failure(configError(
                "CONFIG_MODEL_CONTEXT_LENGTH_INVALID", path.string()));
        }
    }
    if (profile.id.empty() || profile.role.empty() ||
        profile.location.empty() || profile.runtime_type.empty()) {
        return Result<ModelProfile>::Failure(configError(
            "CONFIG_MODEL_REQUIRED_FIELD_MISSING", path.string()));
    }
    return Result<ModelProfile>::Success(std::move(profile));
}

Status validateSelectedProfile(const ModelProfile& profile,
                               const std::string& role,
                               const std::string& location) {
    if (!profile.enabled) {
        return configError("CONFIG_MODEL_DISABLED", profile.id);
    }
    if (profile.role != role) {
        return configError("CONFIG_MODEL_ROLE_MISMATCH", profile.id);
    }
    if (profile.location != location) {
        return configError("CONFIG_MODEL_LOCATION_MISMATCH", profile.id);
    }
    return Status::Ok();
}

bool contains(const std::vector<std::string>& values,
              const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

Result<ModelSelection> loadModelSelection(
    const std::filesystem::path& bundle_root,
    const std::optional<std::string>& intent_profile_override,
    const std::optional<std::string>& cloud_profile_override) {
    if (!std::filesystem::is_directory(bundle_root)) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_BUNDLE_NOT_FOUND", bundle_root.string()));
    }
    const auto manifest_result = parseYamlSubset(bundle_root / "manifest.yaml");
    if (!manifest_result.status.ok || !manifest_result.value) {
        return Result<ModelSelection>::Failure(manifest_result.status);
    }
    const auto& manifest = *manifest_result.value;
    if (scalar(manifest, "kind") != "MasterAgentConfigBundle") {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_BUNDLE_KIND_INVALID", bundle_root.string()));
    }
    ModelSelection selection;
    selection.bundle_root = std::filesystem::weakly_canonical(bundle_root);
    selection.bundle_name = scalar(manifest, "metadata.name");
    selection.bundle_version = scalar(manifest, "metadata.version");
    selection.routing_policy_id =
        scalar(manifest, "spec.defaults.model_routing");
    if (selection.bundle_name.empty() || selection.bundle_version.empty() ||
        selection.routing_policy_id.empty()) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_BUNDLE_MODEL_DEFAULTS_MISSING", bundle_root.string()));
    }

    std::map<std::string, ModelProfile> profiles;
    const auto models_directory = bundle_root / "models";
    if (!std::filesystem::is_directory(models_directory)) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_MODELS_DIRECTORY_MISSING", models_directory.string()));
    }
    for (const auto& entry : std::filesystem::directory_iterator(models_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".yaml") continue;
        const auto loaded = loadProfile(entry.path());
        if (!loaded.status.ok || !loaded.value) {
            return Result<ModelSelection>::Failure(loaded.status);
        }
        if (!profiles.emplace(loaded.value->id, *loaded.value).second) {
            return Result<ModelSelection>::Failure(configError(
                "CONFIG_MODEL_ID_DUPLICATE", loaded.value->id));
        }
    }

    std::optional<ParsedYaml> routing;
    const auto policies_directory = bundle_root / "policies";
    if (!std::filesystem::is_directory(policies_directory)) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_POLICIES_DIRECTORY_MISSING", policies_directory.string()));
    }
    for (const auto& entry : std::filesystem::directory_iterator(policies_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".yaml") continue;
        const auto loaded = parseYamlSubset(entry.path());
        if (!loaded.status.ok || !loaded.value) {
            return Result<ModelSelection>::Failure(loaded.status);
        }
        if (scalar(*loaded.value, "kind") == "ModelRoutingPolicy" &&
            scalar(*loaded.value, "metadata.id") == selection.routing_policy_id) {
            if (routing) {
                return Result<ModelSelection>::Failure(configError(
                    "CONFIG_MODEL_ROUTING_DUPLICATE", selection.routing_policy_id));
            }
            routing = std::move(*loaded.value);
        }
    }
    if (!routing) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_MODEL_ROUTING_NOT_FOUND", selection.routing_policy_id));
    }

    const auto& routes = *routing;
    const auto intent_candidates =
        routes.arrays.count("spec.roles.intent.on_device_candidates")
            ? routes.arrays.at("spec.roles.intent.on_device_candidates")
            : std::vector<std::string>{};
    const auto cloud_candidates =
        routes.arrays.count("spec.roles.intent.cloud_candidates")
            ? routes.arrays.at("spec.roles.intent.cloud_candidates")
            : std::vector<std::string>{};
    const auto classifier_candidates =
        routes.arrays.count("spec.roles.classifier.on_device_candidates")
            ? routes.arrays.at("spec.roles.classifier.on_device_candidates")
            : std::vector<std::string>{};

    const auto intent_id = intent_profile_override.value_or(
        scalar(routes, "spec.roles.intent.default"));
    const auto intent = profiles.find(intent_id);
    if (intent == profiles.end()) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_INTENT_MODEL_NOT_FOUND", intent_id));
    }
    if (!contains(intent_candidates, intent_id)) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_INTENT_MODEL_NOT_ALLOWED", intent_id));
    }
    const auto intent_status =
        validateSelectedProfile(intent->second, "intent", "on_device");
    if (!intent_status.ok) {
        return Result<ModelSelection>::Failure(intent_status);
    }
    selection.intent = intent->second;

    const auto classifier_id = scalar(routes, "spec.roles.classifier.default");
    const auto classifier = profiles.find(classifier_id);
    if (classifier == profiles.end()) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_CLASSIFIER_MODEL_NOT_FOUND", classifier_id));
    }
    if (!contains(classifier_candidates, classifier_id)) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_CLASSIFIER_MODEL_NOT_ALLOWED", classifier_id));
    }
    const auto classifier_status =
        validateSelectedProfile(classifier->second, "classifier", "on_device");
    if (!classifier_status.ok) {
        return Result<ModelSelection>::Failure(classifier_status);
    }
    selection.classifier = classifier->second;

    selection.cloud_fallback_enabled =
        boolean(routes, "spec.cloud_fallback.enabled");
    if (cloud_profile_override && !selection.cloud_fallback_enabled) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_CLOUD_FALLBACK_DISABLED", *cloud_profile_override));
    }
    std::string cloud_id;
    if (cloud_profile_override) {
        cloud_id = *cloud_profile_override;
    } else if (selection.cloud_fallback_enabled) {
        for (const auto& candidate_id : cloud_candidates) {
            const auto candidate = profiles.find(candidate_id);
            if (candidate != profiles.end() && candidate->second.enabled) {
                cloud_id = candidate_id;
                break;
            }
        }
    }
    if (selection.cloud_fallback_enabled && cloud_id.empty()) {
        return Result<ModelSelection>::Failure(configError(
            "CONFIG_CLOUD_MODEL_NOT_AVAILABLE", selection.routing_policy_id));
    }
    if (!cloud_id.empty()) {
        if (!contains(cloud_candidates, cloud_id)) {
            return Result<ModelSelection>::Failure(configError(
                "CONFIG_CLOUD_MODEL_NOT_ALLOWED", cloud_id));
        }
        const auto cloud = profiles.find(cloud_id);
        if (cloud == profiles.end()) {
            return Result<ModelSelection>::Failure(configError(
                "CONFIG_CLOUD_MODEL_NOT_FOUND", cloud_id));
        }
        const auto cloud_status =
            validateSelectedProfile(cloud->second, "intent", "cloud");
        if (!cloud_status.ok) {
            return Result<ModelSelection>::Failure(cloud_status);
        }
        selection.cloud_intent = cloud->second;
    }
    return Result<ModelSelection>::Success(std::move(selection));
}

}  // namespace master_agent::config
