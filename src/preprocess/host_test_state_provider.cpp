/**
 * @file host_test_state_provider.cpp
 * @brief Implements deterministic state values for the Mock runtime only.
 */

#include "include/state_provider_registry.h"

#include <algorithm>
#include <utility>

namespace master_agent::preprocess::detail {

HostTestStateProvider::HostTestStateProvider(
    std::shared_ptr<IRuntimeClock> clock,
    StateDomain state_type,
    std::map<std::string, std::string> values)
    : clock_(std::move(clock)),
      state_type_(state_type),
      values_(std::move(values)) {}

Result<StateCapability>
HostTestStateProvider::getCapability() const {
    StateCapability capability;
    capability.state_type = state_type_;
    const std::vector<std::string> preferred =
        state_type_ == StateDomain::Vehicle
            ? std::vector<std::string>{
                  "speed_kmh", "engine_on",
                  "is_parked", "battery_soc"}
            : std::vector<std::string>{
                  "outside_temperature", "weather"};
    for (const auto& field : preferred) {
        if (values_.count(field) != 0) {
            capability.fields.push_back(field);
        }
    }
    for (const auto& [field, unused] : values_) {
        (void)unused;
        if (std::find(
                capability.fields.begin(),
                capability.fields.end(),
                field) == capability.fields.end()) {
            capability.fields.push_back(field);
        }
    }
    return Result<StateCapability>::Success(
        std::move(capability));
}

Result<StateQueryResult>
HostTestStateProvider::query(
    const StateQuery& request) const {
    StateQueryResult result;
    result.timestamp_utc_ms =
        clock_ ? clock_->utcNowMs() : 0;
    for (const auto& field : request.fields) {
        const auto found = values_.find(field);
        if (found == values_.end()) {
            result.missing_fields.push_back(field);
        } else {
            result.values.emplace(*found);
        }
    }
    result.success = result.missing_fields.empty();
    if (!result.success) {
        result.error_message =
            "one or more requested fields are unavailable";
    }
    return Result<StateQueryResult>::Success(
        std::move(result));
}

}  // namespace master_agent::preprocess::detail
