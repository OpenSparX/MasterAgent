#pragma once
/**
 * @file sparx_tool_registry.h
 * @brief Tool Registry / Syscall Manager — unified tool management for Agent OS.
 *
 * Research basis:
 *   - MCP (Model Context Protocol) — Anthropic, 2024
 *   - AIOS Tool Manager (arXiv:2403.16971)
 *   - OpenAPI/JSON Schema tool description standard
 *   - Unix syscall table design (numbered, versioned, stable ABI)
 *
 * In a traditional OS, syscalls provide the interface between user-space
 * and kernel. In Agent OS, the Tool Registry provides the interface between
 * agents and external capabilities (APIs, file system, other agents).
 *
 * This module provides:
 *   1. Dynamic registration: tools can be added/removed at runtime
 *   2. Versioned API: tools declare their interface version for compatibility
 *   3. Dependency resolution: tools can declare prerequisites
 *   4. Permission integration: tool calls go through Access Control
 *   5. MCP compatibility: tools can be described in MCP format
 *   6. Execution tracking: latency, success rate, error patterns per tool
 */

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace sparx::os {

// ─── Tool Definition ────────────────────────────────────────────────────────

/// Parameter type for tool arguments.
enum class ParamType : uint8_t {
    String,
    Integer,
    Float,
    Boolean,
    Array,
    Object,
};

/// A single tool parameter definition.
struct ToolParam {
    std::string name;
    std::string description;
    ParamType type = ParamType::String;
    bool required = false;
    std::string default_value;       // JSON-encoded default
    std::vector<std::string> enum_values;  // Valid values (if constrained)
};

/// Tool execution result.
struct ToolResult {
    bool success = false;
    std::string output;              // Result content (text/JSON)
    std::string error;               // Error message (if failed)
    std::chrono::milliseconds latency{0};
    uint32_t tokens_consumed = 0;    // If tool internally uses LLM
};

/// Tool handler function signature.
using ToolHandler = std::function<ToolResult(
    const std::map<std::string, std::string>& params)>;

/// Complete tool definition.
struct ToolDefinition {
    /// Unique tool name (namespaced: "service.tool_name").
    std::string name;
    /// Human-readable description.
    std::string description;
    /// Semantic version (semver: "1.2.3").
    std::string version;
    /// Category for organization.
    std::string category;

    // ── Interface ──
    std::vector<ToolParam> parameters;
    std::string return_description;

    // ── Metadata ──
    /// Is this tool destructive (writes/deletes external state)?
    bool is_destructive = false;
    /// Is this tool idempotent (safe to retry)?
    bool is_idempotent = false;
    /// Does this tool require user confirmation?
    bool requires_confirmation = false;
    /// Required permission to invoke (e.g., "tool:web_search").
    std::string required_permission;
    /// Tool dependencies (must be available before this tool works).
    std::vector<std::string> dependencies;
    /// Estimated latency class.
    enum class Latency : uint8_t { Fast, Medium, Slow } expected_latency = Latency::Fast;

    // ── MCP Compatibility ──
    /// MCP server name (if this tool comes from an MCP server).
    std::string mcp_server;
    /// MCP tool schema (JSON Schema string for MCP protocol).
    std::string mcp_schema;

    // ── Handler ──
    ToolHandler handler;
};

// ─── Tool Execution Stats ───────────────────────────────────────────────────

struct ToolStats {
    std::string tool_name;
    uint64_t invocations = 0;
    uint64_t successes = 0;
    uint64_t failures = 0;
    float avg_latency_ms = 0.0f;
    float p99_latency_ms = 0.0f;
    int64_t last_invoked_ms = 0;
    std::map<std::string, uint32_t> error_counts;  // error_type → count
};

// ─── Tool Registry Configuration ────────────────────────────────────────────

struct ToolRegistryConfig {
    /// Maximum registered tools.
    uint32_t max_tools = 256;
    /// Enable MCP server auto-discovery.
    bool enable_mcp_discovery = false;
    /// MCP server connection timeout (ms).
    int32_t mcp_timeout_ms = 5000;
    /// Track execution stats.
    bool track_stats = true;
    /// Maximum concurrent tool executions.
    uint32_t max_concurrent_calls = 8;
};

// ─── Tool Registry ──────────────────────────────────────────────────────────

/**
 * @brief Unified tool management — the syscall table of Agent OS.
 *
 * Provides a single registry for all tools available to agents, regardless
 * of their backend (local function, MCP server, REST API, other agent).
 *
 * Lifecycle:
 *   1. registerTool(def) — make a tool available
 *   2. invoke(name, params, agent_id) — call a tool (goes through ACL)
 *   3. unregister(name) — remove a tool
 *
 * Thread-safe. Supports concurrent tool invocations up to max_concurrent.
 */
class ToolRegistry {
public:
    explicit ToolRegistry(ToolRegistryConfig config = {});

    // ── Registration ──

    /// Register a new tool. Returns false if name already exists.
    bool registerTool(ToolDefinition def);

    /// Unregister a tool by name.
    bool unregisterTool(const std::string& name);

    /// Update an existing tool's definition (version bump).
    bool updateTool(const std::string& name, ToolDefinition def);

    // ── Discovery ──

    /// Get all registered tool names.
    std::vector<std::string> listTools() const;

    /// Get tools by category.
    std::vector<std::string> listByCategory(const std::string& category) const;

    /// Get a tool's definition.
    std::optional<ToolDefinition> getTool(const std::string& name) const;

    /// Check if a tool is registered.
    bool hasTool(const std::string& name) const;

    /// Get tools available to a specific agent (filtered by permissions).
    std::vector<ToolDefinition> toolsForAgent(
        uint64_t agent_id,
        const std::set<std::string>& agent_permissions) const;

    // ── Invocation ──

    /// Invoke a tool. Checks permissions, tracks stats, handles errors.
    ToolResult invoke(const std::string& tool_name,
                      const std::map<std::string, std::string>& params,
                      uint64_t agent_id = 0);

    /// Validate parameters against the tool's schema (before invoking).
    struct ValidationResult {
        bool valid = true;
        std::vector<std::string> errors;
    };
    ValidationResult validateParams(
        const std::string& tool_name,
        const std::map<std::string, std::string>& params) const;

    // ── Dependency Resolution ──

    /// Check if all dependencies of a tool are satisfied.
    bool dependenciesMet(const std::string& tool_name) const;

    /// Get unsatisfied dependencies for a tool.
    std::vector<std::string> missingDependencies(
        const std::string& tool_name) const;

    /// Topological sort of tools by dependency (for ordered initialization).
    std::vector<std::string> resolutionOrder() const;

    // ── MCP Integration ──

    /// Generate MCP-compatible tool list (for LLM function calling).
    std::string toMcpToolList() const;

    /// Import tools from MCP server description (JSON).
    uint32_t importFromMcp(const std::string& mcp_json,
                           const std::string& server_name);

    // ── Stats ──

    /// Get execution stats for a tool.
    std::optional<ToolStats> getStats(const std::string& tool_name) const;

    /// Get all stats.
    std::vector<ToolStats> allStats() const;

    /// Reset stats for a tool.
    void resetStats(const std::string& tool_name);

    // ── Global Stats ──

    struct GlobalStats {
        uint32_t total_tools = 0;
        uint64_t total_invocations = 0;
        uint64_t total_failures = 0;
        uint32_t active_invocations = 0;
    };
    GlobalStats globalStats() const;

private:
    ToolRegistryConfig config_;
    mutable std::mutex mutex_;

    // Tool storage
    std::map<std::string, ToolDefinition> tools_;
    std::map<std::string, ToolStats> stats_;

    // Concurrency control
    std::atomic<uint32_t> active_calls_{0};

    // ── Internal ──

    /// Record invocation result in stats.
    void recordInvocation(const std::string& name, const ToolResult& result);
};

}  // namespace sparx::os
