/**
 * @file cache_invalidation.cpp
 * @brief Invalidates matching cache entries with idempotent control semantics.
 */

#include "include/kv_access_control.h"
#include "include/kv_token_accounting.h"
#include "include/kv_operation_identity.h"

namespace master_agent::kv_cache {

Result<KvCacheInvalidateResult> KvCacheManager::invalidate(
    const KvCacheInvalidateRequest& request, const CallContext& call) {

    const auto caller = validateInferenceCaller(call);
    if (!caller.ok) {
        return Result<KvCacheInvalidateResult>::Failure(caller);
    }
    const bool has_selector =
        request.cache_id || request.replica_id || request.replica_epoch ||
        request.runtime_fingerprint_digest;
    if (request.invalidate_id.empty() || request.request_id.empty() ||
        request.reason_code.empty() || !has_selector ||
        request.deadline_mono_ns <= 0 ||
        call.request_id != request.request_id ||
        call.priority != request.priority ||
        call.deadline_mono_ns != request.deadline_mono_ns) {
        return Result<KvCacheInvalidateResult>::Failure(Status::Error(
            "kv_cache", "KV_INVALIDATE_INVALID",
            "invalidate request or call binding is invalid"));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLeases();
    if (!clock_ ||
        deadlineExpired(request.deadline_mono_ns, *clock_)) {
        return Result<KvCacheInvalidateResult>::Failure(Status::Error(
            "kv_cache", "KV_INVALIDATE_EXPIRED",
            "invalidate deadline expired"));
    }
    const auto digest = invalidateDigest(request);
    const auto replay =
        invalidate_results_.find(request.invalidate_id);
    if (replay != invalidate_results_.end()) {
        if (invalidate_digests_.at(request.invalidate_id) != digest) {
            return Result<KvCacheInvalidateResult>::Failure(Status::Error(
                "kv_cache", "KV_INVALIDATE_IDEMPOTENCY_CONFLICT",
                "invalidate_id was reused with different selectors"));
        }
        auto result = replay->second;
        result.existing = true;
        return Result<KvCacheInvalidateResult>::Success(
            std::move(result));
    }

    KvCacheInvalidateResult result;
    for (auto& pair : entries_) {
        auto& entry = pair.second;
        if ((request.cache_id && pair.first != *request.cache_id) ||
            (request.replica_id &&
             entry.replica_id != *request.replica_id) ||
            (request.replica_epoch &&
             entry.replica_epoch != *request.replica_epoch) ||
            (request.runtime_fingerprint_digest &&
             entry.runtime_fingerprint_digest !=
                 *request.runtime_fingerprint_digest)) {
            continue;
        }
        if (!entry.invalidated) {
            entry.invalidated = true;
            ++result.invalidated_count;
        }
        const bool leased = std::any_of(
            leases_.begin(), leases_.end(),
            [&pair](const auto& lease) {
                return lease.second.binding.cache_id ==
                       pair.second.cache_id;
            });
        if (leased) ++result.protected_by_lease_count;
    }
    invalidate_results_[request.invalidate_id] = result;
    invalidate_digests_[request.invalidate_id] = digest;

    // An active lease keeps its invalidated entry alive.  Unleased entries
    // may be reclaimed immediately because invalidate already fenced hits.
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
    return Result<KvCacheInvalidateResult>::Success(std::move(result));
}


}  // namespace master_agent::kv_cache
