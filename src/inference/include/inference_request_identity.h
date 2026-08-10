#pragma once

/**
 * @file inference_request_identity.h
 * @brief Private inference request and runtime compatibility identity helpers.
 *
 * This header is private to Inference and is not part of the installed API.
 */

#include "master_agent/inference/inference_framework.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace master_agent::inference {
namespace {

std::string requestDigest(const InferenceRequest& request) {
    nlohmann::json encoded{
        {"request_id", request.request_id},
        {"parent_operation_id", request.parent_operation_id},
        {"session_id", request.session_id},
        {"prompt", request.prompt},
        {"prompt_digest", request.prompt_digest},
        {"prompt_protocol_version", request.prompt_protocol_version},
        {"inference_phase", request.inference_phase},
        {"model", request.model},
        {"adapter", request.adapter},
        {"kv_reuse_policy", request.kv_reuse_policy},
        {"priority", toString(request.priority)},
        {"deadline_mono_ns", request.deadline_mono_ns},
        {"parent_dispatch_id", request.parent_dispatch_id},
        {"parent_agent_id", request.parent_agent_id},
        {"parent_agent_epoch", request.parent_agent_epoch},
        {"parent_lease_id", request.parent_lease_id},
        {"parent_fencing_token", request.parent_fencing_token},
        {"reality", request.reality},
        {"trace_id", request.trace_id},
        {"admission",
         {{"principal_id", request.admission.principal_id},
          {"caller_module_id",
           toString(request.admission.caller_module_id)},
          {"source_request_id",
           request.admission.source_request_id},
          {"granted_priority",
           toString(request.admission.granted_priority)},
          {"p0_authorization",
           request.admission.p0_authorization},
          {"policy_snapshot_id",
           request.admission.policy_snapshot_id},
          {"allowed_model_profiles",
           request.admission.allowed_model_profiles},
          {"max_input_tokens", request.admission.max_input_tokens},
          {"max_output_tokens", request.admission.max_output_tokens},
          {"deadline_mono_ns",
           request.admission.deadline_mono_ns},
          {"signature_ref", request.admission.signature_ref}}}};
    encoded["prompt_segments"] = nlohmann::json::array();
    for (const auto& segment : request.prompt_segments) {
        encoded["prompt_segments"].push_back(
            {{"segment_id", segment.segment_id},
             {"digest", segment.digest},
             {"token_count", segment.token_count}});
    }
    return secureDigest(encoded.dump());
}

// KV reuse is valid only for an identical token/cache ABI. The reference runtime uses
// fixed mock component versions, but still binds every compatibility axis so
// This explicit composition prevents callers from keying entries by model name
// while omitting runtime compatibility fields.
// An integrated model runtime replaces these fixed component IDs with signed
// ModelArtifact/Tokenizer/Backend/CacheLayout manifest values.
std::string runtimeFingerprint(
    const InferenceRequest& request) {
    const nlohmann::json manifest{
        {"model_artifact", request.model},
        {"prompt_protocol", request.prompt_protocol_version},
        {"adapter", request.adapter},
        {"runtime", "mock-runtime"},
        {"backend", "mock-backend-v1"},
        {"tokenizer", "mock-tokenizer-v1"},
        {"quantization", "none"},
        {"cache_layout", "mock-kv-layout-v1"}};
    return secureDigest(manifest.dump());
}

}  // namespace
}  // namespace master_agent::inference

