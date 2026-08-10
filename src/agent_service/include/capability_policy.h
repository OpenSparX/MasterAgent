#pragma once

/**
 * @file capability_policy.h
 * @brief Private capability allowlist and producer epoch helpers.
 *
 * This header is private to Agent Service and is not part of the installed API.
 */

#include "master_agent/agent_service/agent_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <random>
#include <set>
#include <utility>

namespace master_agent::agent_service {
namespace {

// capability allowlist.  Model/rule output is never allowed to mint its own
// capability grant.
const std::set<std::string>& productionCapabilityAllowlist() {
    static const std::set<std::string> allowed{
        "com_sgm_service_climate_setAirCirculationMode",
        "com_sgm_service_climate_setAutoFanSpeed",
        "com_sgm_agent_trip_plan"};
    return allowed;
}

std::uint64_t nextProducerEpoch() {
    static std::atomic<std::uint64_t> process_sequence{1};
    const auto time_bits = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count());
    std::uint64_t random_bits = 0;
    try {
        std::random_device random;
        random_bits =
            (static_cast<std::uint64_t>(random()) << 32U) ^
            static_cast<std::uint64_t>(random());
    } catch (...) {
        random_bits = 0;
    }
    auto epoch =
        time_bits ^ random_bits ^
        (process_sequence.fetch_add(1) *
         0x9E3779B97F4A7C15ULL);
    if (epoch == 0) epoch = 1;
    return epoch;
}

}  // namespace
}  // namespace master_agent::agent_service

