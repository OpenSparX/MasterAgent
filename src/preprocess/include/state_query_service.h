#pragma once

/**
 * @file state_query_service.h
 * @brief Private runtime state-query service contract.
 *
 * This header is private to Preprocess and is not part of the installed API.
 */

#include "state_provider_registry.h"

namespace master_agent::preprocess::detail {

/**
 * @brief State-query validation and orchestration component.
 *
 * Design mapping: Sections 2.4, 3.1, 3.3 and 4.
 */
class StateQueryServiceImpl final : public IStateQuery {
public:
    StateQueryServiceImpl(
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<VehicleStateCollector> collector);

    Result<StateQueryResult> queryRuntimeState(
        const StateQuery& query,
        const CallContext& call) const override;

    Result<std::vector<StateCapability>> getCapabilities(
        const CallContext& call) const override;

private:
    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<VehicleStateCollector> collector_;
};

}  // namespace master_agent::preprocess::detail

