#pragma once

/**
 * @file kv_cache_manager.h
 * @brief Defines scoped KV-cache acquisition, publication, invalidation, and lease completion.
 */

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "master_agent/common/types.h"

namespace master_agent::kv_cache {

struct PromptSegment {
    std::string segment_id;
    std::string digest;
    std::uint32_t token_count = 0;
};

struct KvCacheAcquireRequest {
    std::string request_id;
    std::string job_id;
    std::string namespace_id;
    std::string runtime_fingerprint_digest;
    std::vector<PromptSegment> segments;
    std::vector<std::string> replica_candidates;
    std::map<std::string, std::uint64_t> replica_candidate_epochs;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
};

struct KvCacheRuntimeBinding {
    std::string cache_id;
    std::string lease_id;
    std::string runtime_cache_handle;
    std::string replica_id;
    std::uint64_t replica_epoch = 0;
    std::uint32_t cached_token_count = 0;
    std::string fingerprint_digest;
};

struct KvCacheLease {
    std::string lease_id;
    std::string job_id;
    std::string request_id;
    std::string trace_id;
    std::string principal_id_hash;
    TaskPriority priority = TaskPriority::P1;
    KvCacheRuntimeBinding binding;
    std::int64_t expires_at_mono_ns = 0;
};

enum class AcquireOutcome : std::uint8_t {
    Hit,
    Miss,
    Unavailable
};

struct KvCacheAcquireResult {
    AcquireOutcome outcome = AcquireOutcome::Miss;
    std::optional<KvCacheLease> lease;
    std::size_t matched_segment_count = 0;
    std::string reason_code;
};

struct KvCachePublishRequest {
    std::string publish_id;
    std::string request_id;
    std::string job_id;
    std::string namespace_id;
    std::string runtime_fingerprint_digest;
    std::vector<PromptSegment> segments;
    std::string replica_id;
    std::uint64_t replica_epoch = 0;
    std::string runtime_cache_handle;
    std::uint64_t size_bytes = 0;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
};

struct KvCachePublishResult {
    bool admitted = false;
    bool existing = false;
    std::string cache_id;
    std::string reason_code;
};

struct KvCacheUseReport {
    std::string complete_id;
    std::string lease_id;
    std::string job_id;
    bool restore_succeeded = true;
};

/// At least one selector must be supplied.  Selectors are combined with AND
/// so a caller can fence a single replica generation without affecting other
/// compatible entries.
struct KvCacheInvalidateRequest {
    std::string invalidate_id;
    std::string request_id;
    std::optional<std::string> cache_id;
    std::optional<std::string> replica_id;
    std::optional<std::uint64_t> replica_epoch;
    std::optional<std::string> runtime_fingerprint_digest;
    std::string reason_code;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
};

struct KvCacheInvalidateResult {
    bool existing = false;
    std::size_t invalidated_count = 0;
    std::size_t protected_by_lease_count = 0;
};

struct KvCacheManagerStatus {
    std::size_t ready_entries = 0;
    std::size_t active_leases = 0;
    std::uint64_t used_bytes = 0;
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
};

/**
 * @brief Manages tenant-scoped KV entries through explicit leases.
 *
 * A cache hit is valid only for the complete runtime fingerprint and prompt
 * prefix. completeUse is idempotent and is required to release every lease.
 */
class IKvCacheManager {
public:
    virtual ~IKvCacheManager() = default;

    /**
     * Returns a fingerprint-compatible prefix and an explicit lease, or a miss.
     * A caller must not use an entry after the lease expires.
     */
    virtual Result<KvCacheAcquireResult> acquire(
        const KvCacheAcquireRequest& request,
        const CallContext& call) = 0;

    /// Publishes a cache entry idempotently under the request's stable identity.
    virtual Result<KvCachePublishResult> publish(
        const KvCachePublishRequest& request,
        const CallContext& call) = 0;

    /// Releases a lease and records use outcome; duplicate reports are idempotent.
    virtual Status completeUse(const KvCacheUseReport& report,
                               const CallContext& call) = 0;

    /**
     * Prevents new acquisitions immediately. Physical removal is deferred while
     * an already issued lease remains active.
     */
    virtual Result<KvCacheInvalidateResult> invalidate(
        const KvCacheInvalidateRequest& request,
        const CallContext& call) = 0;

    virtual KvCacheManagerStatus queryStatus(
        const CallContext& call) const = 0;
};

class KvCacheManager final : public IKvCacheManager {
public:
    KvCacheManager(std::shared_ptr<IRuntimeClock> clock,
                      std::shared_ptr<IdGenerator> ids,
                      std::uint64_t capacity_bytes = 64 * 1024 * 1024);

    Result<KvCacheAcquireResult> acquire(
        const KvCacheAcquireRequest& request,
        const CallContext& call) override;

    Result<KvCachePublishResult> publish(
        const KvCachePublishRequest& request,
        const CallContext& call) override;

    Status completeUse(const KvCacheUseReport& report,
                       const CallContext& call) override;

    Result<KvCacheInvalidateResult> invalidate(
        const KvCacheInvalidateRequest& request,
        const CallContext& call) override;

    KvCacheManagerStatus queryStatus(
        const CallContext& call) const override;

private:
    static std::size_t longestPrefix(
        const std::vector<PromptSegment>& cached,
        const std::vector<PromptSegment>& requested);

    bool ensureCapacityFor(std::uint64_t additional_bytes);

    void reapExpiredLeases() const;

    struct Entry {
        std::string cache_id;
        std::string namespace_id;
        std::string runtime_fingerprint_digest;
        std::vector<PromptSegment> segments;
        std::string replica_id;
        std::uint64_t replica_epoch = 0;
        std::string runtime_cache_handle;
        std::uint64_t size_bytes = 0;
        std::uint64_t hit_count = 0;
        std::int64_t last_access_mono_ns = 0;
        bool invalidated = false;
    };

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::uint64_t capacity_bytes_;
    mutable std::mutex mutex_;
    mutable std::map<std::string, Entry> entries_;
    mutable std::map<std::string, KvCacheLease> leases_;
    std::map<std::string, KvCachePublishResult> publish_results_;
    std::map<std::string, std::string> publish_digests_;
    std::map<std::string, Status> complete_results_;
    std::map<std::string, std::string> complete_digests_;
    std::map<std::string, KvCacheInvalidateResult> invalidate_results_;
    std::map<std::string, std::string> invalidate_digests_;
    std::uint64_t hit_count_ = 0;
    std::uint64_t miss_count_ = 0;
};

}  // namespace master_agent::kv_cache
