#pragma once

/**
 * @file exception_runtime_policy.h
 * @brief Private durability classification and compatibility-storage helpers.
 *
 * This header is private to Exception Management and is not part of the installed API.
 */

#include "master_agent/exception/exception_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace master_agent::exception {
namespace {

bool isD2OrD3(data_log::DurabilityClass durability) {
    return durability == data_log::DurabilityClass::D2Journaled ||
           durability == data_log::DurabilityClass::D3Fsynced;
}

Status durabilityUnknown(const std::string& message) {
    return Status::Error("exception", "EXM_DURABILITY_UNKNOWN", message,
                         true, SideEffectState::Unknown);
}

std::filesystem::path compatibilityStorage(
    const std::shared_ptr<IdGenerator>& ids) {
    static std::atomic<std::uint64_t> fallback_sequence{1};
    std::uint64_t process_id = 0;
#ifdef _WIN32
    process_id = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    process_id = static_cast<std::uint64_t>(::getpid());
#endif
    const auto time_nonce = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now()
            .time_since_epoch()
            .count());
    std::uint64_t random_nonce = 0;
    try {
        std::random_device entropy;
        random_nonce =
            (static_cast<std::uint64_t>(entropy()) << 32U) ^
            static_cast<std::uint64_t>(entropy());
    } catch (...) {
        random_nonce = time_nonce;
    }
    std::string suffix = std::to_string(process_id) + "-" +
                         std::to_string(time_nonce) + "-" +
                         std::to_string(random_nonce) + "-" +
                         std::to_string(
                             fallback_sequence.fetch_add(1));
    if (ids) {
        suffix += "-" + ids->next("exception-journal");
    }
    return std::filesystem::temp_directory_path() /
           ("master-agent-exception-" +
            secureDigest(suffix).substr(0, 20));
}

}  // namespace
}  // namespace master_agent::exception

