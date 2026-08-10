#pragma once

/**
 * @file dispatch_access_control.h
 * @brief Private caller-access rules for Agent Dispatch query operations.
 *
 * This header is internal to the Agent Dispatch module and is not installed as
 * part of the public SDK. Keeping caller authorization separate from dispatch
 * execution makes the trust boundary explicit during code review.
 */

#include "master_agent/agent_dispatch/agent_dispatch.h"

namespace master_agent::agent_dispatch {
namespace {

bool canQuery(CallerModuleId caller) {
    return caller == CallerModuleId::TaskOrchestrationEngine ||
           caller == CallerModuleId::AgentService;
}

}  // namespace
}  // namespace master_agent::agent_dispatch

