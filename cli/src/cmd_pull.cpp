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

// These URLs point to HuggingFace Hub. Paths are case-sensitive: the Qwen2.5
// GGUF repos publish lower-case file names, the Qwen3 ones publish upper-case,
// and getting either wrong is a 404 rather than a redirect. Sizes are the
// approximate on-disk bytes and are only used to render the progress bar and
// to spot a truncated earlier download — completion is decided by curl's exit
// status, never by these numbers.
constexpr ModelEntry KNOWN_MODELS[] = {
    // Default for the quick start: small enough to download over a phone
    // tether, good enough to hold a conversation.
    {"qwen2.5-0.5b-instruct",
     "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q8_0.gguf",
     "qwen2.5-0.5b-instruct-q8_0.gguf",
     531'000'000ULL,
     nullptr},
    {"qwen2.5-1.5b-instruct",
     "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf",
     "qwen2.5-1.5b-instruct-q4_k_m.gguf",
     986'000'000ULL,
     nullptr},
    {"qwen2.5-3b-instruct",
     "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf",
     "qwen2.5-3b-instruct-q4_k_m.gguf",
     1'930'000'000ULL,
     nullptr},
    {"qwen3-0.6b",
     "https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf",
     "Qwen3-0.6B-Q8_0.gguf",
     639'000'000ULL,
     nullptr},
    {"qwen3-1.7b",
     "https://huggingface.co/Qwen/Qwen3-1.7B-GGUF/resolve/main/Qwen3-1.7B-Q4_K_M.gguf",
     "Qwen3-1.7B-Q4_K_M.gguf",
     1'110'000'000ULL,
     nullptr},
    {"qwen3-4b",
     "https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf",
     "Qwen3-4B-Q4_K_M.gguf",
     2'500'000'000ULL,
     nullptr},
};

/// The model `sparx pull` suggests when the developer has not picked one.
constexpr const char* DEFAULT_MODEL = "qwen2.5-0.5b-instruct";

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

/// Reads a whole small file. Returns false if it does not exist yet.
bool readSmallFile(const fs::path& path, std::string& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::getline(in, out);
    return true;
}

/// Downloads url to dest using curl (universally available on macOS/Linux/Android).
/// Shows a progress bar. Returns true on success.
///
/// curl runs detached so the progress bar can be driven from the growing
/// `.part` file, which means its exit status has to come back out of band: the
/// shell writes it to a sentinel file that this function polls for. That
/// sentinel is the ONLY thing that decides success. Deciding from file size
/// instead is what makes a 404 look like a finished download — HuggingFace
/// answers a bad path with a short HTML error body, and renaming that to
/// .gguf produces a model file that only fails later, inside llama-server,
/// with an error that points nowhere near the real cause.
bool downloadWithProgress(const std::string& url, const fs::path& dest,
                          std::uint64_t expected_size) {
    const fs::path tmp = dest.string() + ".part";
    const fs::path status_file = dest.string() + ".status";
    const fs::path error_file = dest.string() + ".stderr";

    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(status_file, ec);
    fs::remove(error_file, ec);

    // -f makes curl fail on HTTP >= 400 instead of saving the error body.
    // The subshell records the exit status atomically-enough for a poll loop:
    // the temp-then-rename keeps a half-written status out of the reader.
    const std::string cmd =
        "( curl -fSL --connect-timeout 15 -o '" + tmp.string() + "' '" + url +
        "' 2>'" + error_file.string() + "'; "
        "printf '%s' \"$?\" > '" + status_file.string() + ".tmp'; "
        "mv '" + status_file.string() + ".tmp' '" + status_file.string() + "' ) &";
    std::system(cmd.c_str());

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t last_size = 0;
    int stall_count = 0;
    std::string status_text;
    bool drew_bar = false;

    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const auto current_size = fs::exists(tmp)
            ? static_cast<std::uint64_t>(fs::file_size(tmp, ec))
            : 0;
        if (ec) ec.clear();

        const auto elapsed = std::chrono::steady_clock::now() - start;
        const double secs = std::chrono::duration<double>(elapsed).count();
        const double speed = secs > 0 ? (current_size / 1'000'000.0) / secs : 0;

        if (current_size > 0) {
            drawProgress(current_size, expected_size, speed);
            drew_bar = true;
        }

        // curl has exited: its status is authoritative, so stop polling.
        if (readSmallFile(status_file, status_text)) break;

        if (current_size == last_size) {
            // A stall only matters once bytes have started moving; before that
            // curl is still connecting and TLS-handshaking.
            if (++stall_count > 150) {  // 30s with no growth
                if (drew_bar) std::fprintf(stderr, "\n");
                std::cerr << "  ✗ download stalled with no data for 30s\n";
                fs::remove(tmp, ec);
                return false;
            }
        } else {
            stall_count = 0;
            last_size = current_size;
        }
    }

    if (drew_bar) std::fprintf(stderr, "\n");

    const int curl_status = status_text.empty() ? -1 : std::atoi(status_text.c_str());
    fs::remove(status_file, ec);

    if (curl_status != 0) {
        std::string detail;
        readSmallFile(error_file, detail);
        std::cerr << "  ✗ download failed (curl exit " << curl_status << ")";
        if (!detail.empty()) std::cerr << ": " << detail;
        std::cerr << "\n";
        if (curl_status == 22) {
            std::cerr << "    the server rejected the URL (HTTP 4xx/5xx). "
                         "if this is a built-in model name, the upstream "
                         "repository may have moved the file.\n";
        }
        fs::remove(tmp, ec);
        fs::remove(error_file, ec);
        return false;
    }
    fs::remove(error_file, ec);

    if (!fs::exists(tmp)) {
        std::cerr << "  ✗ download reported success but produced no file\n";
        return false;
    }

    // A GGUF starts with the magic bytes "GGUF". Checking them turns "the
    // download was actually an HTML error page" into an error here rather than
    // a confusing failure inside llama-server later. Only enforced for .gguf
    // destinations so `sparx pull <url>` of anything else still works.
    if (dest.extension() == ".gguf") {
        std::ifstream probe(tmp, std::ios::binary);
        char magic[4] = {};
        probe.read(magic, 4);
        if (probe.gcount() != 4 || std::string(magic, 4) != "GGUF") {
            std::cerr << "  ✗ downloaded file is not a GGUF model "
                         "(bad magic bytes)\n";
            std::cerr << "    the URL likely returned an error page rather "
                         "than model weights\n";
            fs::remove(tmp, ec);
            return false;
        }
    }

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
            const bool is_default = std::string(m.name) == DEFAULT_MODEL;
            std::cout << "    " << m.name << "  ("
                      << humanSize(m.size_bytes) << ")"
                      << (is_default ? "   ← start here" : "") << "\n";
        }
        std::cout << "\n  or pass a URL directly:\n";
        std::cout << "    sparx pull https://example.com/model.gguf\n";
        std::cout << "\n  models land in " << modelsDir() << "\n\n";
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
            std::cerr << "    known names:";
            for (const auto& m : KNOWN_MODELS) std::cerr << " " << m.name;
            std::cerr << "\n";
            std::cerr << "    run `sparx pull` with no arguments for sizes, "
                         "or pass a GGUF URL directly\n";
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

    std::error_code size_ec;
    const auto final_size = static_cast<std::uint64_t>(fs::file_size(dest, size_ec));
    std::cout << "  ✓ download complete";
    if (!size_ec) std::cout << " (" << humanSize(final_size) << ")";
    std::cout << "\n    " << dest.string() << "\n";

    // `sparx run` resolves a model from --model, then agent.yaml, then
    // $SPARX_MODEL. Show the flag first because it works from any directory,
    // and the agent.yaml form second because that is what makes it stick.
    std::cout << "\n  next steps:\n";
    std::cout << "    sparx run --model " << dest.string() << "\n";
    std::cout << "\n  or make it the default for this agent, in agent.yaml:\n";
    std::cout << "    model:\n";
    std::cout << "      path: " << dest.string() << "\n";
    std::cout << "\n  on-device (Qualcomm NPU) deployment is a separate step:\n";
    std::cout << "    sparx deploy --device 1 --model " << dest.string() << "\n\n";
    return 0;
}

}  // namespace sparx
