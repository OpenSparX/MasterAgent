#pragma once

/**
 * @file state_provider_registry.h
 * @brief Private Mock Provider and unique-domain Provider registry contracts.
 *
 * This header is private to Preprocess and is not part of the installed API.
 */

#include "preprocess_validation.h"

namespace master_agent::preprocess::detail {

/**
 * @brief Deterministic Provider used only by the reference Mock runtime.
 */
class HostTestStateProvider final : public IRuntimeStateProvider {
public:
    HostTestStateProvider(
        std::shared_ptr<IRuntimeClock> clock,
        StateDomain state_type,
        std::map<std::string, std::string> values);

    Result<StateCapability> getCapability() const override;
    Result<StateQueryResult> query(
        const StateQuery& request) const override;

private:
    std::shared_ptr<IRuntimeClock> clock_;
    StateDomain state_type_;
    std::map<std::string, std::string> values_;
};

/**
 * @brief Unique-domain Provider registry and direct state router.
 *
 * Design mapping: Sections 2.2, 2.4, 4 and 5.
 */
class VehicleStateCollector {
public:
    explicit VehicleStateCollector(
        std::vector<std::shared_ptr<IRuntimeStateProvider>> providers,
        std::shared_ptr<IRuntimeClock> clock);

    Result<std::vector<StateCapability>> getCapabilities() const;
    Result<StateQueryResult> query(
        const StateQuery& query) const;

private:
    // Section 2.4 ownership rule: one StateDomain has exactly one Provider.
    // A map makes the invariant explicit and gives query() deterministic,
    // direct routing instead of relying on registration order.
    std::map<StateDomain,
             std::shared_ptr<IRuntimeStateProvider>> providers_;
    // Construction remains non-throwing for compatibility with the runtime
    // composition root. Configuration errors are retained and returned by
    // both state-query APIs; the independent process() path remains usable.
    Status initialization_status_ = Status::Ok();
    std::shared_ptr<IRuntimeClock> clock_;
};

}  // namespace master_agent::preprocess::detail

