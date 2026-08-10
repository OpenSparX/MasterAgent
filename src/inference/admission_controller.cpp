/**
 * @file admission_controller.cpp
 * @brief Validates and admits inference requests.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"

namespace master_agent::inference {

InferenceAcceptance InferenceFramework::submitInference(
    const InferenceRequest& request, const CallContext& call) {

    if ((call.caller != CallerModuleId::IntentRecognitionEngine &&
         call.caller != CallerModuleId::MemoryService &&
         call.caller != CallerModuleId::SubAgent) ||
        request.admission.caller_module_id != call.caller ||
        !hasHostModuleIdentity(call, call.caller) ||
        request.idempotency_key.empty() ||
        request.admission.principal_id.empty() ||
        request.request_id.empty() ||
        request.admission.source_request_id != request.request_id ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.principal_id_hash !=
            request.admission.principal_id ||
        !isValidTaskPriority(call.priority) ||
        !isValidTaskPriority(request.priority) ||
        !isValidTaskPriority(
            request.admission.granted_priority)) {
        return {false, false, request.job_id,
                "INFERENCE_ADMISSION_IDENTITY_MISMATCH"};
    }
    if (!clock_ || !ids_ || !runtime_ || !kv_cache_) {
        return {false, false, request.job_id, "INFERENCE_NOT_READY"};
    }
    const auto caller_validation =
        validateSubmitCaller(request, call);
    if (!caller_validation.ok) {
        return {false, false, request.job_id,
                caller_validation.error.code};
    }
    if (call.caller == CallerModuleId::SubAgent) {
        if (!lineage_validator_) {
            return {
                false, false, request.job_id,
                "INFERENCE_PARENT_LINEAGE_VALIDATOR_UNAVAILABLE"};
        }
        Status lineage_status;
        try {
            lineage_status =
                lineage_validator_->validateInferenceParentLineage(
                    request, call);
        } catch (...) {
            return {
                false, false, request.job_id,
                "INFERENCE_PARENT_LINEAGE_VALIDATION_FAILED"};
        }
        if (!lineage_status.ok) {
            return {false, false, request.job_id,
                    lineage_status.error.code};
        }
    }
    if (request.reality != "SIMULATED") {
        return {false, false, request.job_id,
                "INFERENCE_REALITY_NOT_AVAILABLE"};
    }
    if (deadlineExpired(call.deadline_mono_ns, *clock_) ||
        deadlineExpired(request.deadline_mono_ns, *clock_)) {
        return {false, false, request.job_id,
                "INFERENCE_DEADLINE_EXCEEDED"};
    }
    const auto digest = requestDigest(request);
    const auto ledger_key = scopedIdempotencyLedgerKey(
        request.admission.principal_id,
        request.idempotency_key);
    std::uint64_t sizing_token = 0;
    std::uint64_t sizing_version = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto replay = idempotency_to_job_.find(ledger_key);
        if (replay != idempotency_to_job_.end()) {
            const auto& existing = jobs_.at(replay->second);
            if (existing.request_digest != digest) {
                return {false, false, request.job_id,
                        "INFERENCE_IDEMPOTENCY_CONFLICT"};
            }
            return {true, true, existing.snapshot.job_id, {}};
        }
        if (request.job_id.empty() || request.request_id.empty() ||
            request.prompt.empty() || request.prompt_digest.empty() ||
            request.idempotency_key.empty() ||
            request.deadline_mono_ns <= 0 ||
            (request.inference_phase != "FIRST_INFERENCE" &&
             request.inference_phase != "SECOND_INFERENCE") ||
            (request.inference_phase == "SECOND_INFERENCE" &&
             request.admission.caller_module_id !=
                 CallerModuleId::IntentRecognitionEngine) ||
            request.prompt_digest != secureDigest(request.prompt) ||
            deadlineExpired(request.deadline_mono_ns, *clock_)) {
            return {false, false, request.job_id,
                    "INFERENCE_REQUEST_INVALID"};
        }
        if (!request.adapter.empty()) {
            return {false, false, request.job_id,
                    "INFERENCE_ADAPTER_NOT_ENABLED"};
        }
        if (jobs_.count(request.job_id) != 0) {
            return {false, false, request.job_id,
                    "INFERENCE_JOB_ID_CONFLICT"};
        }

        Job job;
        job.request = request;
        job.submit_call = call;
        job.request_digest = digest;
        job.snapshot.job_id = request.job_id;
        job.snapshot.attempt_id = ids_->next("infer-attempt");
        job.snapshot.operation_id = ids_->next("infer-operation");

        job.snapshot.state = InferenceJobState::Accepted;
        job.snapshot.base_priority = request.priority;
        job.snapshot.effective_priority = request.priority;
        job.snapshot.enqueue_sequence = ++enqueue_sequence_;
        job.snapshot.queued_at_mono_ns = clock_->monotonicNowNs();
        job.snapshot.deadline_mono_ns = request.deadline_mono_ns;
        job.snapshot.stage = "RUNTIME_SIZING";
        job.external_operation = ExternalOperation::RuntimeSizing;
        sizing_token = ++job.external_token;
        sizing_version = job.snapshot.version;
        jobs_[request.job_id] = std::move(job);
        idempotency_to_job_[ledger_key] = request.job_id;
        emit(jobs_.at(request.job_id), "ACCEPTED");
    }

    std::uint32_t required_work_units = 1;
    std::optional<StructuredError> sizing_error;
    try {
        required_work_units =
            std::max<std::uint32_t>(1,
                                    runtime_->requiredWorkUnits(request));
    } catch (const std::exception&) {
        sizing_error = StructuredError{
            "inference", "INFERENCE_RUNTIME_SIZING_FAILED",
            "model runtime sizing failed", true,
            SideEffectState::NotApplicable};
    } catch (...) {
        sizing_error = StructuredError{
            "inference", "INFERENCE_RUNTIME_SIZING_FAILED",
            "runtime sizing raised a non-standard exception", true,
            SideEffectState::NotApplicable};
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = jobs_.find(request.job_id);
        if (found == jobs_.end()) {
            return {false, false, request.job_id,
                    "INFERENCE_ADMISSION_LOST"};
        }
        auto& job = found->second;
        if (job.external_operation == ExternalOperation::RuntimeSizing &&
            job.external_token == sizing_token) {
            job.external_operation = ExternalOperation::None;

            if (job.snapshot.version == sizing_version &&
                job.snapshot.state == InferenceJobState::Accepted) {
                if (deadlineExpired(job.snapshot.deadline_mono_ns,
                                    *clock_)) {
                    job.snapshot.state = InferenceJobState::Failed;
                    job.snapshot.stage = "DEADLINE_EXPIRED";
                    job.snapshot.last_error = StructuredError{
                        "inference", "INFERENCE_DEADLINE_EXPIRED",
                        "deadline expired during runtime sizing", false,
                        SideEffectState::NotApplicable};
                    ++job.snapshot.version;
                    emit(job, "FAILED", true);
                } else if (sizing_error) {
                    job.snapshot.state = InferenceJobState::Failed;
                    job.snapshot.stage = "RUNTIME_SIZING_FAILED";
                    job.snapshot.last_error = *sizing_error;
                    ++job.snapshot.version;
                    emit(job, "FAILED", true);
                } else {
                    job.remaining_work_units = required_work_units;
                    job.snapshot.state = InferenceJobState::Ready;
                    job.snapshot.stage = "READY";
                    ++job.snapshot.version;
                    emit(job, "READY");
                }
            }
        }
    }
    return {true, false, request.job_id, {}};
}


Status InferenceFramework::validateSubmitCaller(
    const InferenceRequest& request, const CallContext& call) {
    const auto caller = call.caller;
    if (caller != CallerModuleId::IntentRecognitionEngine &&
        caller != CallerModuleId::MemoryService &&
        caller != CallerModuleId::SubAgent) {
        return Status::Error("inference",
                             "INFERENCE_CALLER_MODULE_NOT_ALLOWED",
                             "caller may not submit inference");
    }
    if (request.admission.caller_module_id != caller) {
        return Status::Error("inference", "INFERENCE_ADMISSION_CALLER_MISMATCH",
                             "admission caller does not match IPC caller");
    }
    if (!hasHostModuleIdentity(call, call.caller) ||
        request.request_id.empty() ||
        request.admission.source_request_id != request.request_id ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        request.admission.principal_id.empty() ||
        call.principal_id_hash !=
            request.admission.principal_id ||
        request.admission.policy_snapshot_id.empty() ||
        !isValidTaskPriority(call.priority) ||
        !isValidTaskPriority(request.priority) ||
        !isValidTaskPriority(
            request.admission.granted_priority)) {
        return Status::Error("inference",
                             "INFERENCE_ADMISSION_IDENTITY_MISMATCH",
                             "call, request and admission identities must bind");
    }
    if (isHigherPriority(request.priority, call.priority)) {
        return Status::Error("inference",
                             "INFERENCE_CALL_PRIORITY_NOT_AUTHORIZED",
                             "requested priority exceeds call grant");
    }
    if (isHigherPriority(request.priority,
                         request.admission.granted_priority)) {
        return Status::Error("inference",
                             "INFERENCE_PRIORITY_NOT_AUTHORIZED",
                             "requested priority exceeds admission grant");
    }
    if (request.priority == TaskPriority::P0 &&
        (!request.admission.p0_authorization ||
         request.admission.signature_ref.empty())) {
        return Status::Error("inference",
                             "INFERENCE_P0_AUTHORIZATION_REQUIRED",
                             "P0 inference requires trusted authorization");
    }
    if (request.deadline_mono_ns <= 0 ||
        request.admission.deadline_mono_ns <= 0 ||
        call.deadline_mono_ns <= 0) {
        return Status::Error("inference",
                             "INFERENCE_DEADLINE_REQUIRED",
                             "request and admission require deadlines");
    }
    if (request.admission.deadline_mono_ns > 0 &&
        request.deadline_mono_ns >
            request.admission.deadline_mono_ns) {
        return Status::Error("inference",
                             "INFERENCE_DEADLINE_NOT_AUTHORIZED",
                             "job deadline exceeds admission deadline");
    }
    if (request.admission.deadline_mono_ns >
        call.deadline_mono_ns) {
        return Status::Error(
            "inference", "INFERENCE_CALL_DEADLINE_NOT_AUTHORIZED",
            "admission deadline exceeds caller control deadline");
    }
    if (request.priority == TaskPriority::P0 &&
        (request.admission.signature_ref.rfind(
             "trusted-safety:", 0) != 0 ||
         call.authorization_ref !=
             request.admission.signature_ref)) {
        return Status::Error(
            "inference", "INFERENCE_P0_AUTHORIZATION_REQUIRED",
            "P0 admission must bind authenticated safety authorization");
    }
    if (!request.admission.allowed_model_profiles.empty() &&
        std::find(request.admission.allowed_model_profiles.begin(),
                  request.admission.allowed_model_profiles.end(),
                  request.model) ==
            request.admission.allowed_model_profiles.end()) {
        return Status::Error("inference",
                             "INFERENCE_MODEL_NOT_AUTHORIZED",
                             "model is outside admission allowlist");
    }
    if (request.prompt_protocol_version.empty() ||
        request.prompt_protocol_version.size() > 128 ||
        request.model.empty() || request.model.size() > 256 ||
        request.session_id.size() > 256 ||
        request.parent_operation_id.size() > 512 ||
        request.trace_id.empty() || request.trace_id.size() > 512 ||
        request.admission.max_input_tokens == 0 ||
        request.admission.max_input_tokens > 65536 ||
        request.admission.max_output_tokens == 0 ||
        request.admission.max_output_tokens > 16384 ||
        request.prompt.size() > 1024U * 1024U ||
        request.prompt_segments.size() > 128 ||
        !validUtf8RuntimeText(request.prompt) ||
        !validUtf8RuntimeText(
            request.prompt_protocol_version) ||
        !validUtf8RuntimeText(request.model) ||
        !validUtf8RuntimeText(request.session_id) ||
        !validUtf8RuntimeText(request.parent_operation_id) ||
        !validUtf8RuntimeText(request.trace_id)) {
        return Status::Error(
            "inference", "INFERENCE_REQUEST_BOUNDS_INVALID",
            "request metadata, protocol, token budget or payload is out of bounds");
    }
    const bool sub_agent =
        caller == CallerModuleId::SubAgent;
    const bool has_parent_lineage =
        !request.parent_dispatch_id.empty() ||
        !request.parent_agent_id.empty() ||
        request.parent_agent_epoch != 0 ||
        !request.parent_lease_id.empty() ||
        request.parent_fencing_token != 0;
    if ((sub_agent &&
         (request.parent_operation_id.empty() ||
          request.parent_dispatch_id.empty() ||
          request.parent_dispatch_id.size() > 512 ||
          request.parent_agent_id.empty() ||
          request.parent_agent_id.size() > 256 ||
          request.parent_agent_epoch == 0 ||
          request.parent_lease_id.empty() ||
          request.parent_lease_id.size() > 512 ||
          request.parent_fencing_token == 0)) ||
        (!sub_agent && has_parent_lineage)) {
        return Status::Error(
            "inference", "INFERENCE_PARENT_LINEAGE_FIELDS_INVALID",
            "parent AgentLease fields must be complete only for SubAgent calls");
    }
    if (approximateTokens(request.prompt) >
        request.admission.max_input_tokens) {
        return Status::Error("inference",
                             "INFERENCE_INPUT_TOKEN_LIMIT",
                             "prompt exceeds admission token limit");
    }
    std::uint64_t segment_tokens = 0;
    for (const auto& segment : request.prompt_segments) {
        if (segment.segment_id.empty() || segment.digest.empty() ||
            segment.segment_id.size() > 256 ||
            segment.digest.size() > 256 ||
            segment.token_count == 0) {
            return Status::Error("inference",
                                 "INFERENCE_PROMPT_SEGMENT_INVALID",
                                 "prompt segments must be digest-bound");
        }
        segment_tokens += segment.token_count;
        if (segment_tokens >
            request.admission.max_input_tokens) {
            return Status::Error(
                "inference", "INFERENCE_PROMPT_SEGMENT_TOKEN_LIMIT",
                "prompt segment token total exceeds admission");
        }
    }
    return Status::Ok();
}


}  // namespace master_agent::inference
