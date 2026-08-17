#pragma once

/**
 * @file kv_cache_manager.h
 * @brief KV-cache lifecycle management interfaces.
 *
 * The KV-cache manager handles lease acquisition, capacity planning,
 * and cross-session cache sharing for inference jobs.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace master_agent::kv_cache {

/// A segment of the prompt that may have cached KV state.
struct PromptSegment {
    std::string text;
    std::string digest;         // Hash for cache lookup
    bool cacheable = true;      // Some segments (e.g., dynamic context) skip cache
    std::uint32_t token_count = 0;
};

/// Represents a lease on KV-cache memory for an inference job.
struct KvCacheLease {
    std::string lease_id;
    std::uint64_t allocated_bytes = 0;
    std::uint32_t context_slots = 0;
    std::int64_t expires_mono_ns = 0;
    bool is_active = false;
};

/// Abstract interface for KV-cache management.
class IKvCacheManager {
public:
    virtual ~IKvCacheManager() = default;

    /// Acquire a lease for the given context length.
    virtual std::optional<KvCacheLease> acquire(
        std::uint32_t context_length,
        std::int64_t deadline_ns) = 0;

    /// Release a previously acquired lease.
    virtual void release(const std::string& lease_id) = 0;

    /// Query available capacity (in tokens).
    virtual std::uint32_t availableCapacity() const = 0;

    /// Check if a prompt segment has cached KV state.
    virtual bool hasCachedState(const std::string& digest) const = 0;
};

}  // namespace master_agent::kv_cache
