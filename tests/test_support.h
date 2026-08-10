/**
 * @file test_support.h
 * @brief Provides deterministic clocks, identities, fixtures, and assertions.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace master_agent::test_support {

inline void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ScopedTempDirectory {
public:
    explicit ScopedTempDirectory(const std::string& prefix) {
        static std::atomic<std::uint64_t> sequence{1};
        const auto token = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path_ = std::filesystem::temp_directory_path() /
                (prefix + "-" + std::to_string(token) + "-" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }

    ~ScopedTempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

}  // namespace master_agent::test_support
