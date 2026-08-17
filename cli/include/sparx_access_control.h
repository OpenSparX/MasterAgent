#pragma once
/**
 * @file sparx_access_control.h
 * @brief Access Control — Capability-based security for Agent OS.
 *
 * Research basis:
 *   - AIOS Access Control (arXiv:2403.16971)
 *   - Capability-Based Security (Dennis & Van Horn, 1966)
 *   - "Principle of Least Privilege" (Saltzer & Schroeder, 1975)
 *   - Android Permission Model (per-app sandboxing)
 *   - SENTINEL: Multi-Level Safety Framework (arXiv:2510.12985)
 *
 * Security model: Capability-based access control.
 * Each agent holds a set of capability tokens that grant specific permissions.
 * No ambient authority — an agent with no tokens can do nothing.
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │  Permission Classes                                          │
 *   ├──────────────────────────────────────────────────────────────┤
 *   │  tool:<name>         — invoke a specific tool                │
 *   │  memory:read:<scope> — read memory (own / shared / all)      │
 *   │  memory:write:<scope>— write memory                          │
 *   │  network:<domain>    — make HTTP requests to domain          │
 *   │  fs:read:<path>      — read filesystem path                  │
 *   │  fs:write:<path>     — write filesystem path                 │
 *   │  agent:spawn         — spawn child agents                    │
 *   │  agent:kill          — kill other agents                     │
 *   │  cloud:invoke        — use cloud inference                   │
 *   │  resource:tokens:<N> — token budget of N                     │
 *   └──────────────────────────────────────────────────────────────┘
 */

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sparx::os {

// ─── Capability Token ───────────────────────────────────────────────────────

/// A capability token granting a specific permission.
struct Capability {
    std::string permission;          // e.g., "tool:web_search", "fs:read:/tmp"
    std::string granted_by;          // Who granted this (user, system, parent agent)
    int64_t granted_at_ms = 0;       // When it was granted
    int64_t expires_at_ms = 0;       // 0 = never expires
    bool revocable = true;           // Can be revoked at runtime

    /// Check if this capability is still valid.
    bool isValid() const {
        if (expires_at_ms == 0) return true;
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return now < expires_at_ms;
    }

    /// Check if this capability matches a requested permission.
    /// Supports wildcards: "tool:*" matches "tool:web_search".
    bool matches(const std::string& requested) const;
};

/// An agent's complete capability set.
struct CapabilitySet {
    uint64_t agent_id = 0;
    std::string agent_name;
    std::vector<Capability> capabilities;

    /// Check if this set grants a specific permission.
    bool hasPermission(const std::string& permission) const;

    /// Get all permissions as strings.
    std::vector<std::string> permissions() const;
};

// ─── Audit Log ──────────────────────────────────────────────────────────────

/// An entry in the security audit log.
struct AuditEntry {
    enum class Action : uint8_t {
        PermissionGranted,
        PermissionRevoked,
        AccessAllowed,
        AccessDenied,
        AgentSpawned,
        AgentKilled,
        ToolInvoked,
        ResourceExhausted,
        PolicyViolation,
    };
    Action action;
    uint64_t agent_id = 0;
    std::string agent_name;
    std::string permission;          // Which permission was involved
    std::string resource;            // What resource was accessed
    std::string detail;              // Additional context
    int64_t timestamp_ms = 0;
    bool allowed = true;             // Was the action permitted?
};

// ─── Resource Quota ─────────────────────────────────────────────────────────

/// Resource limits per agent.
struct ResourceQuota {
    uint64_t max_tokens = 0;         // 0 = unlimited
    uint32_t max_api_calls = 0;      // 0 = unlimited
    uint32_t max_tool_calls = 0;     // 0 = unlimited
    uint32_t max_child_agents = 0;   // 0 = unlimited
    uint64_t max_memory_bytes = 0;   // 0 = unlimited
    int64_t max_runtime_ms = 0;      // 0 = unlimited
};

/// Current resource usage.
struct ResourceUsage {
    uint64_t tokens_used = 0;
    uint32_t api_calls_used = 0;
    uint32_t tool_calls_used = 0;
    uint32_t child_agents_spawned = 0;
    uint64_t memory_bytes_used = 0;
    int64_t runtime_ms = 0;

    bool withinQuota(const ResourceQuota& quota) const;
};

// ─── Security Policy ────────────────────────────────────────────────────────

/// A security policy rule.
struct PolicyRule {
    std::string name;
    std::string description;
    /// Pattern matching: which agents this applies to (name glob or "*").
    std::string agent_pattern;
    /// Default permissions granted to matching agents.
    std::vector<std::string> default_permissions;
    /// Explicitly denied permissions (override grants).
    std::vector<std::string> denied_permissions;
    /// Resource quota.
    ResourceQuota quota;
    /// Whether tool calls require user confirmation.
    bool require_user_confirm = false;
    /// List of destructive tools that always need confirmation.
    std::vector<std::string> confirm_tools;
};

// ─── Access Control Configuration ───────────────────────────────────────────

struct AccessControlConfig {
    /// Enable access control (false = all permissions granted).
    bool enabled = true;
    /// Maximum audit log entries before rotation.
    uint32_t max_audit_entries = 10000;
    /// Default policy for agents without explicit rules.
    PolicyRule default_policy;
    /// Path for audit log persistence.
    std::string audit_log_path;  // defaults to ~/.sparx/audit.log
};

// ─── Access Control Manager ─────────────────────────────────────────────────

/**
 * @brief Capability-based access control for Agent OS.
 *
 * Every agent operation (tool call, memory access, network request, file I/O)
 * passes through this gatekeeper. The manager checks the agent's capability
 * set against the requested permission and logs the decision.
 *
 * Key principles:
 *   1. Deny by default: no capability = no access
 *   2. Least privilege: agents get only what they need
 *   3. Audit everything: every access decision is logged
 *   4. Revocable: capabilities can be withdrawn at runtime
 *   5. Time-bounded: capabilities can expire
 *
 * Thread-safe.
 */
class AccessControl {
public:
    explicit AccessControl(AccessControlConfig config = {});

    // ── Agent Registration ──

    /// Register an agent with the access control system.
    /// Applies default policy and returns its capability set.
    CapabilitySet registerAgent(uint64_t agent_id,
                                const std::string& agent_name,
                                const std::string& agent_type = "generic");

    /// Unregister an agent (cleanup on termination).
    void unregisterAgent(uint64_t agent_id);

    // ── Permission Management ──

    /// Grant a permission to an agent.
    bool grant(uint64_t agent_id, const std::string& permission,
               const std::string& granted_by = "system",
               int64_t ttl_ms = 0);

    /// Revoke a permission from an agent.
    bool revoke(uint64_t agent_id, const std::string& permission);

    /// Revoke all permissions from an agent.
    void revokeAll(uint64_t agent_id);

    /// Check if an agent has a specific permission.
    bool check(uint64_t agent_id, const std::string& permission) const;

    /// Check and log (returns false + logs denial if not permitted).
    bool checkAndLog(uint64_t agent_id, const std::string& permission,
                     const std::string& resource = "");

    /// Get an agent's full capability set.
    std::optional<CapabilitySet> getCapabilities(uint64_t agent_id) const;

    // ── Resource Quotas ──

    /// Set resource quota for an agent.
    void setQuota(uint64_t agent_id, ResourceQuota quota);

    /// Record resource usage (returns false if over quota).
    bool recordUsage(uint64_t agent_id, const std::string& resource_type,
                     uint64_t amount = 1);

    /// Get current usage for an agent.
    ResourceUsage getUsage(uint64_t agent_id) const;

    /// Check if agent is within quota.
    bool withinQuota(uint64_t agent_id) const;

    // ── Policy Management ──

    /// Add a security policy rule.
    void addPolicy(PolicyRule rule);

    /// Get all policies.
    std::vector<PolicyRule> policies() const;

    // ── Audit Log ──

    /// Get recent audit entries.
    std::vector<AuditEntry> auditLog(uint32_t count = 100) const;

    /// Get audit entries for a specific agent.
    std::vector<AuditEntry> auditLogForAgent(uint64_t agent_id,
                                              uint32_t count = 50) const;

    /// Get denied access attempts.
    std::vector<AuditEntry> deniedAttempts(uint32_t count = 50) const;

    // ── Stats ──

    struct Stats {
        uint64_t total_checks = 0;
        uint64_t total_grants = 0;
        uint64_t total_denials = 0;
        uint64_t total_revocations = 0;
        uint32_t active_agents = 0;
        uint32_t audit_entries = 0;
    };
    Stats stats() const;

private:
    AccessControlConfig config_;
    mutable std::mutex mutex_;

    // Per-agent state
    std::map<uint64_t, CapabilitySet> agent_caps_;
    std::map<uint64_t, ResourceQuota> agent_quotas_;
    std::map<uint64_t, ResourceUsage> agent_usage_;

    // Policy rules
    std::vector<PolicyRule> policies_;

    // Audit log (ring buffer)
    std::deque<AuditEntry> audit_log_;

    mutable Stats stats_;

    // ── Internal ──

    /// Find matching policy for an agent.
    const PolicyRule* findPolicy(const std::string& agent_name,
                                 const std::string& agent_type) const;

    /// Log an audit entry.
    void audit(AuditEntry entry);

    /// Check if a permission is explicitly denied by policy.
    bool isDenied(uint64_t agent_id, const std::string& permission) const;
};

}  // namespace sparx::os
