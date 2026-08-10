#pragma once

/**
 * @file kv_operation_identity.h
 * @brief Private publish, invalidation, and completion identity helpers.
 *
 * This header is private to KV Cache and is not part of the installed API.
 */

#include "master_agent/kv_cache/kv_cache_manager.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace master_agent::kv_cache {
namespace {

std::string publishDigest(const KvCachePublishRequest& request) {
    std::string encoded =
        request.request_id + "|" + request.job_id + "|" +
        request.namespace_id + "|" +
        request.runtime_fingerprint_digest + "|" +
        request.replica_id + "|" +
        std::to_string(request.replica_epoch) + "|" +
        request.runtime_cache_handle + "|" +
        std::to_string(request.size_bytes) + "|" +
        toString(request.priority) + "|" +
        std::to_string(request.deadline_mono_ns);
    for (const auto& segment : request.segments) {
        encoded += "|" + segment.segment_id + ":" + segment.digest +
                   ":" + std::to_string(segment.token_count);
    }
    return secureDigest(encoded);
}

std::string invalidateDigest(const KvCacheInvalidateRequest& request) {
    return secureDigest(
        request.request_id + "|" +
        request.cache_id.value_or("") + "|" +
        request.replica_id.value_or("") + "|" +
        (request.replica_epoch
             ? std::to_string(*request.replica_epoch)
             : std::string{}) +
        "|" + request.runtime_fingerprint_digest.value_or("") + "|" +
        request.reason_code + "|" + toString(request.priority) + "|" +
        std::to_string(request.deadline_mono_ns));
}

std::string completionDigest(const KvCacheUseReport& report,
                             const CallContext& call) {
    return secureDigest(
        report.lease_id + "|" + report.job_id + "|" +
        (report.restore_succeeded ? "1" : "0") + "|" +
        call.request_id + "|" + call.trace_id + "|" +
        call.principal_id_hash + "|" + toString(call.priority));
}

}  // namespace
}  // namespace master_agent::kv_cache

