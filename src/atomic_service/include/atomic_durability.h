#pragma once

/**
 * @file atomic_durability.h
 * @brief Private platform durability synchronization helper.
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

Status nativeAtomicDurabilitySync(
    const std::filesystem::path& path) {
#ifdef _WIN32
    const auto handle = CreateFileW(
        path.wstring().c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
            "cannot open atomic WAL for durable flush", true,
            SideEffectState::Unknown);
    }
    const BOOL flushed = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!flushed) {
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
            "atomic WAL durable flush failed", true,
            SideEffectState::Unknown);
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
            "cannot open atomic WAL for durable flush", true,
            SideEffectState::Unknown);
    }
    const int synced = ::fsync(fd);
    const int saved_errno = errno;
    ::close(fd);
    if (synced != 0) {
        (void)saved_errno;
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
            "atomic WAL durable flush failed", true,
            SideEffectState::Unknown);
    }
#endif
    return Status::Ok();
}

}  // namespace
}  // namespace master_agent::atomic_service

