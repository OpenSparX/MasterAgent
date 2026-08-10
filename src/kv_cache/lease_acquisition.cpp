/**
 * @file lease_acquisition.cpp
 * @brief Constructs the cache manager and acquires fenced KV leases.
 */

#include "include/kv_access_control.h"
#include "include/kv_token_accounting.h"
#include "include/kv_operation_identity.h"

namespace master_agent::kv_cache {

KvCacheManager::KvCacheManager(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::uint64_t capacity_bytes)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      capacity_bytes_(capacity_bytes) {}

Result<KvCacheAcquireResult> KvCacheManager::acquire(
    const KvCacheAcquireRequest& request, const CallContext& call) {

    const auto caller = validateInferenceCaller(call);
    if (!caller.ok) {
        return Result<KvCacheAcquireResult>::Failure(caller);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLeases();
    if (!clock_ || !ids_ || request.job_id.empty() ||
        request.request_id.empty() ||
        request.namespace_id.empty() ||
        request.runtime_fingerprint_digest.empty() ||
        request.segments.empty() || request.deadline_mono_ns <= 0 ||
        request.replica_candidates.empty() ||
        call.request_id != request.request_id ||
        call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        call.priority != request.priority ||
        call.deadline_mono_ns != request.deadline_mono_ns) {
        return Result<KvCacheAcquireResult>::Failure(Status::Error(
            "kv_cache", "KV_ACQUIRE_INVALID", "acquire request is invalid"));
    }
    for (const auto& replica : request.replica_candidates) {
        const auto epoch =
            request.replica_candidate_epochs.find(replica);
        if (replica.empty() ||
            epoch == request.replica_candidate_epochs.end() ||
            epoch->second == 0) {
            return Result<KvCacheAcquireResult>::Failure(Status::Error(
                "kv_cache", "KV_ACQUIRE_REPLICA_EPOCH_REQUIRED",
                "every Replica candidate requires a non-zero epoch"));
        }
    }
    if (deadlineExpired(request.deadline_mono_ns, *clock_)) {
        return Result<KvCacheAcquireResult>::Failure(Status::Error(
            "kv_cache", "KV_ACQUIRE_EXPIRED", "acquire deadline expired"));
    }

    Entry* best = nullptr;
    std::size_t best_prefix = 0;
    for (auto& pair : entries_) {
        auto& entry = pair.second;
        if (entry.invalidated ||
            entry.namespace_id != request.namespace_id ||
            entry.runtime_fingerprint_digest !=
                request.runtime_fingerprint_digest ||
            std::find(request.replica_candidates.begin(),
                      request.replica_candidates.end(),
                      entry.replica_id) == request.replica_candidates.end()) {
            continue;
        }
        const auto expected_epoch =
            request.replica_candidate_epochs.find(entry.replica_id);
        if (expected_epoch == request.replica_candidate_epochs.end() ||
            expected_epoch->second != entry.replica_epoch) {
            continue;
        }
        const auto prefix = longestPrefix(entry.segments, request.segments);
        if (prefix > best_prefix) {
            best = &entry;
            best_prefix = prefix;
        }
    }
    if (!best || best_prefix == 0) {
        ++miss_count_;
        return Result<KvCacheAcquireResult>::Success(
            {AcquireOutcome::Miss, std::nullopt, 0, "NO_PREFIX_MATCH"});
    }

    KvCacheLease lease;
    lease.lease_id = ids_->next("kv-lease");
    lease.job_id = request.job_id;
    lease.request_id = request.request_id;
    lease.trace_id = call.trace_id;
    lease.principal_id_hash = call.principal_id_hash;
    lease.priority = request.priority;
    lease.expires_at_mono_ns =
        std::min(request.deadline_mono_ns,
                 clock_->monotonicNowNs() +
                     std::int64_t{10'000'000'000});
    lease.binding.cache_id = best->cache_id;
    lease.binding.lease_id = lease.lease_id;
    lease.binding.runtime_cache_handle = best->runtime_cache_handle;
    lease.binding.replica_id = best->replica_id;
    lease.binding.replica_epoch = best->replica_epoch;
    lease.binding.cached_token_count =
        tokenCount(best->segments, best_prefix);
    lease.binding.fingerprint_digest = best->runtime_fingerprint_digest;
    leases_[lease.lease_id] = lease;
    ++best->hit_count;
    best->last_access_mono_ns = clock_->monotonicNowNs();
    ++hit_count_;
    return Result<KvCacheAcquireResult>::Success(
        {AcquireOutcome::Hit, lease, best_prefix, "LONGEST_PREFIX_HIT"});
}


}  // namespace master_agent::kv_cache
