/**
 * @file kv_coordinator.cpp
 * @brief Releases fenced KV-cache leases owned by inference jobs.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"

namespace master_agent::inference {

Status InferenceFramework::releaseKvLease(
    const std::string& job_id) {
    kv_cache::KvCacheUseReport report;
    CallContext kv_call;
    std::uint64_t complete_token = 0;
    std::uint64_t complete_version = 0;
    std::string attempt_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) {
            return Status::Error("inference",
                                 "INFERENCE_JOB_NOT_FOUND",
                                 "job was not found");
        }
        auto& job = found->second;
        if (!job.kv_lease) return Status::Ok();
        if (job.external_operation != ExternalOperation::None) {
            return Status::Error(
                "inference", "INFERENCE_KV_RELEASE_INFLIGHT",
                "another external operation owns the job", true);
        }
        if (job.kv_complete_id.empty()) {
            job.kv_complete_id = ids_->next("kv-complete");
        }
        report = {job.kv_complete_id, job.kv_lease->lease_id,
                  job.request.job_id, job.kv_restore_succeeded};
        const auto cleanup_deadline =
            clock_->monotonicNowNs() + 1'000'000'000LL;
        kv_call = {CallerModuleId::InferenceFramework,
                   job.request.request_id, job.request.trace_id,
                   job.request.admission.principal_id,
                   job.request.priority,
                   cleanup_deadline, {}, 0,
                   job.request.admission.signature_ref.empty()
                       ? "policy:" +
                             job.request.admission.policy_snapshot_id
                       : job.request.admission.signature_ref};
        job.external_operation = ExternalOperation::KvCompleteUse;
        complete_token = ++job.external_token;
        complete_version = job.snapshot.version;
        attempt_id = job.snapshot.attempt_id;
    }

    Status released = Status::Error(
        "inference", "INFERENCE_KV_RELEASE_FAILED",
        "KV completeUse did not return", true);
    try {
        released = kv_cache_->completeUse(report, kv_call);
    } catch (const std::exception&) {
        released = Status::Error(
            "inference", "INFERENCE_KV_RELEASE_FAILED",
            "KV completeUse failed", true,
            SideEffectState::Unknown);
    } catch (...) {
        released = Status::Error(
            "inference", "INFERENCE_KV_RELEASE_FAILED",
            "KV completeUse raised a non-standard exception", true,
            SideEffectState::Unknown);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) return released;
        auto& job = found->second;
        if (job.external_operation !=
                ExternalOperation::KvCompleteUse ||
            job.external_token != complete_token ||
            job.snapshot.attempt_id != attempt_id) {
            return released;
        }
        job.external_operation = ExternalOperation::None;
        const bool same_lease =
            job.kv_lease &&
            job.kv_lease->lease_id == report.lease_id;
        if (released.ok && same_lease) {
            job.kv_lease.reset();
            job.kv_complete_id.clear();
            if (job.snapshot.version == complete_version &&
                job.snapshot.state == InferenceJobState::Running &&
                job.snapshot.stage == "KV_RELEASE_PENDING") {
                job.snapshot.stage = "KV_RESTORED";
                ++job.snapshot.version;
            }
            if (job.snapshot.state ==
                    InferenceJobState::Cancelled ||
                job.snapshot.state == InferenceJobState::Failed) {
                emit(job, "RESOURCES_RELEASED", true);
            }
        } else if (!released.ok && same_lease &&
                   job.snapshot.version == complete_version &&
                   job.snapshot.state == InferenceJobState::Running) {
            job.snapshot.stage = "KV_RELEASE_PENDING";
            ++job.snapshot.version;
        }
    }
    return released;
}


}  // namespace master_agent::inference
