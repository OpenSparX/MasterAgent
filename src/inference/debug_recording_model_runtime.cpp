#include "master_agent/inference/debug_recording_model_runtime.h"

#include <nlohmann/json.hpp>

namespace master_agent::inference {

DebugRecordingModelRuntime::DebugRecordingModelRuntime(
    std::shared_ptr<IModelRuntime> delegate, std::filesystem::path artifact_path)
    : delegate_(std::move(delegate)), artifact_path_(std::move(artifact_path)) {
    std::filesystem::create_directories(artifact_path_.parent_path());
}

std::string DebugRecordingModelRuntime::runtimeTag() const {
    return delegate_ ? delegate_->runtimeTag() : "debug-runtime-unavailable";
}

std::uint32_t DebugRecordingModelRuntime::requiredWorkUnits(
    const InferenceRequest& request) const {
    return delegate_ ? delegate_->requiredWorkUnits(request) : 1;
}

bool DebugRecordingModelRuntime::supportsStreaming() const {
    return delegate_ && delegate_->supportsStreaming();
}

void DebugRecordingModelRuntime::append(const nlohmann::json& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream output(artifact_path_, std::ios::binary | std::ios::app);
    output << record.dump() << '\n';
    output.flush();
}

void DebugRecordingModelRuntime::recordInput(const InferenceRequest& request) {
    append({{"debug_schema", "masteragent.local-debug/v1"},
            {"event_type", "MODEL_INPUT"}, {"stage", "local_inference"},
            {"trace_id", request.trace_id}, {"request_id", request.request_id},
            {"job_id", request.job_id}, {"phase", request.inference_phase},
            {"payload", {{"model", request.model},
                         {"protocol_version", request.prompt_protocol_version},
                         {"prompt", request.prompt},
                         {"prompt_digest", request.prompt_digest}}}});
}

void DebugRecordingModelRuntime::recordOutput(
    const InferenceRequest& request, const Result<InferenceOutput>& output) {
    nlohmann::json payload{{"model", request.model}};
    std::string status = "FAILED";
    if (output.status.ok && output.value) {
        status = "SUCCEEDED";
        payload["raw_output"] = output.value->raw_output;
        payload["finish_reason"] = output.value->finish_reason;
        payload["runtime"] = output.value->runtime_backend;
        payload["prompt_tokens"] = output.value->prompt_token_count;
        payload["generated_tokens"] = output.value->generated_token_count;
        payload["total_latency_ms"] = output.value->total_latency_ms;
    } else {
        payload["error_code"] = output.status.error.code;
    }
    append({{"debug_schema", "masteragent.local-debug/v1"},
            {"event_type", "MODEL_OUTPUT"}, {"stage", "local_inference"},
            {"status", status}, {"trace_id", request.trace_id},
            {"request_id", request.request_id}, {"job_id", request.job_id},
            {"phase", request.inference_phase}, {"payload", std::move(payload)}});
}

Result<InferenceOutput> DebugRecordingModelRuntime::infer(
    const InferenceRequest& request, const RuntimeInvocationSeal& seal) {
    recordInput(request);
    if (!delegate_) return Result<InferenceOutput>::Failure(
        Status::Error("inference", "DEBUG_RUNTIME_DELEGATE_MISSING", "model runtime is unavailable"));
    auto result = delegate_->infer(request, seal);
    recordOutput(request, result);
    return result;
}

Result<InferenceOutput> DebugRecordingModelRuntime::inferStream(
    const InferenceRequest& request, const RuntimeInvocationSeal& seal,
    const InferenceStreamSink& sink) {
    recordInput(request);
    if (!delegate_) return Result<InferenceOutput>::Failure(
        Status::Error("inference", "DEBUG_RUNTIME_DELEGATE_MISSING", "model runtime is unavailable"));
    auto result = delegate_->inferStream(request, seal, sink);
    recordOutput(request, result);
    return result;
}

}  // namespace master_agent::inference
