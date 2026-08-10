/**
 * @file inference_framework_impl.cpp
 * @brief Implements framework lifecycle, queries, and control operations.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"

namespace master_agent::inference {

InferenceFramework::InferenceFramework(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<IModelRuntime> runtime,
    std::shared_ptr<kv_cache::IKvCacheManager> kv_cache,
    std::size_t replica_count,
    std::shared_ptr<IInferenceParentLineageValidator>
        lineage_validator)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      runtime_(std::move(runtime)),
      kv_cache_(std::move(kv_cache)),
      lineage_validator_(std::move(lineage_validator)) {
    const auto count =
        std::max<std::size_t>(1, std::min<std::size_t>(4, replica_count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto replica =
            "mock-replica-" + std::to_string(i + 1);
        replicas_.push_back(replica);
        replica_epochs_[replica] = 1;
        replica_fencing_sequences_[replica] = 0;
    }
}

// Admission freezes request identity and creates the idempotency ledger entry.
// Runtime sizing occurs outside mutex_ and is committed only if the captured
// job version and external-operation token still match.

Result<InferenceJobSnapshot> InferenceFramework::queryInference(
    const std::string& job_id, const CallContext& call) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!canQuery(call.caller) ||
        !hasHostModuleIdentity(call, call.caller) ||
        !clock_ || !isValidTaskPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<InferenceJobSnapshot>::Failure(Status::Error(
            "inference", "INFERENCE_QUERY_CALLER_NOT_ALLOWED",
            "caller may not query inference"));
    }
    const auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        return Result<InferenceJobSnapshot>::Failure(Status::Error(
            "inference", "INFERENCE_JOB_NOT_FOUND", "job was not found"));
    }
    if (call.request_id != found->second.request.request_id ||
        call.trace_id != found->second.request.trace_id ||
        call.principal_id_hash !=
            found->second.request.admission.principal_id) {
        return Result<InferenceJobSnapshot>::Failure(Status::Error(
            "inference", "INFERENCE_QUERY_IDENTITY_MISMATCH",
            "query identity does not own the inference job"));
    }
    const auto owner =
        found->second.request.admission.caller_module_id;
    if (!ownerControlAllowed(call.caller, owner)) {
        return Result<InferenceJobSnapshot>::Failure(Status::Error(
            "inference", "INFERENCE_QUERY_OWNER_MISMATCH",
            "caller module does not own the inference job"));
    }
    if (call.caller == owner &&
        call.authorization_ref !=
            found->second.submit_call.authorization_ref) {
        return Result<InferenceJobSnapshot>::Failure(Status::Error(
            "inference", "INFERENCE_QUERY_AUTHORIZATION_MISMATCH",
            "owner authorization is not bound to the job"));
    }
    return Result<InferenceJobSnapshot>::Success(found->second.snapshot);
}

Status InferenceFramework::cancelInference(
    const std::string& job_id, std::uint64_t control_epoch,
    const CallContext& call) {
    bool must_release_kv = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!canCancel(call.caller) ||
            !hasHostModuleIdentity(call, call.caller)) {
            return Status::Error("inference",
                                 "INFERENCE_CANCEL_CALLER_NOT_ALLOWED",
                                 "caller may not cancel inference");
        }
        if (!clock_ || control_epoch == 0 ||
            !isValidTaskPriority(call.priority) ||
            call.request_id.empty() || call.trace_id.empty() ||
            call.principal_id_hash.empty() ||
            call.deadline_mono_ns <= 0 ||
            deadlineExpired(call.deadline_mono_ns, *clock_) ||
            (call.priority == TaskPriority::P0 &&
             call.authorization_ref.rfind(
                 "trusted-safety:", 0) != 0)) {
            return Status::Error(
                "inference", "INFERENCE_CANCEL_CALL_INVALID",
                "cancel identity, priority or deadline is invalid");
        }
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) {
            return Status::Error("inference",
                                 "INFERENCE_JOB_NOT_FOUND",
                                 "job was not found");
        }
        auto& job = found->second;
        if (call.request_id != job.request.request_id ||
            call.trace_id != job.request.trace_id ||
            call.principal_id_hash !=
                job.request.admission.principal_id) {
            return Status::Error(
                "inference",
                "INFERENCE_CANCEL_IDENTITY_MISMATCH",
                "cancel call is not bound to the target job");
        }
        const auto owner =
            job.request.admission.caller_module_id;
        if (!ownerControlAllowed(call.caller, owner)) {
            return Status::Error(
                "inference", "INFERENCE_CANCEL_OWNER_MISMATCH",
                "caller module does not own the inference job");
        }
        if (call.caller == owner &&
            call.authorization_ref !=
                job.submit_call.authorization_ref) {
            return Status::Error(
                "inference",
                "INFERENCE_CANCEL_AUTHORIZATION_MISMATCH",
                "owner authorization is not bound to the job");
        }
        if (control_epoch < job.snapshot.control_epoch) {
            return Status::Error("inference",
                                 "INFERENCE_CONTROL_EPOCH_STALE",
                                 "control epoch is stale");
        }
        if (control_epoch == job.snapshot.control_epoch &&
            control_epoch != 0) {
            return Status::Ok();
        }
        if (job.snapshot.state == InferenceJobState::Completed ||
            job.snapshot.state == InferenceJobState::Failed ||
            job.snapshot.state == InferenceJobState::Cancelled) {
            return Status::Ok();
        }
        job.snapshot.control_epoch = control_epoch;
        const bool external_owns_replica =
            job.external_operation == ExternalOperation::KvAcquire ||
            job.external_operation == ExternalOperation::RuntimeInfer;
        if (!external_owns_replica &&
            !job.snapshot.replica_id.empty()) {
            const auto occupied =
                replica_to_job_.find(job.snapshot.replica_id);
            if (occupied != replica_to_job_.end() &&
                occupied->second == job.snapshot.job_id) {
                replica_to_job_.erase(occupied);
            }
        }
        if (!external_owns_replica) {
            job.snapshot.replica_id.clear();
        }
        job.snapshot.state = InferenceJobState::Cancelled;
        job.snapshot.stage = external_owns_replica
                                 ? "CANCEL_PENDING_EXTERNAL"
                                 : "CANCELLED";
        ++job.snapshot.version;
        must_release_kv =
            job.kv_lease &&
            job.external_operation == ExternalOperation::None;
        const bool released =
            !job.kv_lease &&
            !external_owns_replica &&
            job.external_operation !=
                ExternalOperation::KvCompleteUse;
        emit(job, "CANCELLED", released);
    }

    // A known lease is completed after the cancellation transition, without
    // holding mutex_.  An in-flight acquire performs the same cleanup when it
    // returns and observes the newer job version.
    return must_release_kv ? releaseKvLease(job_id) : Status::Ok();
}

Status InferenceFramework::attachStreamSink(
    const std::string& job_id, std::uint64_t control_epoch,
    InferenceStreamSink sink, const CallContext& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Attaching a sink exposes model output as it is produced, before the
    // commit fence has validated it. That is the same disclosure boundary as
    // cancellation control, so it carries the same caller checks.
    if (!canCancel(call.caller) ||
        !hasHostModuleIdentity(call, call.caller)) {
        return Status::Error(
            "inference",
            "INFERENCE_STREAM_SINK_CALLER_NOT_ALLOWED",
            "caller may not attach a stream sink");
    }
    if (!clock_ || control_epoch == 0 || !sink ||
        !isValidTaskPriority(call.priority) ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_) ||
        (call.priority == TaskPriority::P0 &&
         call.authorization_ref.rfind("trusted-safety:", 0) != 0)) {
        return Status::Error(
            "inference", "INFERENCE_STREAM_SINK_CALL_INVALID",
            "stream sink identity, priority or deadline is invalid");
    }
    auto found = jobs_.find(job_id);
    if (found == jobs_.end()) {
        return Status::Error("inference",
                             "INFERENCE_JOB_NOT_FOUND",
                             "job was not found");
    }
    auto& job = found->second;
    if (call.request_id != job.request.request_id ||
        call.trace_id != job.request.trace_id ||
        call.principal_id_hash !=
            job.request.admission.principal_id) {
        return Status::Error(
            "inference",
            "INFERENCE_STREAM_SINK_IDENTITY_MISMATCH",
            "stream sink call is not bound to the target job");
    }
    const auto owner = job.request.admission.caller_module_id;
    if (!ownerControlAllowed(call.caller, owner)) {
        return Status::Error(
            "inference",
            "INFERENCE_STREAM_SINK_OWNER_MISMATCH",
            "caller module does not own the inference job");
    }
    if (control_epoch < job.snapshot.control_epoch) {
        return Status::Error("inference",
                             "INFERENCE_CONTROL_EPOCH_STALE",
                             "control epoch is stale");
    }
    // Once the runtime has been entered the seal is frozen and the sink would
    // observe a partial stream it cannot interpret; once terminal there is
    // nothing left to stream.
    if (job.external_operation == ExternalOperation::RuntimeInfer) {
        return Status::Error(
            "inference", "INFERENCE_STREAM_SINK_TOO_LATE",
            "runtime invocation is already in flight");
    }
    if (job.snapshot.state == InferenceJobState::Completed ||
        job.snapshot.state == InferenceJobState::Failed ||
        job.snapshot.state == InferenceJobState::Cancelled) {
        return Status::Error(
            "inference", "INFERENCE_STREAM_SINK_TERMINAL",
            "job already reached a terminal state");
    }
    stream_sinks_[job_id] = std::move(sink);
    return Status::Ok();
}

InferenceStreamSink InferenceFramework::streamSinkFor(
    const std::string& job_id,
    const std::string& invocation_id) const {
    InferenceStreamSink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = stream_sinks_.find(job_id);
        if (found == stream_sinks_.end() || !found->second) {
            return nullptr;
        }
        sink = found->second;
    }
    // The sink is held by value from here on: caller code is never entered
    // while mutex_ is held, matching every other external boundary. The
    // invocation_id guard rejects a chunk from a superseded attempt even if
    // the sink outlived it.
    return [sink, invocation_id](
               const InferenceChunk& chunk) -> StreamControl {
        if (chunk.invocation_id != invocation_id) {
            return StreamControl::Abort;
        }
        return sink(chunk);
    };
}

Status InferenceFramework::requestPreempt(
    const std::string& job_id, TaskPriority arriving_priority,
    std::uint64_t control_epoch, const CallContext& call) {

    bool must_release_kv = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!canRequestPreempt(call.caller) ||
            !hasHostModuleIdentity(call, call.caller)) {
            return Status::Error(
                "inference", "INFERENCE_PREEMPT_CALLER_NOT_ALLOWED",
                "only AgentService or AgentDispatch may preempt");
        }
        if (!clock_ || control_epoch == 0 ||
            !isValidTaskPriority(call.priority) ||
            !isValidTaskPriority(arriving_priority) ||
            call.priority != arriving_priority ||
            call.request_id.empty() || call.trace_id.empty() ||
            call.principal_id_hash.empty() ||
            call.deadline_mono_ns <= 0 ||
            deadlineExpired(call.deadline_mono_ns, *clock_) ||
            (arriving_priority == TaskPriority::P0 &&
             call.authorization_ref.rfind(
                 "trusted-safety:", 0) != 0)) {
            return Status::Error(
                "inference", "INFERENCE_PREEMPT_CALL_INVALID",
                "preempt identity, priority or deadline is invalid");
        }
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) {
            return Status::Error("inference",
                                 "INFERENCE_JOB_NOT_FOUND",
                                 "job was not found");
        }
        auto& job = found->second;
        if (call.request_id != job.request.request_id ||
            call.trace_id != job.request.trace_id ||
            call.principal_id_hash !=
                job.request.admission.principal_id) {
            return Status::Error(
                "inference",
                "INFERENCE_PREEMPT_IDENTITY_MISMATCH",
                "preempt call is not bound to the target job");
        }
        const auto owner =
            job.request.admission.caller_module_id;
        if (!ownerControlAllowed(call.caller, owner)) {
            return Status::Error(
                "inference", "INFERENCE_PREEMPT_OWNER_MISMATCH",
                "control module may not preempt this inference owner");
        }
        if (call.caller == CallerModuleId::AgentDispatch &&
            call.authorization_ref !=
                job.submit_call.authorization_ref) {
            return Status::Error(
                "inference",
                "INFERENCE_PREEMPT_AUTHORIZATION_MISMATCH",
                "AgentDispatch control is not bound to the child job");
        }
        if (control_epoch < job.snapshot.control_epoch) {
            return Status::Error("inference",
                                 "INFERENCE_CONTROL_EPOCH_STALE",
                                 "control epoch is stale");
        }
        if (control_epoch == job.snapshot.control_epoch &&
            control_epoch != 0) {
            return Status::Ok();
        }
        if (job.snapshot.state != InferenceJobState::Running ||
            !isHigherPriority(arriving_priority,
                              job.snapshot.effective_priority)) {
            return Status::Error(
                "inference", "INFERENCE_PREEMPT_NOT_APPLICABLE",
                "victim is not running at lower priority");
        }
        if (job.external_operation == ExternalOperation::KvAcquire ||
            job.external_operation == ExternalOperation::RuntimeInfer) {
            return Status::Error(
                "inference", "INFERENCE_PREEMPT_NOT_AT_SAFE_POINT",
                "runtime/KV import callback still owns the replica", true);
        }
        job.snapshot.control_epoch = control_epoch;
        emit(job, "PREEMPT_ACCEPTED");
        emit(job, "SAFE_POINT_REACHED");
        const auto occupied =
            replica_to_job_.find(job.snapshot.replica_id);
        if (occupied != replica_to_job_.end() &&
            occupied->second == job.snapshot.job_id) {
            replica_to_job_.erase(occupied);
        }
        job.snapshot.checkpoint_ref =
            "checkpoint:" + job.snapshot.attempt_id + ":" +
            std::to_string(job.remaining_work_units);
        job.snapshot.replica_id.clear();
        job.snapshot.state = InferenceJobState::Suspended;
        job.snapshot.stage = "SUSPENDED";
        ++job.snapshot.version;
        must_release_kv =
            job.kv_lease &&
            job.external_operation == ExternalOperation::None;
        emit(job, "SUSPENDED",
             !job.kv_lease &&
                 job.external_operation !=
                     ExternalOperation::KvAcquire &&
                 job.external_operation !=
                     ExternalOperation::KvCompleteUse);
    }
    return must_release_kv ? releaseKvLease(job_id) : Status::Ok();
}

Status InferenceFramework::rebuildReplica(
    const std::string& replica_id, const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        return Status::Error(
            "inference", "INFERENCE_REBUILD_CALLER_NOT_ALLOWED",
            "only AgentService may rebuild a Replica");
    }
    if (!clock_ || !ids_ || !kv_cache_ || replica_id.empty() ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Status::Error(
            "inference", "INFERENCE_REBUILD_CALL_INVALID",
            "replica rebuild identity or deadline is invalid");
    }

    std::uint64_t retired_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto epoch = replica_epochs_.find(replica_id);
        if (epoch == replica_epochs_.end()) {
            return Status::Error(
                "inference", "INFERENCE_REPLICA_NOT_FOUND",
                "replica was not found");
        }
        if (replica_to_job_.count(replica_id) != 0) {
            return Status::Error(
                "inference", "INFERENCE_REPLICA_BUSY",
                "active ModelSlotLease must drain before rebuild",
                true);
        }
        retired_epoch = epoch->second;
        ++epoch->second;
    }

    kv_cache::KvCacheInvalidateRequest invalidate;
    invalidate.invalidate_id =
        ids_->next("kv-invalidate-replica-rebuild");
    invalidate.request_id = call.request_id;
    invalidate.replica_id = replica_id;
    invalidate.replica_epoch = retired_epoch;
    invalidate.reason_code = "REPLICA_REBUILT";
    invalidate.priority = call.priority;
    invalidate.deadline_mono_ns = call.deadline_mono_ns;
    const CallContext kv_call{
        CallerModuleId::InferenceFramework, call.request_id,
        call.trace_id, call.principal_id_hash, call.priority,
        call.deadline_mono_ns, {}, 0, call.authorization_ref};
    const auto invalidated =
        kv_cache_->invalidate(invalidate, kv_call);
    if (!invalidated.status.ok) {
        return Status::Error(
            "inference",
            "INFERENCE_REPLICA_REBUILT_KV_INVALIDATION_FAILED",
            "Replica epoch advanced but stale KV invalidation failed",
            true, SideEffectState::Committed);
    }
    return Status::Ok();
}

// One scheduler turn performs state selection under mutex_, releases the lock
// for KV/runtime calls, and then validates the frozen token before publishing.
// This avoids blocking control operations while rejecting late callbacks.

std::vector<InferenceEvent> InferenceFramework::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}


}  // namespace master_agent::inference
