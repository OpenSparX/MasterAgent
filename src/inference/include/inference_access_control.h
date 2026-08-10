#pragma once

/**
 * @file inference_access_control.h
 * @brief Private caller and job-owner control rules.
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

bool canQuery(CallerModuleId caller) {
    return caller == CallerModuleId::IntentRecognitionEngine ||
           caller == CallerModuleId::MemoryService ||
           caller == CallerModuleId::SubAgent ||
           caller == CallerModuleId::AgentService ||
           caller == CallerModuleId::AgentDispatch;
}

bool canCancel(CallerModuleId caller) {
    return canQuery(caller);
}

bool canRequestPreempt(CallerModuleId caller) {
    return caller == CallerModuleId::AgentService ||
           caller == CallerModuleId::AgentDispatch;
}

// Owner isolation: Intent, Memory and SubAgent jobs are separate security
// domains even when request/trace/principal happen to be identical.
// AgentService is the governed cross-owner control plane. AgentDispatch can
// control only child jobs owned by SubAgent.
bool ownerControlAllowed(CallerModuleId caller,
                         CallerModuleId owner) {
    return caller == owner ||
           caller == CallerModuleId::AgentService ||
           (caller == CallerModuleId::AgentDispatch &&
            owner == CallerModuleId::SubAgent);
}

}  // namespace
}  // namespace master_agent::inference

