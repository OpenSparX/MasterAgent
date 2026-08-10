/**
 * @file model_runtime.cpp
 * @brief Implements stable runtime digests and the deterministic model mock.
 */

#include "include/inference_access_control.h"
#include "include/inference_request_identity.h"
#include "include/inference_runtime_limits.h"

namespace master_agent::inference {

// Stable, parent-bound identity for SubAgent model work.
std::string inferenceChildIdempotencyKey(
    const std::string& parent_dispatch_id,
    const std::string& child_job_id) {
    return secureDigest(
        "inference-child|" + parent_dispatch_id + "|" +
        child_job_id);
}

std::string runtimeInvocationDigest(
    const RuntimeInvocationSeal& seal) {
    const nlohmann::json encoded{
        {"job_id", seal.job_id},
        {"attempt_id", seal.attempt_id},
        {"operation_id", seal.operation_id},
        {"replica_id", seal.replica_id},
        {"replica_epoch", seal.replica_epoch},
        {"lease_id", seal.lease_id},
        {"fencing_token", seal.fencing_token},
        {"control_epoch", seal.control_epoch},
        {"prompt_digest", seal.prompt_digest},
        {"model_digest", seal.model_digest},
        {"deadline_mono_ns", seal.deadline_mono_ns}};
    return secureDigest(encoded.dump());
}

std::string inferenceOutputDigest(const InferenceOutput& output) {
    const nlohmann::json encoded{
        {"raw_output", output.raw_output},
        {"finish_reason", output.finish_reason},
        {"model_id", output.model_id},
        {"model_digest", output.model_digest},
        {"job_id", output.job_id},
        {"operation_id", output.operation_id},
        {"replica_id", output.replica_id},
        {"replica_epoch", output.replica_epoch},
        {"lease_id", output.lease_id},
        {"fencing_token", output.fencing_token},
        {"control_epoch", output.control_epoch},
        {"attempt_id", output.attempt_id},
        {"prompt_digest", output.prompt_digest},
        {"invocation_id", output.invocation_id},
        {"prompt_token_count", output.prompt_token_count},
        {"generated_token_count", output.generated_token_count},
        {"total_latency_ms", output.total_latency_ms},
        {"runtime_backend", output.runtime_backend},
        {"reality", output.reality}};
    return secureDigest(encoded.dump());
}

MockModelRuntime::MockModelRuntime(std::uint32_t work_units)
    : work_units_(std::max<std::uint32_t>(1, work_units)) {}

std::uint32_t MockModelRuntime::requiredWorkUnits(
    const InferenceRequest&) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return work_units_;
}

Result<InferenceOutput> MockModelRuntime::infer(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!next_error_.empty()) {
        const auto code = std::move(next_error_);
        next_error_.clear();
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", code, "mock runtime injected failure", true));
    }
    if (seal.job_id != request.job_id ||
        seal.attempt_id.empty() || seal.operation_id.empty() ||
        seal.replica_id.empty() || seal.replica_epoch == 0 ||
        seal.lease_id.empty() || seal.fencing_token == 0 ||
        seal.prompt_digest != request.prompt_digest ||
        seal.model_digest !=
            secureDigest(request.model + "|" + runtimeTag()) ||
        seal.deadline_mono_ns != request.deadline_mono_ns ||
        seal.invocation_id != runtimeInvocationDigest(seal)) {
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_INVOCATION_INVALID",
            "runtime invocation seal is incomplete or inconsistent"));
    }

    // Mock behavior is derived only from the current USER segment. Looking
    // at the whole Prompt would let prior MEMORY or Evidence text alter the
    // current turn's simulated decision.
    std::string_view current_user = request.prompt;
    const auto first_protocol =
        request.prompt.find("\nPROTOCOL:\n");
    const auto user_marker = request.prompt.rfind(
        "\nUSER:\n", first_protocol);
    if (user_marker != std::string::npos) {
        const auto begin = user_marker + 7U;
        const auto end = first_protocol;
        current_user = std::string_view(request.prompt).substr(
            begin, end == std::string::npos
                       ? std::string::npos
                       : end - begin);
    }

    nlohmann::json output;
    if (current_user.find(u8"行程") != std::string::npos ||
        current_user.find(u8"规划") != std::string::npos ||
        current_user.find("trip") != std::string::npos) {
        output = {
            {"outcome", "PLAN"},
            {"reply", u8"正在为你规划行程。"},
            {"nodes",
             {{{"node_id", "T1"},
               {"executor", "agent_dispatch"},
               {"action", "com_sgm_agent_trip_plan"},
               {"target_agent", "trip-agent"},
               {"params", {{"request", request.request_id}}}}}}};
    } else {
        output = {{"outcome", "REPLY"},
                  {"reply", u8"我已经理解你的请求。"}};
    }
    const auto contains = [current_user](std::string_view marker) {
        return current_user.find(marker) != std::string::npos;
    };
    const bool asks_for_trip_utf8 =
        contains("itinerary") ||
        contains("\xE8\xA1\x8C\xE7\xA8\x8B") ||
        contains("\xE8\xA7\x84\xE5\x88\x92");
    const bool asks_for_clarification =
        contains("clarify") || contains("ambiguous") ||
        contains("\xE6\xBE\x84\xE6\xB8\x85");
    const bool is_unsupported =
        contains("unsupported") ||
        contains("\xE4\xB8\x8D\xE6\x94\xAF\xE6\x8C\x81");
    if (is_unsupported) {
        output = {
            {"outcome", "FAIL"},
            {"reason_code", "unsupported"},
            {"reply", "当前能力暂不支持该请求。"}};
    } else if (asks_for_clarification) {
        output = {
            {"outcome", "ASK"},
            {"slot", "preference"},
            {"reply", "请确认你希望采用哪一种偏好。"}};
    } else if (asks_for_trip_utf8 &&
        output.value("outcome", std::string{}) != "PLAN") {
        output = {
            {"outcome", "PLAN"},
            {"reply", "正在为你规划行程。"},
            {"nodes",
             {{{"node_id", "T1"},
               {"executor", "agent_dispatch"},
               {"action", "com_sgm_agent_trip_plan"},
               {"target_agent", "trip-agent"},
               {"params", {{"request", request.request_id}}}}}}};
    }
    const bool asks_for_context =
        contains("use memory") || contains("using memory") ||
        contains("history") || contains("preference") ||
        contains("\xE5\x8E\x86\xE5\x8F\xB2") ||
        contains("\xE5\x81\x8F\xE5\xA5\xBD");
    const bool asks_for_full_query_matrix =
        contains("full query matrix");
    if (request.admission.caller_module_id ==
            CallerModuleId::IntentRecognitionEngine &&
        request.inference_phase == "FIRST_INFERENCE") {
        if (asks_for_full_query_matrix) {
            // This deterministic test vector exercises every read-only route
            // defined by the intent design in one joined QUERY_BATCH. The
            // owning modules, not this model mock, produce all evidence.
            output = {
                {"branch", "QUERY_BATCH"},
                {"queries",
                 {{{"query_id", "Q1"},
                   {"capability", "preprocess.queryRuntimeState"},
                   {"arguments",
                    {{"state_type", "VEHICLE"},
                     {"fields", {"speed_kmh"}}}},
                   {"read_only", true}},
                  {{"query_id", "Q2"},
                   {"capability", "memory.getContext"},
                   {"arguments",
                    {{"scope", "short_term"},
                     {"request_id", request.request_id}}},
                   {"read_only", true}},
                  {{"query_id", "Q3"},
                   {"capability", "skill.matchSkill"},
                   {"arguments",
                    {{"query", std::string(current_user)},
                     {"top_k", 3}}},
                   {"read_only", true}},
                  {{"query_id", "Q4"},
                   {"capability", "atomic.queryReadOnly"},
                   {"arguments",
                    {{"tool_name", "com_sgm_service_weather_getCurrent"},
                     {"arguments", nlohmann::json::object()}}},
                   {"read_only", true}}}}};
        } else if (asks_for_context) {
            output = {
                {"branch", "QUERY_BATCH"},
                {"queries",
                 {{{"query_id", "Q1"},
                   {"capability", "memory.getContext"},
                   {"arguments",
                    {{"scope", "short_term"},
                     {"request_id", request.request_id}}},
                   {"read_only", true}}}}};
        } else {
            output = {{"branch", "NO_QUERY"},
                      {"final", std::move(output)}};
        }
    }

    InferenceOutput result;
    result.raw_output = output.dump();
    result.finish_reason = "stop";
    result.model_id = request.model;
    result.model_digest = seal.model_digest;
    result.job_id = seal.job_id;
    result.operation_id = seal.operation_id;
    result.replica_id = seal.replica_id;
    result.replica_epoch = seal.replica_epoch;
    result.lease_id = seal.lease_id;
    result.fencing_token = seal.fencing_token;
    result.control_epoch = seal.control_epoch;
    result.attempt_id = seal.attempt_id;
    result.prompt_digest = seal.prompt_digest;
    result.invocation_id = seal.invocation_id;
    result.prompt_token_count = approximateTokens(request.prompt);
    result.generated_token_count =
        approximateTokens(result.raw_output);
    result.total_latency_ms = work_units_;
    result.reality = request.reality;
    result.output_digest = inferenceOutputDigest(result);
    MockModelInvocation invocation;
    invocation.sequence = ++invocation_sequence_;
    invocation.request_id = request.request_id;
    invocation.job_id = request.job_id;
    invocation.inference_phase = request.inference_phase;
    invocation.prompt_protocol_version =
        request.prompt_protocol_version;
    invocation.prompt = request.prompt;
    invocation.prompt_digest = request.prompt_digest;
    invocation.raw_output = result.raw_output;
    invocation.output_digest = result.output_digest;
    invocation.reality = result.reality;
    invocations_.push_back(std::move(invocation));
    return Result<InferenceOutput>::Success(std::move(result));
}

void MockModelRuntime::setWorkUnits(std::uint32_t units) {
    std::lock_guard<std::mutex> lock(mutex_);
    work_units_ = std::max<std::uint32_t>(1, units);
}

void MockModelRuntime::failNext(std::string error_code) {
    std::lock_guard<std::mutex> lock(mutex_);
    next_error_ = std::move(error_code);
}

std::vector<MockModelInvocation>
MockModelRuntime::invocations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return invocations_;
}


}  // namespace master_agent::inference
