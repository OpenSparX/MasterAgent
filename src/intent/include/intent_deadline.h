#pragma once

/**
 * @file intent_deadline.h
 * @brief Private intent-node deadline helper.
 *
 * This header is private to Intent Recognition and is not part of the installed API.
 */

#include "master_agent/intent/intent_engine.h"

#include <algorithm>
#include <future>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace master_agent::intent {
namespace {

std::int64_t nodeDeadline(const IntentContext& context) {
    return context.deadline_mono_ns;
}

}  // namespace
}  // namespace master_agent::intent

