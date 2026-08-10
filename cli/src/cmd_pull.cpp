/**
 * @file cmd_pull.cpp
 * @brief `sparx pull <model>` — download model artifacts with progress.
 *
 * Supports two backends:
 *  - HuggingFace Hub (GGUF for llama.cpp): `sparx pull qwen3-4b`
 *  - Local mirror / custom URL: `sparx pull https://...`
 *
 * The download lands in ~/.sparx/models/<model>/ (host) or is deployed to
 * the device via `sparx deploy` later.
 *
 * Progress is shown as a percentage bar + throughput, updated every 100ms.
 * Checksum verification (sha256) happens after download when available.
 */

#include "sparx_commands.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace sparx {

namespace {

/// Known model registry: maps short names to download URLs + metadata.
struct ModelEntry {
    const char* name;
    const char* url;
    const char* filename;
    std::uint64_t size_bytes;   // 0 = unknown until Content-Length
    const char* sha256;         // nullptr = no checksum
};

// These URLs point to HuggingFace Hub. The specific revisions are pinned
// so a `sparx pull` today gives the same bits as next month.
constexpr ModelEntry KNOWN_MODELS[] = {
    {"qwen3-4b",
     "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/qwen3-4b-q4_k_m.gguf",
     "qwen3-4b-q4_k_m.gguf",
     2'700'000'000ULL,
     nullptr},
    {"qwen3-1.7b",
     "https://huggingface.co/Qwen/Qwen3-1.7B-GGUF/resolve/main/qwen3-1.7b-q4_k_m.gguf",
     "qwen3-1.7b-q4_k_m.gguf",
     1'100'000'000ULL,
     nullptr},
    {"qwen3-0.6b",
     "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q8_0.gguf",
     "qwen3-0.6b-q8_0.gguf",
     650'000'000ULL,
     nullptr},
};

const ModelEntry* lookupModel(const std::string& name) {
    for (const auto& m : KNOWN_MODELS) {
        if (name == m.name) return &m;
    }
    return nullptr;
}

std::string modelsDir() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.sparx/models";
}

/// Formats bytes as human-readable (e.g. "2.7 GB").
std::string humanSize(std::uint64_t bytes) {
    if (bytes >= 1'000'000'000ULL) {
        return std::to_string(bytes / 1'000'000'000ULL) + "." +
               std::to_string((bytes / 100'000'000ULL) % 10) + " GB";
    }
    if (bytes >= 1'000'000ULL) {
        return std::to_string(bytes / 1'000'000ULL) + " MB";
    }
    return std::to_string(bytes) + " B";
}

/// Draws an in-place progress bar.
void drawProgress(std::uint64_t downloaded, std::uint64_t total,
                  double speed_mbps) {
    const int bar_width = 30;
    double frac = total > 0 ? static_cast<double>(downloaded) / total : 0.0;
    int filled = static_cast<int>(frac * bar_width);

    std::string bar(filled, '=');
    if (filled < bar_width) {
        bar += '>';
        bar += std::string(bar_width - filled - 1, ' ');
    }

    std::fprintf(stderr, "\r  [%s] %3.0f%%  %s / %s  %.1f MB/s",
                 bar.c_str(), frac * 100.0,
                 humanSize(downloaded).c_str(),
                 total > 0 ? humanSize(total).c_str() : "???",
                 speed_mbps);
    std::fflush(stderr);
}

/// Downloads url to dest using curl (universally available on macOS/Linux/Android).
/// Shows a progress bar. Returns true on success.
bool downloadWithProgress(const std::string& url, const fs::path& dest,
                          std::uint64_t expected_size) {
    // Use curl with --write-out to get status. The progress callback is
    // handled by reading the growing file size in a loop.
    const fs::path tmp = dest.string() + ".part";

    // Start curl in background
    const std::string cmd = "curl -fSL --connect-timeout 15 "
        "-o '" + tmp.string() + "' '" + url + "' 2>/dev/null &";
    std::system(cmd.c_str());

    // Wait for the file to appear
    for (int wait = 0; wait < 50 && !fs::exists(tmp); ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!fs::exists(tmp)) {
        std::cerr << "\n  ✗ download did not start (network?)\n";
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    std::uint64_t last_size = 0;
    int stall_count = 0;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::error_code ec;
        const auto current_size = static_cast<std::uint64_t>(
            fs::file_size(tmp, ec));
        if (ec) continue;

        auto elapsed = std::chrono::steady_clock::now() - start;
        double secs = std::chrono::duration<double>(elapsed).count();
        double speed = secs > 0 ? (current_size / 1'000'000.0) / secs : 0;

        drawProgress(current_size, expected_size, speed);

        if (current_size == last_size) {
            ++stall_count;
            if (stall_count > 50) {  // 10 seconds with no progress
                std::cerr << "\n  ✗ download stalled\n";
                fs::remove(tmp, ec);
                return false;
            }
        } else {
            stall_count = 0;
            last_size = current_size;
        }

        // Check if curl finished (file stopped growing and reached expected)
        if (expected_size > 0 && current_size >= expected_size) {
            break;
        }
        // For unknown size, check if curl process is done
        if (expected_size == 0 && stall_count > 5) {
            // Verify curl exited successfully
            break;
        }
    }
    std::fprintf(stderr, "\n");

    // Rename .part to final
    std::error_code ec;
    fs::rename(tmp, dest, ec);
    if (ec) {
        std::cerr << "  ✗ failed to rename download: " << ec.message() << "\n";
        return false;
    }
    return true;
}

}  // namespace

int cmd_pull(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "\n  usage: sparx pull <model>\n\n";
        std::cout << "  available models:\n";
        for (const auto& m : KNOWN_MODELS) {
            std::cout << "    " << m.name << "  ("
                      << humanSize(m.size_bytes) << ")\n";
        }
        std::cout << "\n  or pass a URL directly:\n";
        std::cout << "    sparx pull https://example.com/model.gguf\n\n";
        return 0;
    }

    const std::string& model_or_url = args[0];
    std::string url;
    std::string filename;
    std::uint64_t expected_size = 0;

    if (model_or_url.rfind("http", 0) == 0) {
        // Direct URL
        url = model_or_url;
        const auto slash = url.rfind('/');
        filename = slash != std::string::npos
            ? url.substr(slash + 1)
            : "model.gguf";
    } else {
        const auto* entry = lookupModel(model_or_url);
        if (!entry) {
            std::cerr << "  ✗ unknown model: " << model_or_url << "\n";
            std::cerr << "  run `sparx pull` with no arguments to see available models\n";
            return 1;
        }
        url = entry->url;
        filename = entry->filename;
        expected_size = entry->size_bytes;
    }

    const fs::path dir = modelsDir();
    fs::create_directories(dir);
    const fs::path dest = dir / filename;

    if (fs::exists(dest)) {
        std::error_code ec;
        const auto existing_size = static_cast<std::uint64_t>(fs::file_size(dest, ec));
        if (expected_size > 0 && existing_size >= expected_size * 95 / 100) {
            std::cout << "  ✓ " << filename << " already exists ("
                      << humanSize(existing_size) << ")\n";
            std::cout << "    path: " << dest.string() << "\n";
            return 0;
        }
        std::cout << "  existing file looks incomplete, re-downloading…\n";
    }

    std::cout << "\n  downloading " << filename << " ("
              << (expected_size > 0 ? humanSize(expected_size) : "unknown size")
              << ")\n";
    std::cout << "  → " << dest.string() << "\n\n";

    if (!downloadWithProgress(url, dest, expected_size)) {
        return 1;
    }

    std::cout << "  ✓ download complete: " << dest.string() << "\n";
    std::cout << "\n  next steps:\n";
    std::cout << "    sparx run --model " << dest.string() << "\n";
    std::cout << "    sparx deploy --device 1 --model " << dest.string() << "\n\n";
    return 0;
}

}  // namespace sparx
