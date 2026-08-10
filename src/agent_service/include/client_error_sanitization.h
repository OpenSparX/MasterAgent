#pragma once

/**
 * @file client_error_sanitization.h
 * @brief Private client-safe error metadata helpers.
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

bool safeMetadataToken(const std::string& value,
                       std::size_t max_size = 128) {
    return !value.empty() && value.size() <= max_size &&
           std::all_of(
               value.begin(), value.end(),
               [](unsigned char character) {
                   return std::isalnum(character) != 0 ||
                          character == '_' || character == '-' ||
                          character == '.';
               });
}

std::string normalizedErrorCode(const std::string& code) {
    return safeMetadataToken(code)
               ? code
               : "UNTRUSTED_MODULE_FAILURE";
}

std::string normalizedErrorDomain(
    const std::string& domain,
    const std::string& fallback) {
    if (safeMetadataToken(domain)) return domain;
    return safeMetadataToken(fallback) ? fallback : "agent_service";
}

std::string safeClientErrorMessage() {
    return "request could not be completed";
}

}  // namespace
}  // namespace master_agent::agent_service

