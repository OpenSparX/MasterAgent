/**
 * @file model_executor.cpp
 * @brief Starts model jobs and validates sealed completion results.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"
#include "include/inference_stream_accumulator.h"

namespace master_agent::inference {

bool InferenceFramework::startJob(
    const std::string& job_id, const std::string& replica_id) {

    kv_cache::KvCacheAcquireRequest acquire;
    CallContext kv_call;
    std::uint64_t acquire_token = 0;
    std::uint64_t acquire_version = 0;
    std::string attempt_id;
    std::uint64_t replica_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) return false;
        auto& job = found->second;
        const auto occupied = replica_to_job_.find(replica_id);
        if (job.snapshot.state != InferenceJobState::Ready ||
            job.external_operation != ExternalOperation::None ||
            job.kv_lease ||
            (occupied != replica_to_job_.end() &&
             occupied->second != job_id)) {
            return false;
        }

        job.snapshot.state = InferenceJobState::Running;
        job.snapshot.stage = job.snapshot.checkpoint_ref.empty()
                                 ? "PREFILL"
                                 : "RESTORE";
        job.snapshot.replica_id = replica_id;
        job.snapshot.replica_epoch =
            replica_epochs_.at(replica_id);
        job.snapshot.fencing_token =
            ++replica_fencing_sequences_.at(replica_id);
        job.snapshot.started_at_mono_ns =
            clock_->monotonicNowNs();
        job.snapshot.lease_id = ids_->next("model-slot-lease");
        replica_to_job_[replica_id] = job.snapshot.job_id;
        ++job.snapshot.version;
        job.external_operation = ExternalOperation::KvAcquire;
        acquire_token = ++job.external_token;
        acquire_version = job.snapshot.version;
        attempt_id = job.snapshot.attempt_id;
        replica_epoch = job.snapshot.replica_epoch;
        job.kv_complete_id = ids_->next("kv-complete");
        job.kv_restore_succeeded = true;

        acquire.request_id = job.request.request_id;
        acquire.job_id = job.request.job_id;
        acquire.namespace_id =
            secureDigest(
                         job.request.admission.principal_id + "|" +
                         job.request.session_id + "|" +
                         toString(
                             job.request.admission.caller_module_id));
        acquire.runtime_fingerprint_digest =
            runtimeFingerprint(job.request);
        acquire.segments = job.request.prompt_segments;
        if (acquire.segments.empty()) {
            acquire.segments.push_back(
                {"prompt", job.request.prompt_digest,
                 approximateTokens(job.request.prompt)});
        }
        acquire.replica_candidates = {replica_id};
        acquire.replica_candidate_epochs[replica_id] =
            replica_epoch;
        acquire.priority = job.request.priority;
        acquire.deadline_mono_ns = job.request.deadline_mono_ns;
        kv_call = {CallerModuleId::InferenceFramework,
                   job.request.request_id, job.request.trace_id,
                   job.request.admission.principal_id,
                   job.request.priority,
                   job.request.deadline_mono_ns, {}, 0,
                   job.request.admission.signature_ref.empty()
                       ? "policy:" +
                             job.request.admission.policy_snapshot_id
                       : job.request.admission.signature_ref};
        emit(job, "STARTED");
    }

    auto kv_result =
        Result<kv_cache::KvCacheAcquireResult>::Failure(
            Status::Error("inference", "INFERENCE_KV_ACQUIRE_FAILED",
                          "KV acquire did not return", true));
    try {
        kv_result = kv_cache_->acquire(acquire, kv_call);
    } catch (const std::exception&) {
        kv_result =
            Result<kv_cache::KvCacheAcquireResult>::Failure(
                Status::Error("inference",
                              "INFERENCE_KV_ACQUIRE_FAILED",
                              "KV acquire failed", true));
    } catch (...) {
        kv_result =
            Result<kv_cache::KvCacheAcquireResult>::Failure(
                Status::Error(
                    "inference", "INFERENCE_KV_ACQUIRE_FAILED",
                    "KV acquire raised a non-standard exception", true));
    }

    const bool returned_lease =
        kv_result.value && kv_result.value->lease;
    const bool declared_hit =
        kv_result.status.ok && kv_result.value &&
        kv_result.value->outcome == kv_cache::AcquireOutcome::Hit;
    std::uint64_t expected_cached_tokens = 0;
    if (kv_result.value &&
        kv_result.value->matched_segment_count <=
            acquire.segments.size()) {
        for (std::size_t index = 0;
             index < kv_result.value->matched_segment_count;
             ++index) {
            expected_cached_tokens +=
                acquire.segments[index].token_count;
        }
    }
    const bool valid_hit =
        declared_hit && returned_lease &&
        kv_result.value->matched_segment_count > 0 &&
        kv_result.value->matched_segment_count <=
            acquire.segments.size() &&
        kv_result.value->lease->job_id == job_id &&
        kv_result.value->lease->request_id ==
            acquire.request_id &&
        kv_result.value->lease->trace_id ==
            kv_call.trace_id &&
        kv_result.value->lease->principal_id_hash ==
            kv_call.principal_id_hash &&
        kv_result.value->lease->priority ==
            acquire.priority &&
        !kv_result.value->lease->lease_id.empty() &&
        kv_result.value->lease->lease_id ==
            kv_result.value->lease->binding.lease_id &&
        !kv_result.value->lease->binding.cache_id.empty() &&
        !kv_result.value->lease->binding.runtime_cache_handle.empty() &&
        kv_result.value->lease->binding.replica_id == replica_id &&
        kv_result.value->lease->binding.replica_epoch ==
            replica_epoch &&
        kv_result.value->lease->binding.fingerprint_digest ==
            acquire.runtime_fingerprint_digest &&
        kv_result.value->lease->binding.cached_token_count ==
            expected_cached_tokens &&
        kv_result.value->lease->expires_at_mono_ns >
            clock_->monotonicNowNs() &&
        kv_result.value->lease->expires_at_mono_ns <=
            acquire.deadline_mono_ns;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) return true;
        auto& job = found->second;
        if (job.external_operation != ExternalOperation::KvAcquire ||
            job.external_token != acquire_token ||
            job.snapshot.attempt_id != attempt_id ||
            job.snapshot.replica_epoch != replica_epoch) {
            return true;
        }
        job.external_operation = ExternalOperation::None;
        const bool identity_active =
            job.snapshot.version == acquire_version &&
            job.snapshot.state == InferenceJobState::Running &&
            job.snapshot.replica_id == replica_id;
        const bool expired =
            identity_active &&
            deadlineExpired(job.snapshot.deadline_mono_ns, *clock_);
        if (expired) {
            job.snapshot.state = InferenceJobState::Failed;
            job.snapshot.stage = "DEADLINE_EXPIRED";
            job.snapshot.last_error = StructuredError{
                "inference", "INFERENCE_DEADLINE_EXPIRED",
                "deadline expired during KV import", false,
                SideEffectState::NotApplicable};
            ++job.snapshot.version;
            emit(job, "FAILED", false);
        }
        const bool still_active = identity_active && !expired;
        if (!still_active) {
            const auto occupied = replica_to_job_.find(replica_id);
            if (occupied != replica_to_job_.end() &&
                occupied->second == job.snapshot.job_id) {
                replica_to_job_.erase(occupied);
            }
            if (job.snapshot.replica_id == replica_id) {
                job.snapshot.replica_id.clear();
            }
            if (job.snapshot.state ==
                InferenceJobState::Cancelled) {
                job.snapshot.stage = "CANCELLED";
            } else if (job.snapshot.state ==
                       InferenceJobState::Failed) {
                job.snapshot.stage = "DEADLINE_EXPIRED";
            }
            ++job.snapshot.version;
            if (!returned_lease) {
                emit(job, "RESOURCES_RELEASED", true);
            }
        }
        if (returned_lease) {

            job.kv_lease = kv_result.value->lease;
            job.kv_restore_succeeded = valid_hit;
            if (still_active) {
                job.snapshot.stage =
                    valid_hit ? "KV_RESTORED"
                              : "KV_FALLBACK_PROTOCOL_VIOLATION";
                ++job.snapshot.version;
            }
        } else {
            job.kv_complete_id.clear();
        }
    }

    if (returned_lease) {
        (void)releaseKvLease(job_id);
    }
    return true;
}

bool InferenceFramework::completeJob(
    const std::string& job_id) {

    InferenceRequest request;
    CallContext submit_call;
    std::string attempt_id;
    std::string replica_id;
    RuntimeInvocationSeal invocation;
    std::uint64_t infer_token = 0;
    std::uint64_t infer_version = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) return false;
        auto& job = found->second;
        if (job.snapshot.state != InferenceJobState::Running ||
            job.remaining_work_units != 0 ||
            job.external_operation != ExternalOperation::None ||
            job.kv_lease) {
            return false;
        }
        job.snapshot.stage = "RUNTIME_INFER";
        ++job.snapshot.version;
        job.external_operation = ExternalOperation::RuntimeInfer;
        infer_token = ++job.external_token;
        infer_version = job.snapshot.version;
        request = job.request;
        submit_call = job.submit_call;
        attempt_id = job.snapshot.attempt_id;
        replica_id = job.snapshot.replica_id;
        invocation.job_id = job.snapshot.job_id;
        invocation.attempt_id = job.snapshot.attempt_id;
        invocation.operation_id = job.snapshot.operation_id;
        invocation.replica_id = job.snapshot.replica_id;
        invocation.replica_epoch =
            job.snapshot.replica_epoch;
        invocation.lease_id = job.snapshot.lease_id;
        invocation.fencing_token =
            job.snapshot.fencing_token;
        invocation.control_epoch =
            job.snapshot.control_epoch;
        invocation.prompt_digest = job.request.prompt_digest;
        invocation.model_digest = secureDigest(
            job.request.model + "|" + runtime_->runtimeTag());
        invocation.deadline_mono_ns =
            job.snapshot.deadline_mono_ns;
        invocation.invocation_id =
            runtimeInvocationDigest(invocation);
    }

    auto fail_before_runtime =
        [&](const std::string& error_code,
            const std::string& error_message) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = jobs_.find(job_id);
            if (found == jobs_.end()) return true;
            auto& job = found->second;
            if (job.external_operation !=
                    ExternalOperation::RuntimeInfer ||
                job.external_token != infer_token ||
                job.snapshot.attempt_id != attempt_id ||
                job.snapshot.version != infer_version) {
                return true;
            }
            job.external_operation = ExternalOperation::None;
            const auto occupied =
                replica_to_job_.find(replica_id);
            if (occupied != replica_to_job_.end() &&
                occupied->second == job.snapshot.job_id) {
                replica_to_job_.erase(occupied);
            }
            job.snapshot.state = InferenceJobState::Failed;
            job.snapshot.stage = "PARENT_INVOCATION_REJECTED";
            job.snapshot.last_error = StructuredError{
                "inference", error_code, error_message, false,
                SideEffectState::NotApplicable};
            ++job.snapshot.version;
            emit(job, "FAILED", true);
            return true;
        };

    std::string parent_reservation_id;
    if (request.admission.caller_module_id ==
        CallerModuleId::SubAgent) {
        if (!lineage_validator_) {
            return fail_before_runtime(
                "INFERENCE_PARENT_LINEAGE_VALIDATOR_UNAVAILABLE",
                "parent lineage validator is unavailable at runtime boundary");
        }
        Result<std::string> acquired =
            Result<std::string>::Failure(Status::Error(
                "inference",
                "INFERENCE_PARENT_INVOCATION_LEASE_FAILED",
                "parent invocation lease was not acquired"));
        try {
            acquired =
                lineage_validator_
                    ->acquireInferenceParentInvocationLease(
                        request, submit_call);
        } catch (...) {
            return fail_before_runtime(
                "INFERENCE_PARENT_INVOCATION_LEASE_FAILED",
                "parent invocation lease acquisition failed");
        }
        if (!acquired.status.ok || !acquired.value ||
            acquired.value->empty()) {
            return fail_before_runtime(
                acquired.status.ok
                    ? "INFERENCE_PARENT_INVOCATION_LEASE_MISSING"
                    : acquired.status.error.code,
                acquired.status.ok
                    ? "parent invocation lease id is missing"
                    : acquired.status.error.message);
        }
        parent_reservation_id = *acquired.value;
        if (deadlineExpired(request.deadline_mono_ns, *clock_)) {
            try {
                (void)lineage_validator_
                    ->releaseInferenceParentInvocationLease(
                        parent_reservation_id);
            } catch (...) {
            }
            return fail_before_runtime(
                "INFERENCE_DEADLINE_EXCEEDED",
                "deadline expired before model runtime invocation");
        }
    }

    auto output = Result<InferenceOutput>::Failure(Status::Error(
        "inference", "INFERENCE_RUNTIME_FAILED",
        "runtime did not return an output", true));
    // Chunks are speculative: the commit fence below has not run yet, so this
    // accumulator forwards them only to the presentation sink and rebuilds the
    // text for independent verification at commit time.
    StreamAccumulator accumulator(
        invocation.invocation_id,
        streamSinkFor(job_id, invocation.invocation_id));
    const bool streaming = runtime_->supportsStreaming();
    try {
        if (streaming) {
            auto* clock = clock_.get();
            output = runtime_->inferStream(
                request, invocation,
                [&accumulator, clock](const InferenceChunk& chunk) {
                    return accumulator.accept(
                        chunk, clock->monotonicNowNs());
                });
        } else {
            output = runtime_->infer(request, invocation);
        }
    } catch (const std::exception&) {
        output = Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_FAILED",
            "model runtime execution failed", true));
    } catch (...) {
        output = Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_FAILED",
            "runtime raised a non-standard exception", true));
    }
    if (!parent_reservation_id.empty()) {
        Status released = Status::Error(
            "inference",
            "INFERENCE_PARENT_INVOCATION_RELEASE_FAILED",
            "parent invocation lease release failed");
        try {
            released =
                lineage_validator_
                    ->releaseInferenceParentInvocationLease(
                        parent_reservation_id);
        } catch (...) {
        }
        if (!released.ok) {
            output = Result<InferenceOutput>::Failure(
                Status::Error(
                    "inference",
                    "INFERENCE_PARENT_INVOCATION_RELEASE_FAILED",
                    "parent invocation lease release failed",
                    false, SideEffectState::Unknown));
        }
    }

    kv_cache::KvCachePublishRequest publish;
    CallContext kv_call;
    std::uint64_t publish_token = 0;
    std::uint64_t publish_version = 0;
    bool should_publish = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) return true;
        auto& job = found->second;
        if (job.external_operation != ExternalOperation::RuntimeInfer ||
            job.external_token != infer_token ||
            job.snapshot.attempt_id != attempt_id ||
            job.snapshot.operation_id !=
                invocation.operation_id ||
            job.snapshot.replica_epoch !=
                invocation.replica_epoch ||
            job.snapshot.lease_id != invocation.lease_id ||
            job.snapshot.fencing_token !=
                invocation.fencing_token) {
            return true;
        }
        job.external_operation = ExternalOperation::None;

        if (job.snapshot.version != infer_version ||
            job.snapshot.state != InferenceJobState::Running ||
            job.snapshot.replica_id != replica_id) {
            const auto occupied = replica_to_job_.find(replica_id);
            if (occupied != replica_to_job_.end() &&
                occupied->second == job.snapshot.job_id) {
                replica_to_job_.erase(occupied);
            }
            if (job.snapshot.replica_id == replica_id) {
                job.snapshot.replica_id.clear();
            }
            if (job.snapshot.state ==
                InferenceJobState::Cancelled) {
                job.snapshot.stage = "CANCELLED";
            } else if (job.snapshot.state ==
                       InferenceJobState::Failed) {
                job.snapshot.stage = "DEADLINE_EXPIRED";
            }
            ++job.snapshot.version;
            emit(job, "RESOURCES_RELEASED", true);
            return true;
        }

        const auto occupied = replica_to_job_.find(replica_id);
        if (occupied != replica_to_job_.end() &&
            occupied->second == job.snapshot.job_id) {
            replica_to_job_.erase(occupied);
        }
        if (deadlineExpired(job.snapshot.deadline_mono_ns, *clock_)) {
            job.snapshot.state = InferenceJobState::Failed;
            job.snapshot.stage = "DEADLINE_EXPIRED";
            job.snapshot.last_error = StructuredError{
                "inference", "INFERENCE_DEADLINE_EXPIRED",
                "deadline expired while the model runtime was executing",
                false, SideEffectState::NotApplicable};
            ++job.snapshot.version;
            emit(job, "FAILED", true);
            return true;
        }
        if (!output.status.ok || !output.value) {
            job.snapshot.state = InferenceJobState::Failed;
            job.snapshot.stage = "FAILED";
            job.snapshot.last_error =
                output.status.ok
                    ? StructuredError{
                          "inference",
                          "INFERENCE_RUNTIME_SUCCESS_WITHOUT_VALUE",
                          "model runtime returned success without output",
                          false, SideEffectState::NotApplicable}
                    : output.status.error;
            ++job.snapshot.version;
            emit(job, "FAILED", true);
            return true;
        }
        std::string identity_error;
        const auto& runtime_output = *output.value;
        const bool output_contract_valid =
            !runtime_output.raw_output.empty() &&
            runtime_output.raw_output.size() <=
                maxRuntimeOutputBytes(
                    request.admission.max_output_tokens) &&
            validUtf8RuntimeText(runtime_output.raw_output) &&
            !runtime_output.finish_reason.empty() &&
            runtime_output.finish_reason.size() <= 64 &&
            validUtf8RuntimeText(runtime_output.finish_reason) &&
            !runtime_output.model_id.empty() &&
            runtime_output.model_id.size() <= 256 &&
            validUtf8RuntimeText(runtime_output.model_id) &&
            !runtime_output.runtime_backend.empty() &&
            runtime_output.runtime_backend.size() <= 128 &&
            validUtf8RuntimeText(runtime_output.runtime_backend) &&
            !runtime_output.reality.empty() &&
            runtime_output.reality.size() <= 32 &&
            validUtf8RuntimeText(runtime_output.reality) &&
            !runtime_output.output_digest.empty() &&
            runtime_output.output_digest.size() <= 256 &&
            runtime_output.prompt_token_count <=
                request.admission.max_input_tokens &&
            runtime_output.generated_token_count <=
                request.admission.max_output_tokens &&
            runtime_output.model_id == request.model &&
            runtime_output.reality == request.reality;
        if (!output_contract_valid) {
            identity_error =
                "INFERENCE_OUTPUT_CONTRACT_VIOLATION";
        } else if (output.value->replica_epoch !=
            invocation.replica_epoch) {
            identity_error = "INFERENCE_STALE_REPLICA_EPOCH";
        } else if (output.value->lease_id !=
                       invocation.lease_id ||
                   output.value->fencing_token !=
                       invocation.fencing_token) {
            identity_error = "INFERENCE_STALE_LEASE";
        } else if (
            output.value->job_id != invocation.job_id ||
            output.value->attempt_id != invocation.attempt_id ||
            output.value->operation_id !=
                invocation.operation_id ||
            output.value->replica_id != invocation.replica_id ||
            output.value->control_epoch !=
                invocation.control_epoch ||
            output.value->prompt_digest !=
                invocation.prompt_digest ||
            output.value->model_digest != invocation.model_digest ||
            output.value->invocation_id !=
                invocation.invocation_id ||
            output.value->output_digest !=
                inferenceOutputDigest(*output.value)) {
            identity_error =
                "INFERENCE_OUTPUT_IDENTITY_MISMATCH";
        }
        // Stream verification runs last, after the seal has been proven, so a
        // divergence is attributable to this exact invocation. The framework
        // compares its own accumulated text against the sealed raw_output: a
        // runtime cannot misreport what it streamed, because it is not the one
        // reporting.
        if (identity_error.empty() && streaming) {
            if (!accumulator.violation().empty()) {
                identity_error = accumulator.violation();
            } else if (accumulator.classify(
                           runtime_output.raw_output) ==
                       StreamIntegrity::Diverged) {
                // Presentation sinks already emitted text the decision path
                // would not agree with. Committing would let a user act on
                // words the seal never covered.
                identity_error = "INFERENCE_STREAM_OUTPUT_DIVERGED";
            }
        }
        if (!identity_error.empty()) {
            job.snapshot.state = InferenceJobState::Failed;
            job.snapshot.stage = "OUTPUT_IDENTITY_INVALID";
            job.snapshot.last_error = StructuredError{
                "inference", identity_error,
                "runtime output does not bind the active invocation seal",
                false, SideEffectState::NotApplicable};
            ++job.snapshot.version;
            emit(job, "FAILED", true);
            return true;
        }

        // Framework observations, not runtime claims. Written after digest
        // validation precisely because they are excluded from
        // inferenceOutputDigest -- recording them cannot invalidate the seal
        // that was just proven.
        if (streaming) {
            output.value->stream_integrity =
                accumulator.classify(runtime_output.raw_output);
            output.value->streamed_chunk_count =
                accumulator.chunkCount();
            output.value->first_chunk_mono_ns =
                accumulator.firstChunkMonoNs();
        }

        job.snapshot.result = *output.value;
        job.snapshot.state = InferenceJobState::Completed;
        job.snapshot.stage = "KV_PUBLISH_PENDING";
        ++job.snapshot.version;
        emit(job, "OUTPUT_SEALED", true);

        publish.publish_id = ids_->next("kv-publish");
        publish.request_id = job.request.request_id;
        publish.job_id = job.request.job_id;
        publish.namespace_id =
            secureDigest(
                         job.request.admission.principal_id + "|" +
                         job.request.session_id + "|" +
                         toString(
                             job.request.admission.caller_module_id));
        publish.runtime_fingerprint_digest =
            runtimeFingerprint(job.request);
        publish.segments = job.request.prompt_segments;
        if (publish.segments.empty()) {
            publish.segments.push_back(
                {"prompt", job.request.prompt_digest,
                 approximateTokens(job.request.prompt)});
        }
        publish.replica_id = replica_id;
        publish.replica_epoch = invocation.replica_epoch;
        publish.runtime_cache_handle =
            "mock-kv:" + job.snapshot.attempt_id;
        publish.size_bytes =
            static_cast<std::uint64_t>(
                output.value->prompt_token_count) *
            64U;
        publish.priority = job.request.priority;
        publish.deadline_mono_ns = job.request.deadline_mono_ns;
        kv_call = {CallerModuleId::InferenceFramework,
                   job.request.request_id, job.request.trace_id,
                   job.request.admission.principal_id,
                   job.request.priority,
                   job.request.deadline_mono_ns, {}, 0,
                   job.request.admission.signature_ref.empty()
                       ? "policy:" +
                             job.request.admission.policy_snapshot_id
                       : job.request.admission.signature_ref};
        job.external_operation = ExternalOperation::KvPublish;
        publish_token = ++job.external_token;
        publish_version = job.snapshot.version;
        should_publish = true;
    }

    if (!should_publish) return true;
    auto publish_result =
        Result<kv_cache::KvCachePublishResult>::Failure(
            Status::Error("inference", "INFERENCE_KV_PUBLISH_FAILED",
                          "KV publish did not return", true));
    try {
        publish_result = kv_cache_->publish(publish, kv_call);
    } catch (const std::exception&) {
        publish_result =
            Result<kv_cache::KvCachePublishResult>::Failure(
                Status::Error("inference",
                              "INFERENCE_KV_PUBLISH_FAILED",
                              "KV publish failed", true));
    } catch (...) {
        publish_result =
            Result<kv_cache::KvCachePublishResult>::Failure(
                Status::Error(
                    "inference", "INFERENCE_KV_PUBLISH_FAILED",
                    "KV publish raised a non-standard exception", true));
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(job_id);
        if (found == jobs_.end()) return true;
        auto& job = found->second;
        if (job.external_operation != ExternalOperation::KvPublish ||
            job.external_token != publish_token ||
            job.snapshot.attempt_id != attempt_id) {
            return true;
        }
        job.external_operation = ExternalOperation::None;
        if (job.snapshot.version == publish_version &&
            job.snapshot.state == InferenceJobState::Completed) {
            job.snapshot.stage = "OUTPUT_SEALED";
            ++job.snapshot.version;
            if (publish_result.status.ok && publish_result.value &&
                publish_result.value->admitted) {
                emit(job, "KV_PUBLISHED", true);
            }
            emit(job, "COMPLETED", true);
        }
    }
    return true;
}


}  // namespace master_agent::inference
