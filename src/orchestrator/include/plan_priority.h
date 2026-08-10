#pragma once

/**
 * @file plan_priority.h
 * @brief Private plan priority aggregation helper.
 *
 * This header is private to Task Orchestration and is not part of the installed API.
 */

#include "master_agent/orchestrator/orchestrator.h"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace master_agent::orchestrator {
namespace {

TaskPriority highestPriority(const std::vector<DAGNode>& nodes) {
    TaskPriority result = TaskPriority::P2;
    for (const auto& node : nodes) {
        if (isHigherPriority(node.base_priority, result)) {
            result = node.base_priority;
        }
    }
    return result;
}

}  // namespace
}  // namespace master_agent::orchestrator
