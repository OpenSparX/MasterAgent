/**
 * @file sparx_model_registry.cpp
 * @brief Unified Model Registry — implementation.
 */

#include "sparx_model_registry.h"

#include <algorithm>
#include <sstream>

namespace sparx {

// ─── Type/Role string conversion ────────────────────────────────────────────

const char* modelTypeStr(ModelType type) {
    switch (type) {
        case ModelType::Local:  return "local";
        case ModelType::Server: return "server";
        case ModelType::Cloud:  return "cloud";
    }
    return "unknown";
}

const char* modelRoleStr(ModelRole role) {
    switch (role) {
        case ModelRole::Default:   return "default";
        case ModelRole::Cloud:     return "cloud";
        case ModelRole::Reasoning: return "reasoning";
        case ModelRole::Embedding: return "embedding";
        case ModelRole::Fallback:  return "fallback";
    }
    return "unknown";
}

ModelType parseModelType(const std::string& s) {
    if (s == "local")  return ModelType::Local;
    if (s == "server") return ModelType::Server;
    if (s == "cloud")  return ModelType::Cloud;
    return ModelType::Local;  // default
}

ModelRole parseModelRole(const std::string& s) {
    if (s == "default")   return ModelRole::Default;
    if (s == "cloud")     return ModelRole::Cloud;
    if (s == "reasoning") return ModelRole::Reasoning;
    if (s == "embedding") return ModelRole::Embedding;
    if (s == "fallback")  return ModelRole::Fallback;
    return ModelRole::Default;
}

// ─── ModelRegistry ──────────────────────────────────────────────────────────

void ModelRegistry::add(ModelEntry entry) {
    // Replace if same name exists
    for (auto& m : models_) {
        if (m.name == entry.name) {
            m = std::move(entry);
            return;
        }
    }
    models_.push_back(std::move(entry));
}

const ModelEntry* ModelRegistry::get(const std::string& name) const {
    for (const auto& m : models_) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

const ModelEntry* ModelRegistry::getByRole(ModelRole role) const {
    for (const auto& m : models_) {
        if (m.role == role) return &m;
    }
    return nullptr;
}

const ModelEntry* ModelRegistry::defaultModel() const {
    // First try routing config name
    if (!routing_.default_model.empty()) {
        if (auto* m = get(routing_.default_model)) return m;
    }
    // Fall back to first model with Default role
    if (auto* m = getByRole(ModelRole::Default)) return m;
    // Last resort: first model
    return models_.empty() ? nullptr : &models_[0];
}

const ModelEntry* ModelRegistry::cloudModel() const {
    if (!routing_.cloud_model.empty()) {
        if (auto* m = get(routing_.cloud_model)) return m;
    }
    return getByRole(ModelRole::Cloud);
}

const ModelEntry* ModelRegistry::reasoningModel() const {
    if (!routing_.reasoning_model.empty()) {
        if (auto* m = get(routing_.reasoning_model)) return m;
    }
    return getByRole(ModelRole::Reasoning);
}

bool ModelRegistry::has(const std::string& name) const {
    return get(name) != nullptr;
}

ModelRegistry::ValidationResult ModelRegistry::validate() const {
    ValidationResult result;

    if (models_.empty()) {
        result.errors.push_back("No models configured");
        result.valid = false;
        return result;
    }

    // Check routing references exist
    if (!routing_.default_model.empty() && !has(routing_.default_model)) {
        result.errors.push_back(
            "routing.default_model '" + routing_.default_model +
            "' not found in models list");
        result.valid = false;
    }
    if (!routing_.cloud_model.empty() && !has(routing_.cloud_model)) {
        result.errors.push_back(
            "routing.cloud_model '" + routing_.cloud_model +
            "' not found in models list");
        result.valid = false;
    }
    if (!routing_.reasoning_model.empty() && !has(routing_.reasoning_model)) {
        result.errors.push_back(
            "routing.reasoning_model '" + routing_.reasoning_model +
            "' not found in models list");
        result.valid = false;
    }

    // Check each model entry
    for (const auto& m : models_) {
        if (m.name.empty()) {
            result.errors.push_back("Model entry missing 'name' field");
            result.valid = false;
        }
        if (m.type == ModelType::Local && m.path.empty()) {
            result.warnings.push_back(
                "Local model '" + m.name + "' has no path (will use $SPARX_MODEL or --model)");
        }
        if (m.type == ModelType::Cloud && m.endpoint.empty()) {
            result.errors.push_back(
                "Cloud model '" + m.name + "' missing endpoint URL");
            result.valid = false;
        }
        if (m.type == ModelType::Cloud && m.api_key_env.empty()) {
            result.warnings.push_back(
                "Cloud model '" + m.name + "' has no api_key_env (will need auth header)");
        }
    }

    // Warn if cloud routing enabled but no cloud model
    if (routing_.cloud_enabled && !cloudModel()) {
        result.warnings.push_back(
            "routing.cloud_enabled is true but no cloud model configured");
    }

    return result;
}

std::string ModelRegistry::summary() const {
    std::ostringstream oss;
    oss << "Model Registry: " << models_.size() << " model(s)\n";
    for (const auto& m : models_) {
        oss << "  • " << m.name
            << " [" << modelTypeStr(m.type) << "]"
            << " role=" << modelRoleStr(m.role)
            << " ctx=" << m.context_length;
        if (m.type == ModelType::Cloud) {
            oss << " endpoint=" << m.endpoint.substr(0, 40);
            if (m.endpoint.size() > 40) oss << "...";
        } else if (!m.path.empty()) {
            oss << " path=" << m.path;
        }
        oss << "\n";
    }
    oss << "Routing: default=" << routing_.default_model;
    if (routing_.cloud_enabled) {
        oss << " cloud=" << routing_.cloud_model
            << " threshold=" << routing_.complexity_threshold;
    }
    oss << "\n";
    return oss.str();
}

// ─── YAML Parser ────────────────────────────────────────────────────────────

namespace {

std::string trimQuotes(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

int indentLevel(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ') ++n;
        else break;
    }
    return n;
}

std::string extractValue(const std::string& line) {
    auto colon = line.find(':');
    if (colon == std::string::npos) return "";
    return trimQuotes(line.substr(colon + 1));
}

}  // namespace

bool parseModelRegistry(const std::string& content, ModelRegistry& registry) {
    std::istringstream stream(content);
    std::string line;

    enum Section { None, Models, Routing } section = None;
    bool in_model_entry = false;
    ModelEntry current_entry;

    auto flushEntry = [&]() {
        if (!current_entry.name.empty()) {
            if (current_entry.model_id.empty()) {
                current_entry.model_id = current_entry.name;
            }
            registry.add(std::move(current_entry));
            current_entry = ModelEntry{};
        }
    };

    RoutingConfig routing;

    while (std::getline(stream, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Skip empty/comment
        if (line.empty() || line[0] == '#') continue;

        int indent = indentLevel(line);

        // Top-level section detection
        if (indent == 0) {
            if (line.find("models:") == 0) {
                flushEntry();
                section = Models;
                in_model_entry = false;
                continue;
            }
            if (line.find("routing:") == 0) {
                flushEntry();
                section = Routing;
                in_model_entry = false;
                continue;
            }
            // Other top-level keys end our sections
            if (section != None) {
                flushEntry();
                section = None;
            }
            continue;
        }

        if (section == Models) {
            // List item start: "  - name: xxx" or "    - name: xxx"
            auto dash = line.find("- ");
            if (dash != std::string::npos && indent <= 4) {
                flushEntry();
                in_model_entry = true;
                // The dash line might have "name:" on it
                auto name_pos = line.find("name:");
                if (name_pos != std::string::npos) {
                    current_entry.name = trimQuotes(line.substr(name_pos + 5));
                }
                continue;
            }

            if (!in_model_entry) continue;

            // Model entry fields (indented under the list item)
            std::string val = extractValue(line);
            if (line.find("name:") != std::string::npos && indent >= 4) {
                current_entry.name = val;
            } else if (line.find("type:") != std::string::npos) {
                current_entry.type = parseModelType(val);
            } else if (line.find("role:") != std::string::npos) {
                current_entry.role = parseModelRole(val);
            } else if (line.find("path:") != std::string::npos) {
                current_entry.path = val;
            } else if (line.find("endpoint:") != std::string::npos) {
                current_entry.endpoint = val;
            } else if (line.find("api_key_env:") != std::string::npos) {
                current_entry.api_key_env = val;
            } else if (line.find("model_id:") != std::string::npos) {
                current_entry.model_id = val;
            } else if (line.find("context_length:") != std::string::npos) {
                try { current_entry.context_length = std::stoi(val); } catch (...) {}
            } else if (line.find("max_output_tokens:") != std::string::npos) {
                try { current_entry.max_output_tokens = std::stoi(val); } catch (...) {}
            } else if (line.find("temperature:") != std::string::npos) {
                try { current_entry.temperature = std::stof(val); } catch (...) {}
            } else if (line.find("supports_code:") != std::string::npos) {
                current_entry.supports_code = (val == "true");
            } else if (line.find("supports_reasoning:") != std::string::npos) {
                current_entry.supports_reasoning = (val == "true");
            } else if (line.find("supports_tools:") != std::string::npos) {
                current_entry.supports_tools = (val == "true");
            } else if (line.find("supports_vision:") != std::string::npos) {
                current_entry.supports_vision = (val == "true");
            } else if (line.find("cost_tier:") != std::string::npos) {
                try { current_entry.cost_tier = std::stoi(val); } catch (...) {}
            }
        }

        if (section == Routing) {
            std::string val = extractValue(line);
            if (line.find("default_model:") != std::string::npos) {
                routing.default_model = val;
            } else if (line.find("cloud_model:") != std::string::npos) {
                routing.cloud_model = val;
            } else if (line.find("reasoning_model:") != std::string::npos) {
                routing.reasoning_model = val;
            } else if (line.find("embedding_model:") != std::string::npos) {
                routing.embedding_model = val;
            } else if (line.find("cloud_enabled:") != std::string::npos) {
                routing.cloud_enabled = (val == "true");
            } else if (line.find("complexity_threshold:") != std::string::npos) {
                try { routing.complexity_threshold = std::stof(val); } catch (...) {}
            } else if (line.find("fallback_to_local:") != std::string::npos) {
                routing.fallback_to_local = (val == "true");
            } else if (line.find("timeout_ms:") != std::string::npos) {
                try { routing.timeout_ms = std::stoi(val); } catch (...) {}
            } else if (line.find("deterministic_first:") != std::string::npos) {
                routing.deterministic_first = (val == "true");
            } else if (line.find("confidence_threshold:") != std::string::npos) {
                try { routing.confidence_threshold = std::stof(val); } catch (...) {}
            }
        }
    }

    flushEntry();
    registry.setRouting(routing);
    return registry.size() > 0;
}

}  // namespace sparx
