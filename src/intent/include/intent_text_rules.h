#pragma once

/**
 * @file intent_text_rules.h
 * @brief Private deterministic text classification helpers.
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

bool isGreeting(const std::string& text) {
    return text == u8"你好" || text == u8"您好" || text == u8"谢谢" ||
           text == "hello" || text == "hi";
}

}  // namespace
}  // namespace master_agent::intent

