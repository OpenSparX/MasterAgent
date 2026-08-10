#pragma once

/**
 * @file kv_token_accounting.h
 * @brief Private prompt-segment token accounting.
 *
 * This header is private to KV Cache and is not part of the installed API.
 */

#include "master_agent/kv_cache/kv_cache_manager.h"

#include <algorithm>
#include <numeric>
#include <utility>

namespace master_agent::kv_cache {
namespace {

std::uint32_t tokenCount(const std::vector<PromptSegment>& segments,
                         std::size_t count) {
    std::uint32_t tokens = 0;
    for (std::size_t i = 0; i < std::min(count, segments.size()); ++i) {
        tokens += segments[i].token_count;
    }
    return tokens;
}

}  // namespace
}  // namespace master_agent::kv_cache

