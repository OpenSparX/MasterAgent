#pragma once

/**
 * @file journal_io.h
 * @brief Private committed-line recovery and durable flush helpers.
 *
 * This header is private to Data Log and is not part of the installed API.
 */

#include "master_agent/data_log/data_log_service.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
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


namespace master_agent::data_log {
namespace {

Status readCommittedLines(const std::filesystem::path& path,
                          std::vector<std::string>& lines) {
    if (!std::filesystem::exists(path)) return Status::Ok();
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return Status::Error("data_log", "LOG_RECOVERY_READ_FAILED",
                             "cannot read active journal", true);
    }
    const std::string bytes((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    input.close();
    std::size_t offset = 0;
    std::size_t valid_bytes = 0;
    while (offset < bytes.size()) {
        const auto newline = bytes.find('\n', offset);
        if (newline == std::string::npos) {
            std::error_code resize_error;
            std::filesystem::resize_file(path, valid_bytes,
                                         resize_error);
            if (resize_error) {
                return Status::Error(
                    "data_log", "LOG_RECOVERY_TRUNCATE_FAILED",
                    "cannot truncate uncommitted journal tail", true);
            }
            break;
        }
        if (newline == offset) {
            return Status::Error(
                "data_log", "LOG_JOURNAL_INTEGRITY",
                "empty committed journal frame");
        }
        lines.push_back(bytes.substr(offset, newline - offset));
        valid_bytes = newline + 1;
        offset = valid_bytes;
    }
    return Status::Ok();
}

Status syncFilePath(const std::filesystem::path& path) {
#ifdef _WIN32
    const HANDLE handle =
        ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Error("data_log", "LOG_FSYNC_OPEN_FAILED",
                             "failed to open journal for durable flush", true);
    }
    const BOOL flushed = ::FlushFileBuffers(handle);
    ::CloseHandle(handle);
    if (!flushed) {
        return Status::Error("data_log", "LOG_FSYNC_FAILED",
                             "operating-system durable flush failed", true);
    }
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return Status::Error("data_log", "LOG_FSYNC_OPEN_FAILED",
                             "failed to open journal for durable flush", true);
    }
    const int synced = ::fsync(descriptor);
    ::close(descriptor);
    if (synced != 0) {
        return Status::Error("data_log", "LOG_FSYNC_FAILED",
                             "operating-system durable flush failed", true);
    }
#endif
    return Status::Ok();
}

// Platform durability adapters are external code.  Once bytes have been
// written, an exception from that boundary is an ambiguous commit and must be
// converted into the same fail-closed Status as an fsync error.
Status invokeDurabilitySync(
    const DataLogService::DurabilitySync& sync,
    const std::filesystem::path& path) {
    try {
        return sync(path);
    } catch (...) {
        return Status::Error(
            "data_log", "LOG_DURABILITY_SYNC_EXCEPTION",
            "durability adapter threw after the journal write", false,
            SideEffectState::Unknown);
    }
}

}  // namespace
}  // namespace master_agent::data_log

