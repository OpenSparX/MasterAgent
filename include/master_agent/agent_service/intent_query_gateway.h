#pragma once

/**
 * @file intent_query_gateway.h
 * @brief Defines the Agent Service proxy used for intent-time state queries.
 */

#include <memory>

#include "master_agent/preprocess/preprocess_engine.h"

namespace master_agent::agent_service {

/**
 * @brief Creates the documented Intent -> Agent Service -> Preprocess route.
 *
 * Preprocess keeps Agent Service as its only public caller. The returned proxy
 * accepts authenticated Intent Engine calls, preserves request authority and
 * deadlines, and forwards them with the Agent Service module identity.
 */
std::shared_ptr<preprocess::IStateQuery>
createIntentStateQueryGateway(
    std::shared_ptr<preprocess::IStateQuery> preprocess);

}  // namespace master_agent::agent_service
