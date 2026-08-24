#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "master_agent/common/types.h"
#include "master_agent/interaction/interaction_layer.h"

namespace master_agent::cloud {

struct CloudEscalationRequest {
    std::string request_id;
    std::string trace_id;
    std::string reason_code;
    TaskPriority priority = TaskPriority::P1;
    std::int64_t deadline_mono_ns = 0;
    std::uint32_t cloud_attempt = 0;
};

struct CloudArbitrationDecision {
    bool allowed = false;
    std::string reason_code;
};

struct CloudRequestEnvelope {
    std::string request_id;
    std::string trace_id;
    std::string normalized_text;
    std::string reason_code;
    std::vector<std::string> allowed_capabilities;
    std::string payload_digest;
};

struct CloudModelOutput {
    std::string raw_output;
    std::string output_digest;
    std::string runtime_tag;
};

struct CloudEvent {
    std::string event_type;
    std::string request_id;
    std::string trace_id;
    std::string reason_code;
    std::string outcome;
    std::string payload_digest;
    std::string output_digest;
    std::string runtime_tag;
};

struct CloudPolicy {
    bool enabled = false;
    bool consent_preapproved = false;
    std::int64_t minimum_remaining_deadline_ns = 1'000'000'000LL;
    std::vector<std::string> allowed_error_codes = {
        "INTENT_FIRST_INFERENCE_PROTOCOL_INVALID",
        "INTENT_MODEL_PROTOCOL_INVALID",
        "INTENT_MODEL_FALSE_EXECUTION_CLAIM"};
};

class ICloudArbiter {
public:
    virtual ~ICloudArbiter() = default;
    virtual CloudArbitrationDecision decide(
        const CloudEscalationRequest& request) const = 0;
};

class ICloudContextBuilder {
public:
    virtual ~ICloudContextBuilder() = default;
    virtual Result<CloudRequestEnvelope> build(
        const CloudEscalationRequest& escalation,
        const interaction::StandardRequest& request,
        const std::string& normalized_text) const = 0;
};

class ICloudModelRuntime {
public:
    virtual ~ICloudModelRuntime() = default;
    virtual Result<CloudModelOutput> infer(
        const CloudRequestEnvelope& request,
        const CallContext& call) const = 0;
};

std::shared_ptr<ICloudArbiter> createDeterministicCloudArbiter(
    std::shared_ptr<IRuntimeClock> clock, CloudPolicy policy);
std::shared_ptr<ICloudContextBuilder> createMinimalCloudContextBuilder();
std::shared_ptr<ICloudModelRuntime> createFakeCloudModelRuntime();

}  // namespace master_agent::cloud
