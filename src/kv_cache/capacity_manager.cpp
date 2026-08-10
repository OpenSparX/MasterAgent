/**
 * @file capacity_manager.cpp
 * @brief Reports cache state, matches prefixes, reclaims entries, and expires leases.
 */

#include "include/kv_access_control.h"
#include "include/kv_token_accounting.h"
#include "include/kv_operation_identity.h"

namespace master_agent::kv_cache {

KvCacheManagerStatus KvCacheManager::queryStatus(
    const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    KvCacheManagerStatus status;
    if (!validateInferenceCaller(call).ok ||
        !clock_ || deadlineExpired(
                       call.deadline_mono_ns, *clock_)) {
        return status;
    }
    reapExpiredLeases();
    status.active_leases = leases_.size();
    status.hit_count = hit_count_;
    status.miss_count = miss_count_;
    for (const auto& pair : entries_) {
        status.used_bytes += pair.second.size_bytes;
        if (!pair.second.invalidated) {
            ++status.ready_entries;
        }
    }
    return status;
}

std::size_t KvCacheManager::longestPrefix(
    const std::vector<PromptSegment>& cached,
    const std::vector<PromptSegment>& requested) {
    const auto limit = std::min(cached.size(), requested.size());
    std::size_t matched = 0;
    while (matched < limit &&
           cached[matched].segment_id == requested[matched].segment_id &&
           cached[matched].digest == requested[matched].digest) {
        ++matched;
    }
    return matched;
}

bool KvCacheManager::ensureCapacityFor(
    std::uint64_t additional_bytes) {
    auto used = std::uint64_t{0};
    for (const auto& pair : entries_) {
        // Invalidated entries remain physically resident while protected by
        // a lease and therefore still consume capacity.
        used += pair.second.size_bytes;
    }
    while (used + additional_bytes > capacity_bytes_) {
        auto victim = entries_.end();
        for (auto candidate = entries_.begin(); candidate != entries_.end();
             ++candidate) {
            const bool leased = std::any_of(
                leases_.begin(), leases_.end(),
                [&candidate](const auto& lease) {
                    return lease.second.binding.cache_id ==
                           candidate->second.cache_id;
                });
            if (candidate->second.invalidated || leased) {
                continue;
            }
            if (victim == entries_.end() ||
                std::tie(candidate->second.hit_count,
                         candidate->second.last_access_mono_ns) <
                    std::tie(victim->second.hit_count,
                             victim->second.last_access_mono_ns)) {
                victim = candidate;
            }
        }
        if (victim == entries_.end()) {
            return false;  // Active leases are never evicted.
        }
        used -= victim->second.size_bytes;
        entries_.erase(victim);
    }
    return true;
}

void KvCacheManager::reapExpiredLeases() const {
    if (!clock_) return;
    // Expiry is an ownership-warning deadline, not proof that the model job
    // or process is dead. Only an authenticated completeUse (or a future
    // platform liveness/recovery coordinator) may release the lease. Silent
    // front-path expiry would permit eviction while a Runtime still imports
    // the handle and would make a delayed idempotent completion impossible.
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        const bool leased = std::any_of(
            leases_.begin(), leases_.end(),
            [&entry](const auto& lease) {
                return lease.second.binding.cache_id ==
                       entry->second.cache_id;
            });
        if (entry->second.invalidated && !leased) {
            entry = entries_.erase(entry);
        } else {
            ++entry;
        }
    }
}

}  // namespace master_agent::kv_cache
