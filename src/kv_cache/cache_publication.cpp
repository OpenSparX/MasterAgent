/**
 * @file cache_publication.cpp
 * @brief Publishes validated reusable KV-cache entries.
 */

#include "include/kv_access_control.h"
#include "include/kv_token_accounting.h"
#include "include/kv_operation_identity.h"

namespace master_agent::kv_cache {

Result<KvCachePublishResult> KvCacheManager::publish(
    const KvCachePublishRequest& request, const CallContext& call) {

    const auto caller = validateInferenceCaller(call);
    if (!caller.ok) {
        return Result<KvCachePublishResult>::Failure(caller);
    }
    if (request.request_id.empty() ||
        call.request_id != request.request_id ||
        call.priority != request.priority ||
        call.deadline_mono_ns != request.deadline_mono_ns) {
        return Result<KvCachePublishResult>::Failure(Status::Error(
            "kv_cache", "KV_PUBLISH_CONTEXT_MISMATCH",
            "publish request must bind the trusted call context"));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLeases();
    if (!clock_ ||
        deadlineExpired(request.deadline_mono_ns, *clock_)) {
        return Result<KvCachePublishResult>::Failure(Status::Error(
            "kv_cache", "KV_PUBLISH_EXPIRED",
            "publish deadline expired"));
    }
    const auto digest = publishDigest(request);
    const auto replay = publish_results_.find(request.publish_id);
    if (replay != publish_results_.end()) {
        if (publish_digests_.at(request.publish_id) != digest) {
            return Result<KvCachePublishResult>::Failure(Status::Error(
                "kv_cache", "KV_PUBLISH_IDEMPOTENCY_CONFLICT",
                "publish_id was reused with different content"));
        }
        return Result<KvCachePublishResult>::Success(replay->second);
    }
    if (!ids_ || request.publish_id.empty() ||
        request.job_id.empty() || request.namespace_id.empty() ||
        request.runtime_fingerprint_digest.empty() ||
        request.segments.empty() || request.replica_id.empty() ||
        request.replica_epoch == 0 ||
        request.runtime_cache_handle.empty() || request.size_bytes == 0 ||
        request.deadline_mono_ns <= 0) {
        return Result<KvCachePublishResult>::Failure(Status::Error(
            "kv_cache", "KV_PUBLISH_INVALID", "publish request is invalid"));
    }
    if (request.size_bytes > capacity_bytes_) {
        KvCachePublishResult rejected{false, false, {},
                                      "ENTRY_EXCEEDS_CAPACITY"};
        publish_results_[request.publish_id] = rejected;
        publish_digests_[request.publish_id] = digest;
        return Result<KvCachePublishResult>::Success(std::move(rejected));
    }
    for (const auto& pair : entries_) {
        const auto& entry = pair.second;
        if (!entry.invalidated &&
            entry.namespace_id == request.namespace_id &&
            entry.runtime_fingerprint_digest ==
                request.runtime_fingerprint_digest &&
            entry.replica_id == request.replica_id &&
            entry.replica_epoch == request.replica_epoch &&
            longestPrefix(entry.segments, request.segments) ==
                request.segments.size() &&
            entry.segments.size() == request.segments.size()) {
            KvCachePublishResult existing{true, true, entry.cache_id,
                                          "EXISTING"};
            publish_results_[request.publish_id] = existing;
            publish_digests_[request.publish_id] = digest;
            return Result<KvCachePublishResult>::Success(std::move(existing));
        }
    }
    if (!ensureCapacityFor(request.size_bytes)) {
        KvCachePublishResult rejected{
            false, false, {}, "CAPACITY_PINNED_BY_ACTIVE_LEASES"};
        publish_results_[request.publish_id] = rejected;
        publish_digests_[request.publish_id] = digest;
        return Result<KvCachePublishResult>::Success(
            std::move(rejected));
    }
    Entry entry;
    entry.cache_id = ids_->next("kv-cache");
    entry.namespace_id = request.namespace_id;
    entry.runtime_fingerprint_digest = request.runtime_fingerprint_digest;
    entry.segments = request.segments;
    entry.replica_id = request.replica_id;
    entry.replica_epoch = request.replica_epoch;
    entry.runtime_cache_handle = request.runtime_cache_handle;
    entry.size_bytes = request.size_bytes;
    entry.last_access_mono_ns = clock_->monotonicNowNs();
    const auto cache_id = entry.cache_id;
    entries_[cache_id] = std::move(entry);
    KvCachePublishResult result{true, false, cache_id, "ADMITTED"};
    publish_results_[request.publish_id] = result;
    publish_digests_[request.publish_id] = digest;
    return Result<KvCachePublishResult>::Success(std::move(result));
}


}  // namespace master_agent::kv_cache
