/**
 * @file sparx_access_control.cpp
 * @brief Implementation of the Agent OS Access Control system.
 *
 * Capability-based security: agents hold permission tokens, deny by default.
 * Every access decision is audited.
 */

#include "sparx_access_control.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <fnmatch.h>

namespace sparx::os {

// ─── Helpers ────────────────────────────────────────────────────────────────

static int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count();
}

// ─── Capability Implementation ──────────────────────────────────────────────

bool Capability::matches(const std::string& requested) const {
    if (permission == requested) return true;

    // Wildcard matching: "tool:*" matches "tool:web_search"
    // Split on ':'
    size_t perm_colon = permission.find(':');
    size_t req_colon = requested.find(':');

    if (perm_colon == std::string::npos || req_colon == std::string::npos) {
        return false;
    }

    std::string perm_class = permission.substr(0, perm_colon);
    std::string req_class = requested.substr(0, req_colon);
    if (perm_class != req_class) return false;

    std::string perm_resource = permission.substr(perm_colon + 1);
    std::string req_resource = requested.substr(req_colon + 1);

    if (perm_resource == "*") return true;

    // Path prefix matching for fs: permissions
    if (perm_class == "fs") {
        // "fs:read:/tmp" matches "fs:read:/tmp/file.txt"
        if (req_resource.find(perm_resource) == 0) return true;
    }

    return fnmatch(perm_resource.c_str(), req_resource.c_str(), 0) == 0;
}

bool CapabilitySet::hasPermission(const std::string& permission) const {
    return std::any_of(capabilities.begin(), capabilities.end(),
        [&permission](const Capability& cap) {
            return cap.isValid() && cap.matches(permission);
        });
}

std::vector<std::string> CapabilitySet::permissions() const {
    std::vector<std::string> result;
    for (auto& cap : capabilities) {
        if (cap.isValid()) result.push_back(cap.permission);
    }
    return result;
}

bool ResourceUsage::withinQuota(const ResourceQuota& quota) const {
    if (quota.max_tokens > 0 && tokens_used >= quota.max_tokens) return false;
    if (quota.max_api_calls > 0 && api_calls_used >= quota.max_api_calls) return false;
    if (quota.max_tool_calls > 0 && tool_calls_used >= quota.max_tool_calls) return false;
    if (quota.max_child_agents > 0 && child_agents_spawned >= quota.max_child_agents) return false;
    if (quota.max_memory_bytes > 0 && memory_bytes_used >= quota.max_memory_bytes) return false;
    if (quota.max_runtime_ms > 0 && runtime_ms >= quota.max_runtime_ms) return false;
    return true;
}

// ─── Constructor ────────────────────────────────────────────────────────────

AccessControl::AccessControl(AccessControlConfig config)
    : config_(std::move(config)) {
    if (config_.audit_log_path.empty()) {
        config_.audit_log_path = ".sparx/audit.log";
    }
}

// ─── Agent Registration ─────────────────────────────────────────────────────

CapabilitySet AccessControl::registerAgent(uint64_t agent_id,
                                           const std::string& agent_name,
                                           const std::string& agent_type) {
    std::lock_guard<std::mutex> lock(mutex_);

    CapabilitySet caps;
    caps.agent_id = agent_id;
    caps.agent_name = agent_name;

    // Apply matching policy
    const PolicyRule* policy = findPolicy(agent_name, agent_type);
    if (policy) {
        for (auto& perm : policy->default_permissions) {
            Capability cap;
            cap.permission = perm;
            cap.granted_by = "policy:" + policy->name;
            cap.granted_at_ms = nowMs();
            caps.capabilities.push_back(std::move(cap));
        }
        agent_quotas_[agent_id] = policy->quota;
    } else if (!config_.default_policy.default_permissions.empty()) {
        for (auto& perm : config_.default_policy.default_permissions) {
            Capability cap;
            cap.permission = perm;
            cap.granted_by = "default_policy";
            cap.granted_at_ms = nowMs();
            caps.capabilities.push_back(std::move(cap));
        }
        agent_quotas_[agent_id] = config_.default_policy.quota;
    }

    agent_caps_[agent_id] = caps;
    agent_usage_[agent_id] = {};
    stats_.active_agents++;

    AuditEntry entry;
    entry.action = AuditEntry::Action::AgentSpawned;
    entry.agent_id = agent_id;
    entry.agent_name = agent_name;
    entry.detail = "type=" + agent_type;
    entry.timestamp_ms = nowMs();
    entry.allowed = true;
    audit(std::move(entry));

    return caps;
}

void AccessControl::unregisterAgent(uint64_t agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = agent_caps_.find(agent_id);
    if (it != agent_caps_.end()) {
        AuditEntry entry;
        entry.action = AuditEntry::Action::AgentKilled;
        entry.agent_id = agent_id;
        entry.agent_name = it->second.agent_name;
        entry.timestamp_ms = nowMs();
        entry.allowed = true;
        audit(std::move(entry));

        agent_caps_.erase(it);
        agent_quotas_.erase(agent_id);
        agent_usage_.erase(agent_id);
        stats_.active_agents--;
    }
}

// ─── Permission Management ──────────────────────────────────────────────────

bool AccessControl::grant(uint64_t agent_id, const std::string& permission,
                          const std::string& granted_by, int64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = agent_caps_.find(agent_id);
    if (it == agent_caps_.end()) return false;

    Capability cap;
    cap.permission = permission;
    cap.granted_by = granted_by;
    cap.granted_at_ms = nowMs();
    cap.expires_at_ms = ttl_ms > 0 ? (nowMs() + ttl_ms) : 0;
    it->second.capabilities.push_back(std::move(cap));

    stats_.total_grants++;

    AuditEntry entry;
    entry.action = AuditEntry::Action::PermissionGranted;
    entry.agent_id = agent_id;
    entry.agent_name = it->second.agent_name;
    entry.permission = permission;
    entry.detail = "by=" + granted_by;
    entry.timestamp_ms = nowMs();
    entry.allowed = true;
    audit(std::move(entry));

    return true;
}

bool AccessControl::revoke(uint64_t agent_id, const std::string& permission) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = agent_caps_.find(agent_id);
    if (it == agent_caps_.end()) return false;

    auto& caps = it->second.capabilities;
    auto cit = std::remove_if(caps.begin(), caps.end(),
        [&permission](const Capability& c) {
            return c.permission == permission && c.revocable;
        });
    if (cit == caps.end()) return false;

    caps.erase(cit, caps.end());
    stats_.total_revocations++;

    AuditEntry entry;
    entry.action = AuditEntry::Action::PermissionRevoked;
    entry.agent_id = agent_id;
    entry.agent_name = it->second.agent_name;
    entry.permission = permission;
    entry.timestamp_ms = nowMs();
    entry.allowed = true;
    audit(std::move(entry));

    return true;
}

void AccessControl::revokeAll(uint64_t agent_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = agent_caps_.find(agent_id);
    if (it == agent_caps_.end()) return;

    auto& caps = it->second.capabilities;
    caps.erase(
        std::remove_if(caps.begin(), caps.end(),
            [](const Capability& c) { return c.revocable; }),
        caps.end());
}

bool AccessControl::check(uint64_t agent_id, const std::string& permission) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!config_.enabled) return true;

    auto it = agent_caps_.find(agent_id);
    if (it == agent_caps_.end()) return false;

    // Check explicit denial first
    if (isDenied(agent_id, permission)) return false;

    return it->second.hasPermission(permission);
}

bool AccessControl::checkAndLog(uint64_t agent_id,
                                const std::string& permission,
                                const std::string& resource) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.total_checks++;

    if (!config_.enabled) return true;

    auto it = agent_caps_.find(agent_id);
    bool allowed = false;

    if (it != agent_caps_.end() && !isDenied(agent_id, permission)) {
        allowed = it->second.hasPermission(permission);
    }

    AuditEntry entry;
    entry.action = allowed ? AuditEntry::Action::AccessAllowed
                           : AuditEntry::Action::AccessDenied;
    entry.agent_id = agent_id;
    entry.agent_name = it != agent_caps_.end() ? it->second.agent_name : "?";
    entry.permission = permission;
    entry.resource = resource;
    entry.timestamp_ms = nowMs();
    entry.allowed = allowed;
    audit(std::move(entry));

    if (!allowed) stats_.total_denials++;
    return allowed;
}

std::optional<CapabilitySet> AccessControl::getCapabilities(
    uint64_t agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agent_caps_.find(agent_id);
    if (it == agent_caps_.end()) return std::nullopt;
    return it->second;
}

// ─── Resource Quotas ────────────────────────────────────────────────────────

void AccessControl::setQuota(uint64_t agent_id, ResourceQuota quota) {
    std::lock_guard<std::mutex> lock(mutex_);
    agent_quotas_[agent_id] = quota;
}

bool AccessControl::recordUsage(uint64_t agent_id,
                                const std::string& resource_type,
                                uint64_t amount) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& usage = agent_usage_[agent_id];
    if (resource_type == "tokens") {
        usage.tokens_used += amount;
    } else if (resource_type == "api_calls") {
        usage.api_calls_used += static_cast<uint32_t>(amount);
    } else if (resource_type == "tool_calls") {
        usage.tool_calls_used += static_cast<uint32_t>(amount);
    } else if (resource_type == "child_agents") {
        usage.child_agents_spawned += static_cast<uint32_t>(amount);
    } else if (resource_type == "memory_bytes") {
        usage.memory_bytes_used += amount;
    }

    auto qit = agent_quotas_.find(agent_id);
    if (qit == agent_quotas_.end()) return true;

    if (!usage.withinQuota(qit->second)) {
        AuditEntry entry;
        entry.action = AuditEntry::Action::ResourceExhausted;
        entry.agent_id = agent_id;
        entry.permission = "resource:" + resource_type;
        entry.timestamp_ms = nowMs();
        entry.allowed = false;
        audit(std::move(entry));
        return false;
    }
    return true;
}

ResourceUsage AccessControl::getUsage(uint64_t agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = agent_usage_.find(agent_id);
    return it != agent_usage_.end() ? it->second : ResourceUsage{};
}

bool AccessControl::withinQuota(uint64_t agent_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto uit = agent_usage_.find(agent_id);
    auto qit = agent_quotas_.find(agent_id);
    if (uit == agent_usage_.end() || qit == agent_quotas_.end()) return true;
    return uit->second.withinQuota(qit->second);
}

// ─── Policy Management ──────────────────────────────────────────────────────

void AccessControl::addPolicy(PolicyRule rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    policies_.push_back(std::move(rule));
}

std::vector<PolicyRule> AccessControl::policies() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return policies_;
}

// ─── Audit Log ──────────────────────────────────────────────────────────────

std::vector<AuditEntry> AccessControl::auditLog(uint32_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t n = std::min(count, static_cast<uint32_t>(audit_log_.size()));
    return {audit_log_.end() - n, audit_log_.end()};
}

std::vector<AuditEntry> AccessControl::auditLogForAgent(
    uint64_t agent_id, uint32_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuditEntry> results;
    for (auto it = audit_log_.rbegin();
         it != audit_log_.rend() && results.size() < count; ++it) {
        if (it->agent_id == agent_id) {
            results.push_back(*it);
        }
    }
    return results;
}

std::vector<AuditEntry> AccessControl::deniedAttempts(uint32_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AuditEntry> results;
    for (auto it = audit_log_.rbegin();
         it != audit_log_.rend() && results.size() < count; ++it) {
        if (!it->allowed) {
            results.push_back(*it);
        }
    }
    return results;
}

// ─── Stats ──────────────────────────────────────────────────────────────────

AccessControl::Stats AccessControl::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s = stats_;
    s.audit_entries = static_cast<uint32_t>(audit_log_.size());
    return s;
}

// ─── Internal ───────────────────────────────────────────────────────────────

const PolicyRule* AccessControl::findPolicy(const std::string& agent_name,
                                            const std::string& agent_type) const {
    for (auto& rule : policies_) {
        if (rule.agent_pattern == "*") return &rule;
        if (fnmatch(rule.agent_pattern.c_str(), agent_name.c_str(), 0) == 0) {
            return &rule;
        }
        if (fnmatch(rule.agent_pattern.c_str(), agent_type.c_str(), 0) == 0) {
            return &rule;
        }
    }
    return nullptr;
}

void AccessControl::audit(AuditEntry entry) {
    audit_log_.push_back(std::move(entry));
    if (audit_log_.size() > config_.max_audit_entries) {
        audit_log_.pop_front();
    }
}

bool AccessControl::isDenied(uint64_t agent_id,
                             const std::string& permission) const {
    auto it = agent_caps_.find(agent_id);
    if (it == agent_caps_.end()) return true;

    // Check if any policy denies this permission
    const PolicyRule* policy = findPolicy(it->second.agent_name, "");
    if (!policy) return false;

    for (auto& denied : policy->denied_permissions) {
        if (denied == permission) return true;
        if (fnmatch(denied.c_str(), permission.c_str(), 0) == 0) return true;
    }
    return false;
}

}  // namespace sparx::os
