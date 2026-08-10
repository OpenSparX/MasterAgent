/**
 * @file lease_completion.cpp
 * @brief Completes fenced KV-cache use and updates entry recency.
 */

#include "include/kv_access_control.h"
#include "include/kv_token_accounting.h"
#include "include/kv_operation_identity.h"

namespace master_agent::kv_cache {

Status KvCacheManager::completeUse(const KvCacheUseReport& report,
                                      const CallContext& call) {

    const auto caller = validateInferenceCaller(call);
    if (!caller.ok) {
        return caller;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reapExpiredLeases();
    if (!clock_ || deadlineExpired(
                       call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "kv_cache", "KV_COMPLETE_EXPIRED",
            "completeUse control deadline expired");
    }
    const auto digest = completionDigest(report, call);
    const auto replay = complete_results_.find(report.complete_id);
    if (replay != complete_results_.end()) {
        if (complete_digests_.at(report.complete_id) != digest) {
            return Status::Error(
                "kv_cache", "KV_COMPLETE_IDEMPOTENCY_CONFLICT",
                "complete_id was reused with different content");
        }
        return replay->second;
    }
    const auto found = leases_.find(report.lease_id);
    if (report.complete_id.empty() || found == leases_.end() ||
        found->second.job_id != report.job_id ||
        found->second.request_id != call.request_id ||
        found->second.trace_id != call.trace_id ||
        found->second.principal_id_hash !=
            call.principal_id_hash ||
        found->second.priority != call.priority) {
        return Status::Error("kv_cache", "KV_LEASE_INVALID",
                             "lease does not belong to job");
    }
    const auto cache_id = found->second.binding.cache_id;
    leases_.erase(found);
    const bool still_leased = std::any_of(
        leases_.begin(), leases_.end(),
        [&cache_id](const auto& lease) {
            return lease.second.binding.cache_id == cache_id;
        });
    const auto entry = entries_.find(cache_id);
    if (entry != entries_.end() && entry->second.invalidated &&
        !still_leased) {
        entries_.erase(entry);
    }
    const auto result = Status::Ok();
    complete_results_[report.complete_id] = result;
    complete_digests_[report.complete_id] = digest;
    return result;
}


}  // namespace master_agent::kv_cache
