/**
 * @file sparx_tool_registry.cpp
 * @brief Implementation of the Agent OS Tool Registry (Syscall Table).
 *
 * Provides unified tool registration, invocation, and MCP compatibility.
 * Every agent tool call is routed through this registry.
 */

#include "sparx_tool_registry.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace sparx::os {

// ─── Helpers ────────────────────────────────────────────────────────────────

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ─── Constructor ────────────────────────────────────────────────────────────

ToolRegistry::ToolRegistry(ToolRegistryConfig config)
    : config_(std::move(config)) {}

// ─── Registration ───────────────────────────────────────────────────────────

bool ToolRegistry::registerTool(ToolDefinition def) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (tools_.size() >= config_.max_tools) return false;
    if (tools_.count(def.name)) return false;

    std::string name = def.name;
    if (def.required_permission.empty()) {
        def.required_permission = "tool:" + name;
    }

    tools_[name] = std::move(def);
    if (config_.track_stats) {
        stats_[name] = ToolStats{name};
    }
    return true;
}

bool ToolRegistry::unregisterTool(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.erase(name) > 0;
}

bool ToolRegistry::updateTool(const std::string& name, ToolDefinition def) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return false;
    it->second = std::move(def);
    return true;
}

// ─── Discovery ──────────────────────────────────────────────────────────────

std::vector<std::string> ToolRegistry::listTools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (auto& [name, _] : tools_) {
        names.push_back(name);
    }
    return names;
}

std::vector<std::string> ToolRegistry::listByCategory(
    const std::string& category) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    for (auto& [name, def] : tools_) {
        if (def.category == category) names.push_back(name);
    }
    return names;
}

std::optional<ToolDefinition> ToolRegistry::getTool(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return std::nullopt;
    return it->second;
}

bool ToolRegistry::hasTool(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tools_.count(name) > 0;
}

std::vector<ToolDefinition> ToolRegistry::toolsForAgent(
    uint64_t agent_id,
    const std::set<std::string>& agent_permissions) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ToolDefinition> available;
    for (auto& [name, def] : tools_) {
        // Check if agent has permission for this tool
        if (agent_permissions.count(def.required_permission) ||
            agent_permissions.count("tool:*")) {
            available.push_back(def);
        }
    }
    return available;
}

// ─── Invocation ─────────────────────────────────────────────────────────────

ToolResult ToolRegistry::invoke(const std::string& tool_name,
                                const std::map<std::string, std::string>& params,
                                uint64_t agent_id) {
    ToolHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tools_.find(tool_name);
        if (it == tools_.end()) {
            ToolResult r;
            r.success = false;
            r.error = "Tool not found: " + tool_name;
            return r;
        }
        handler = it->second.handler;
    }

    if (!handler) {
        ToolResult r;
        r.success = false;
        r.error = "Tool has no handler: " + tool_name;
        return r;
    }

    // Concurrency control
    uint32_t current = active_calls_.fetch_add(1);
    if (current >= config_.max_concurrent_calls) {
        active_calls_.fetch_sub(1);
        ToolResult r;
        r.success = false;
        r.error = "Max concurrent tool calls exceeded";
        return r;
    }

    // Execute
    auto start = std::chrono::steady_clock::now();
    ToolResult result;
    try {
        result = handler(params);
    } catch (const std::exception& e) {
        result.success = false;
        result.error = std::string("Exception: ") + e.what();
    }
    auto end = std::chrono::steady_clock::now();
    result.latency = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start);

    active_calls_.fetch_sub(1);

    // Record stats
    if (config_.track_stats) {
        recordInvocation(tool_name, result);
    }

    return result;
}

ToolRegistry::ValidationResult ToolRegistry::validateParams(
    const std::string& tool_name,
    const std::map<std::string, std::string>& params) const {
    std::lock_guard<std::mutex> lock(mutex_);

    ValidationResult vr;
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) {
        vr.valid = false;
        vr.errors.push_back("Tool not found: " + tool_name);
        return vr;
    }

    auto& def = it->second;
    for (auto& param : def.parameters) {
        if (param.required && params.find(param.name) == params.end()) {
            vr.valid = false;
            vr.errors.push_back("Missing required parameter: " + param.name);
        }
        // Check enum constraints
        if (!param.enum_values.empty()) {
            auto pit = params.find(param.name);
            if (pit != params.end()) {
                bool found = std::find(param.enum_values.begin(),
                                       param.enum_values.end(),
                                       pit->second) != param.enum_values.end();
                if (!found) {
                    vr.valid = false;
                    vr.errors.push_back("Invalid value for " + param.name +
                                        ": " + pit->second);
                }
            }
        }
    }
    return vr;
}

// ─── Dependency Resolution ──────────────────────────────────────────────────

bool ToolRegistry::dependenciesMet(const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tools_.find(tool_name);
    if (it == tools_.end()) return false;

    for (auto& dep : it->second.dependencies) {
        if (tools_.find(dep) == tools_.end()) return false;
    }
    return true;
}

std::vector<std::string> ToolRegistry::missingDependencies(
    const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> missing;
    auto it = tools_.find(tool_name);
    if (it == tools_.end()) return {tool_name};

    for (auto& dep : it->second.dependencies) {
        if (tools_.find(dep) == tools_.end()) {
            missing.push_back(dep);
        }
    }
    return missing;
}

std::vector<std::string> ToolRegistry::resolutionOrder() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Kahn's algorithm for topological sort
    std::map<std::string, int> in_degree;
    std::map<std::string, std::vector<std::string>> dependents;

    for (auto& [name, _] : tools_) {
        in_degree[name] = 0;
    }
    for (auto& [name, def] : tools_) {
        for (auto& dep : def.dependencies) {
            if (tools_.count(dep)) {
                dependents[dep].push_back(name);
                in_degree[name]++;
            }
        }
    }

    std::vector<std::string> order;
    std::vector<std::string> queue;
    for (auto& [name, deg] : in_degree) {
        if (deg == 0) queue.push_back(name);
    }

    while (!queue.empty()) {
        std::string current = queue.back();
        queue.pop_back();
        order.push_back(current);

        for (auto& dep : dependents[current]) {
            if (--in_degree[dep] == 0) {
                queue.push_back(dep);
            }
        }
    }
    return order;
}

// ─── MCP Integration ────────────────────────────────────────────────────────

std::string ToolRegistry::toMcpToolList() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Generate MCP-compatible tool list (JSON format)
    std::ostringstream oss;
    oss << "{\"tools\":[";
    bool first = true;
    for (auto& [name, def] : tools_) {
        if (!first) oss << ",";
        first = false;

        oss << "{\"name\":\"" << name << "\","
            << "\"description\":\"" << def.description << "\","
            << "\"inputSchema\":{\"type\":\"object\",\"properties\":{";

        bool first_param = true;
        std::vector<std::string> required_params;
        for (auto& param : def.parameters) {
            if (!first_param) oss << ",";
            first_param = false;

            oss << "\"" << param.name << "\":{";
            switch (param.type) {
                case ParamType::String:  oss << "\"type\":\"string\""; break;
                case ParamType::Integer: oss << "\"type\":\"integer\""; break;
                case ParamType::Float:   oss << "\"type\":\"number\""; break;
                case ParamType::Boolean: oss << "\"type\":\"boolean\""; break;
                case ParamType::Array:   oss << "\"type\":\"array\""; break;
                case ParamType::Object:  oss << "\"type\":\"object\""; break;
            }
            if (!param.description.empty()) {
                oss << ",\"description\":\"" << param.description << "\"";
            }
            oss << "}";
            if (param.required) required_params.push_back(param.name);
        }

        oss << "},\"required\":[";
        for (size_t i = 0; i < required_params.size(); ++i) {
            if (i > 0) oss << ",";
            oss << "\"" << required_params[i] << "\"";
        }
        oss << "]}}";
    }
    oss << "]}";
    return oss.str();
}

uint32_t ToolRegistry::importFromMcp(const std::string& mcp_json,
                                     const std::string& server_name) {
    // Simplified MCP import: in production, parse full JSON Schema.
    // This stub shows the interface contract.
    (void)mcp_json;
    (void)server_name;
    // TODO: Parse mcp_json and register each tool with mcp_server = server_name
    return 0;
}

// ─── Stats ──────────────────────────────────────────────────────────────────

std::optional<ToolStats> ToolRegistry::getStats(
    const std::string& tool_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(tool_name);
    if (it == stats_.end()) return std::nullopt;
    return it->second;
}

std::vector<ToolStats> ToolRegistry::allStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ToolStats> result;
    for (auto& [_, s] : stats_) {
        result.push_back(s);
    }
    return result;
}

void ToolRegistry::resetStats(const std::string& tool_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(tool_name);
    if (it != stats_.end()) {
        it->second = ToolStats{tool_name};
    }
}

ToolRegistry::GlobalStats ToolRegistry::globalStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GlobalStats gs;
    gs.total_tools = static_cast<uint32_t>(tools_.size());
    gs.active_invocations = active_calls_.load();
    for (auto& [_, s] : stats_) {
        gs.total_invocations += s.invocations;
        gs.total_failures += s.failures;
    }
    return gs;
}

// ─── Internal ───────────────────────────────────────────────────────────────

void ToolRegistry::recordInvocation(const std::string& name,
                                    const ToolResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stats_.find(name);
    if (it == stats_.end()) return;

    auto& s = it->second;
    s.invocations++;
    if (result.success) {
        s.successes++;
    } else {
        s.failures++;
        if (!result.error.empty()) {
            // Categorize error
            std::string error_type = result.error.substr(0, 30);
            s.error_counts[error_type]++;
        }
    }

    // Update latency (exponential moving average)
    float ms = static_cast<float>(result.latency.count());
    if (s.avg_latency_ms == 0.0f) {
        s.avg_latency_ms = ms;
    } else {
        s.avg_latency_ms = s.avg_latency_ms * 0.9f + ms * 0.1f;
    }
    s.p99_latency_ms = std::max(s.p99_latency_ms, ms);
    s.last_invoked_ms = nowMs();
}

}  // namespace sparx::os
