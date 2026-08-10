#pragma once

/**
 * @file exception_durability.h
 * @brief Private platform durability synchronization helper.
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

Status nativeDurabilitySync(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(
        path.wstring().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Error("exception", "EXM_DURABILITY_UNAVAILABLE",
                             "cannot open exception journal for fsync",
                             true, SideEffectState::Unknown);
    }
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed) {
        return Status::Error("exception", "EXM_DURABILITY_UNAVAILABLE",
                             "exception journal fsync failed", true,
                             SideEffectState::Unknown);
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::Error("exception", "EXM_DURABILITY_UNAVAILABLE",
                             "cannot open exception journal for fsync",
                             true, SideEffectState::Unknown);
    }
    const int synced = ::fsync(fd);
    const int saved_errno = errno;
    ::close(fd);
    if (synced != 0) {
        (void)saved_errno;
        return Status::Error("exception", "EXM_DURABILITY_UNAVAILABLE",
                             "exception journal fsync failed", true,
                             SideEffectState::Unknown);
    }
#endif
    return Status::Ok();
}

}  // namespace
}  // namespace master_agent::exception

