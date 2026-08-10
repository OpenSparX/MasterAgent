#pragma once

/**
 * @file kv_access_control.h
 * @brief Private inference-caller boundary validation.
 *
 * This header is private to KV Cache and is not part of the installed API.
 */

#include "master_agent/kv_cache/kv_cache_manager.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace master_agent::kv_cache {
namespace {

Status validateInferenceCaller(const CallContext& call) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::InferenceFramework) ||
        !isValidTaskPriority(call.priority) ||
        call.request_id.empty() || call.trace_id.empty() ||
        call.principal_id_hash.empty() ||
        call.deadline_mono_ns <= 0) {
        return Status::Error("kv_cache", "KV_CALLER_NOT_ALLOWED",
                             "only InferenceFramework may access KV cache");
    }
    return Status::Ok();
}

}  // namespace
}  // namespace master_agent::kv_cache

