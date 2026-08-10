#pragma once

/**
 * @file atomic_state_rules.h
 * @brief Private terminal-state rules for atomic executions.
 *
 * This header is private to Atomic Service and is not part of the installed API.
 */

#include "master_agent/atomic_service/atomic_service.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <set>
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

namespace master_agent::atomic_service {
namespace {

bool terminal(AtomicExecutionState state) {
    return state == AtomicExecutionState::Succeeded ||
           state == AtomicExecutionState::Failed ||
           state == AtomicExecutionState::Cancelled ||
           state == AtomicExecutionState::Unknown;
}

}  // namespace
}  // namespace master_agent::atomic_service

