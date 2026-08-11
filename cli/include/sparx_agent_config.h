#pragma once
/**
 * @file sparx_agent_config.h
 * @brief Loads and represents agent.yaml configuration.
 */

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace sparx {

struct AgentConfig {
    std::string name;
    std::string version;
    std::string model_id;
    /// Filesystem path to a GGUF weight file. Empty means no local model is
    /// configured, which is what makes `sparx run` fall back to simulation:
    /// there is no default path to guess, because guessing wrong would look
    /// like a broken model rather than a missing one.
    std::string model_path;
    /// host:port of a llama-server to attach to. A server already listening
    /// here is used as-is instead of spawning a child.
    std::string endpoint = "127.0.0.1:8080";
    int context_length = 4096;
    int max_output_tokens = 512;
    std::vector<std::string> skills;
    bool deterministic_first = true;
    double confidence_threshold = 0.85;
    std::string reliability = "D2";
    std::string runtime = "auto";
};

/// Minimal YAML parser — handles the flat agent.yaml format without pulling
/// in a full YAML library. Production would use rapidyaml or yaml-cpp.
inline bool loadAgentConfig(const std::string& path, AgentConfig& config) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    bool in_skills = false;
    bool in_model = false;
    bool in_routing = false;

    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // Detect section context
        if (line.find("skills:") == 0) { in_skills = true; in_model = false; in_routing = false; continue; }
        if (line.find("model:") == 0) { in_model = true; in_skills = false; in_routing = false; continue; }
        if (line.find("routing:") == 0) { in_routing = true; in_skills = false; in_model = false; continue; }

        // Top-level scalar fields
        if (line.find("name:") == 0 && !in_model && !in_routing) {
            config.name = line.substr(line.find(':') + 2);
            in_skills = false; in_model = false; in_routing = false;
            continue;
        }
        if (line.find("version:") == 0 && !in_model) {
            auto val = line.substr(line.find(':') + 2);
            // Remove quotes
            if (val.size() >= 2 && val.front() == '"') val = val.substr(1, val.size() - 2);
            config.version = val;
            in_skills = false; in_model = false; in_routing = false;
            continue;
        }
        if (line.find("reliability:") == 0) {
            config.reliability = line.substr(line.find(':') + 2);
            in_skills = false; in_model = false; in_routing = false;
            continue;
        }
        if (line.find("runtime:") == 0 && !in_model && !in_routing) {
            config.runtime = line.substr(line.find(':') + 2);
            in_skills = false; in_model = false; in_routing = false;
            continue;
        }

        // Skill list items
        if (in_skills && line.find("  - ") == 0) {
            config.skills.push_back(line.substr(4));
            continue;
        }

        // Model sub-fields
        if (in_model) {
            if (line.find("  id:") != std::string::npos) {
                config.model_id = line.substr(line.find(':') + 2);
            } else if (line.find("  path:") != std::string::npos) {
                auto val = line.substr(line.find(':') + 2);
                if (val.size() >= 2 && val.front() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                config.model_path = val;
            } else if (line.find("  endpoint:") != std::string::npos) {
                auto val = line.substr(line.find(':') + 2);
                if (val.size() >= 2 && val.front() == '"') {
                    val = val.substr(1, val.size() - 2);
                }
                config.endpoint = val;
            } else if (line.find("  context_length:") != std::string::npos) {
                config.context_length = std::stoi(line.substr(line.find(':') + 2));
            } else if (line.find("  max_output_tokens:") != std::string::npos) {
                config.max_output_tokens = std::stoi(line.substr(line.find(':') + 2));
            }
            continue;
        }

        // Routing sub-fields
        if (in_routing) {
            if (line.find("  deterministic_first:") != std::string::npos) {
                config.deterministic_first = line.find("true") != std::string::npos;
            } else if (line.find("  confidence_threshold:") != std::string::npos) {
                config.confidence_threshold = std::stod(line.substr(line.find(':') + 2));
            }
            continue;
        }
    }
    return !config.name.empty();
}

/// Check if a line matches a deterministic skill's trigger patterns.
/// In the full implementation this loads skills/*.yaml; here it matches
/// the built-in hello skill by checking known patterns.
inline bool matchesDeterministicSkill(const std::string& skill,
                                       const std::string& input) {
    if (skill == "hello") {
        return input == "hello" || input == "hi" ||
               input.find("\xe4\xbd\xa0\xe5\xa5\xbd") != std::string::npos;
    }
    return false;
}

/// Execute a deterministic skill and print output.
inline void executeDeterministicSkill(const std::string& skill,
                                      const std::string& /*input*/) {
    if (skill == "hello") {
        std::cout << "  ✓ \"\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x81"
                     "\xe6\x88\x91\xe6\x98\xaf\xe4\xbd\xa0\xe7\x9a\x84"
                     "\xe7\xab\xaf\xe4\xbe\xa7 Agent\xef\xbc\x8c"
                     "\xe6\xad\xa3\xe5\x9c\xa8\xe6\x9c\xac\xe5\x9c\xb0"
                     "\xe8\xbf\x90\xe8\xa1\x8c\xe3\x80\x82"
                     "\xe6\x9c\x89\xe4\xbb\x80\xe4\xb9\x88\xe5\x8f\xaf\xe4\xbb\xa5"
                     "\xe5\xb8\xae\xe4\xbd\xa0\xe7\x9a\x84\xef\xbc\x9f\"\n";
    }
}

}  // namespace sparx
