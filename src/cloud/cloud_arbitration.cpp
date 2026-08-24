#include "master_agent/cloud/cloud_arbitration.h"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace master_agent::cloud {
namespace {

class DeterministicCloudArbiter final : public ICloudArbiter {
public:
    DeterministicCloudArbiter(
        std::shared_ptr<IRuntimeClock> clock, CloudPolicy policy)
        : clock_(std::move(clock)), policy_(std::move(policy)) {}

    CloudArbitrationDecision decide(
        const CloudEscalationRequest& request) const override {
        if (!policy_.enabled) return {false, "CLOUD_DISABLED"};
        if (request.priority == TaskPriority::P0)
            return {false, "CLOUD_P0_DENIED"};
        if (request.cloud_attempt != 0)
            return {false, "CLOUD_RETRY_DENIED"};
        if (!policy_.consent_preapproved)
            return {false, "CLOUD_CONSENT_REQUIRED"};
        if (std::find(policy_.allowed_error_codes.begin(),
                      policy_.allowed_error_codes.end(),
                      request.reason_code) ==
            policy_.allowed_error_codes.end())
            return {false, "CLOUD_REASON_NOT_ALLOWED"};
        if (request.deadline_mono_ns - clock_->monotonicNowNs() <
            policy_.minimum_remaining_deadline_ns)
            return {false, "CLOUD_DEADLINE_INSUFFICIENT"};
        return {true, "CLOUD_ALLOWED"};
    }

private:
    std::shared_ptr<IRuntimeClock> clock_;
    CloudPolicy policy_;
};

class MinimalCloudContextBuilder final : public ICloudContextBuilder {
public:
    Result<CloudRequestEnvelope> build(
        const CloudEscalationRequest& escalation,
        const interaction::StandardRequest&,
        const std::string& normalized_text) const override {
        if (normalized_text.empty() || normalized_text.size() > 4096) {
            return Result<CloudRequestEnvelope>::Failure(Status::Error(
                "cloud", "CLOUD_CONTEXT_REJECTED",
                "normalized current turn is empty or too large"));
        }
        CloudRequestEnvelope envelope;
        envelope.request_id = escalation.request_id;
        envelope.trace_id = escalation.trace_id;
        envelope.normalized_text = normalized_text;
        envelope.reason_code = escalation.reason_code;
        envelope.allowed_capabilities = {"com_sgm_agent_trip_plan"};
        envelope.payload_digest = secureDigest(
            envelope.request_id + "|" + envelope.trace_id + "|" +
            envelope.reason_code + "|" + envelope.normalized_text +
            "|com_sgm_agent_trip_plan");
        return Result<CloudRequestEnvelope>::Success(std::move(envelope));
    }
};

class FakeCloudModelRuntime final : public ICloudModelRuntime {
public:
    Result<CloudModelOutput> infer(
        const CloudRequestEnvelope& request,
        const CallContext&) const override {
        const bool trip = request.normalized_text.find(u8"行程") !=
                              std::string::npos ||
                          request.normalized_text.find(u8"机场") !=
                              std::string::npos ||
                          request.normalized_text.find("trip") !=
                              std::string::npos;
        nlohmann::json decision;
        if (trip) {
            decision = {
                {"outcome", "PLAN"},
                {"reply", u8"已生成端云协同的行程规划任务。"},
                {"nodes", nlohmann::json::array({
                    {{"node_id", "cloud-trip-1"},
                     {"executor", "agent_dispatch"},
                     {"action", "com_sgm_agent_trip_plan"},
                     {"target_agent", "trip-agent"},
                     {"params", {{"request", request.normalized_text}}}}})}};
        } else {
            decision = {{"outcome", "REPLY"},
                        {"reply", u8"云端已收到请求，但当前假云端只演示行程规划。"}};
        }
        CloudModelOutput output;
        output.raw_output = decision.dump();
        output.runtime_tag = "fake-cloud";
        output.output_digest = secureDigest(
            request.payload_digest + "|" + output.raw_output +
            "|" + output.runtime_tag);
        return Result<CloudModelOutput>::Success(std::move(output));
    }
};

}  // namespace

std::shared_ptr<ICloudArbiter> createDeterministicCloudArbiter(
    std::shared_ptr<IRuntimeClock> clock, CloudPolicy policy) {
    return std::make_shared<DeterministicCloudArbiter>(
        std::move(clock), std::move(policy));
}

std::shared_ptr<ICloudContextBuilder> createMinimalCloudContextBuilder() {
    return std::make_shared<MinimalCloudContextBuilder>();
}

std::shared_ptr<ICloudModelRuntime> createFakeCloudModelRuntime() {
    return std::make_shared<FakeCloudModelRuntime>();
}

}  // namespace master_agent::cloud
