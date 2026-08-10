#pragma once

/**
 * @file inference_runtime_limits.h
 * @brief Private token estimation, UTF-8 validation, and output bounds.
 *
 * This header is private to Inference and is not part of the installed API.
 */

#include "master_agent/inference/inference_framework.h"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace master_agent::inference {
namespace {

std::uint32_t approximateTokens(const std::string& text) {
    return static_cast<std::uint32_t>((text.size() + 3) / 4);
}

bool validUtf8RuntimeText(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first =
            static_cast<unsigned char>(text[offset]);
        if (first <= 0x7fU) {
            if ((first < 0x20U && first != '\t' &&
                 first != '\n' && first != '\r') ||
                first == 0x7fU) {
                return false;
            }
            ++offset;
            continue;
        }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            length = 2;
            codepoint = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            length = 3;
            codepoint = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            length = 4;
            codepoint = first & 0x07U;
        } else {
            return false;
        }
        if (offset + length > text.size()) return false;
        for (std::size_t i = 1; i < length; ++i) {
            const auto continuation =
                static_cast<unsigned char>(text[offset + i]);
            if ((continuation & 0xc0U) != 0x80U) return false;
            codepoint =
                (codepoint << 6U) | (continuation & 0x3fU);
        }
        if ((length == 3 &&
             (codepoint < 0x800U ||
              (codepoint >= 0xd800U &&
               codepoint <= 0xdfffU))) ||
            (length == 4 &&
             (codepoint < 0x10000U ||
              codepoint > 0x10ffffU)) ||
            (codepoint >= 0x80U && codepoint <= 0x9fU)) {
            return false;
        }
        offset += length;
    }
    return true;
}

std::size_t maxRuntimeOutputBytes(
    std::uint32_t max_output_tokens) {
    constexpr std::uint64_t kGlobalOutputCap =
        1024ULL * 1024ULL;
    constexpr std::uint64_t kEnvelopeAllowance = 4096ULL;
    constexpr std::uint64_t kWorstCaseBytesPerToken = 16ULL;
    return static_cast<std::size_t>(std::min(
        kGlobalOutputCap,
        kEnvelopeAllowance +
            static_cast<std::uint64_t>(max_output_tokens) *
                kWorstCaseBytesPerToken));
}

}  // namespace
}  // namespace master_agent::inference

