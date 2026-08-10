/**
 * @file llama_cpp_model_runtime.cpp
 * @brief LlamaCppModelRuntime — streams tokens from llama-server.
 */

#include "llama_cpp_model_runtime.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace master_agent::inference {

// ---------------------------------------------------------------------------
// Minimal HTTP client (libcurl-free, POSIX sockets). In production this would
// use a proper async client; for the CLI's local-dev experience, blocking I/O
// against localhost is acceptable.
// ---------------------------------------------------------------------------

namespace {

/// Opens a TCP connection to host:port. Returns fd or -1.
int tcpConnect(const std::string& host, int port) {
    // Resolve. For localhost this is trivial.
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                      &hints, &res) != 0 || !res) {
        return -1;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); return -1; }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ::close(fd); ::freeaddrinfo(res); return -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

/// Sends all bytes. Returns false on failure.
bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

/// Splits "host:port" into parts. Defaults to port 8080.
std::pair<std::string, int> splitEndpoint(const std::string& ep) {
    const auto colon = ep.rfind(':');
    if (colon == std::string::npos) return {ep, 8080};
    return {ep.substr(0, colon), std::stoi(ep.substr(colon + 1))};
}

/// Escapes a string for embedding in JSON.
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

/// Extracts a string value for `key` from a flat JSON object slice.
/// Deliberately minimal: the SSE payloads we parse have a known shape and
/// pulling in a full JSON parser here would widen the CLI's dependency set.
std::string extractJsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] != '"') return {};
    ++pos;
    std::string out;
    while (pos < json.size()) {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            const char esc = json[pos + 1];
            switch (esc) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                case 'u': {
                    if (pos + 5 < json.size()) {
                        const auto code = static_cast<unsigned>(
                            std::stoul(json.substr(pos + 2, 4), nullptr, 16));
                        // Encode as UTF-8
                        if (code < 0x80) {
                            out += static_cast<char>(code);
                        } else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        pos += 4;
                    }
                    break;
                }
                default: out += esc;
            }
            pos += 2;
            continue;
        }
        if (json[pos] == '"') break;
        out += json[pos];
        ++pos;
    }
    return out;
}

std::uint32_t approxTokens(const std::string& text) {
    // Same heuristic the mock runtime uses: ~4 bytes per token for mixed
    // CJK/latin. Only used for observability, never for admission.
    return static_cast<std::uint32_t>((text.size() + 3U) / 4U);
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / server lifecycle
// ---------------------------------------------------------------------------

LlamaCppModelRuntime::LlamaCppModelRuntime(LlamaCppConfig config)
    : config_(std::move(config)) {
    if (!config_.model_path.empty()) {
        ensureServer();
    }
}

LlamaCppModelRuntime::~LlamaCppModelRuntime() {
    stopServer();
}

void LlamaCppModelRuntime::ensureServer() {
    // If something is already listening, attach rather than spawn. This lets a
    // developer run their own llama-server with custom flags and have `sparx
    // run` use it, which is the difference between a tool that cooperates and
    // one that fights you.
    if (isServerReady()) {
        return;
    }

    std::string binary = config_.server_binary;
    if (binary.empty()) {
        binary = "llama-server";
    }

    const auto [host, port] = splitEndpoint(config_.endpoint);

    pid_t pid = ::fork();
    if (pid < 0) {
        return;  // caller discovers this through waitForServer()
    }
    if (pid == 0) {
        // Child: exec llama-server. Silence its stdout so it does not
        // interleave with the REPL; a developer who needs the log can run the
        // server themselves.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        std::vector<std::string> argv_s = {
            binary,
            "-m", config_.model_path,
            "--host", host,
            "--port", std::to_string(port),
            "-c", std::to_string(config_.context_length),
        };
        if (config_.threads > 0) {
            argv_s.push_back("-t");
            argv_s.push_back(std::to_string(config_.threads));
        }
        if (config_.gpu_layers > 0) {
            argv_s.push_back("-ngl");
            argv_s.push_back(std::to_string(config_.gpu_layers));
        }

        std::vector<char*> argv;
        argv.reserve(argv_s.size() + 1);
        for (auto& s : argv_s) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        ::execvp(binary.c_str(), argv.data());
        ::_exit(127);  // exec failed
    }

    server_pid_ = pid;
    server_managed_.store(true);
}

void LlamaCppModelRuntime::stopServer() {
    if (!server_managed_.load() || server_pid_ <= 0) {
        return;
    }
    // Cooperative shutdown first, matching the framework's preemption stance:
    // ask, then wait, and only escalate if it does not comply.
    ::kill(server_pid_, SIGTERM);
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        const pid_t r = ::waitpid(server_pid_, &status, WNOHANG);
        if (r == server_pid_) {
            server_pid_ = -1;
            server_managed_.store(false);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(server_pid_, SIGKILL);
    int status = 0;
    ::waitpid(server_pid_, &status, 0);
    server_pid_ = -1;
    server_managed_.store(false);
}

bool LlamaCppModelRuntime::isServerReady() const {
    const auto [host, port] = splitEndpoint(config_.endpoint);
    const int fd = tcpConnect(host, port);
    if (fd < 0) return false;

    const std::string req =
        "GET /health HTTP/1.1\r\nHost: " + host + "\r\n"
        "Connection: close\r\n\r\n";
    if (!sendAll(fd, req)) { ::close(fd); return false; }

    std::array<char, 512> buf{};
    const auto n = ::read(fd, buf.data(), buf.size() - 1);
    ::close(fd);
    if (n <= 0) return false;
    const std::string resp(buf.data(), static_cast<size_t>(n));
    return resp.find("200") != std::string::npos;
}

bool LlamaCppModelRuntime::waitForServer(
    std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (isServerReady()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Seal handling
// ---------------------------------------------------------------------------

Status LlamaCppModelRuntime::validateSeal(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal) const {
    // Same fail-closed checks the mock runtime performs. A runtime that skips
    // these lets a stale or misrouted invocation reach the model.
    if (seal.job_id != request.job_id ||
        seal.attempt_id.empty() || seal.operation_id.empty() ||
        seal.replica_id.empty() || seal.replica_epoch == 0 ||
        seal.lease_id.empty() || seal.fencing_token == 0 ||
        seal.prompt_digest != request.prompt_digest ||
        seal.deadline_mono_ns != request.deadline_mono_ns ||
        seal.invocation_id != runtimeInvocationDigest(seal)) {
        return Status::Error(
            "inference", "INFERENCE_RUNTIME_INVOCATION_INVALID",
            "runtime invocation seal is incomplete or inconsistent");
    }
    return Status::Ok();
}

void LlamaCppModelRuntime::echoSeal(
    InferenceOutput& out,
    const RuntimeInvocationSeal& seal,
    const InferenceRequest& request) const {
    out.model_id = request.model;
    out.model_digest = seal.model_digest;
    out.job_id = seal.job_id;
    out.operation_id = seal.operation_id;
    out.replica_id = seal.replica_id;
    out.replica_epoch = seal.replica_epoch;
    out.lease_id = seal.lease_id;
    out.fencing_token = seal.fencing_token;
    out.control_epoch = seal.control_epoch;
    out.attempt_id = seal.attempt_id;
    out.prompt_digest = seal.prompt_digest;
    out.invocation_id = seal.invocation_id;
    out.runtime_backend = config_.runtime_tag;
    out.reality = request.reality;
}

std::string LlamaCppModelRuntime::buildRequestBody(
    const InferenceRequest& request, bool stream) const {
    std::ostringstream body;
    body << "{\"model\":\"" << jsonEscape(request.model) << "\","
         << "\"messages\":[{\"role\":\"user\",\"content\":\""
         << jsonEscape(request.prompt) << "\"}],"
         << "\"stream\":" << (stream ? "true" : "false") << ","
         << "\"cache_prompt\":true";
    body << "}";
    return body.str();
}

std::uint32_t LlamaCppModelRuntime::requiredWorkUnits(
    const InferenceRequest& request) const {
    // Must not invoke the model. Estimate from prompt size only.
    const auto tokens = approxTokens(request.prompt);
    return tokens < 64U ? 1U : (tokens / 64U);
}

// ---------------------------------------------------------------------------
// Inference
// ---------------------------------------------------------------------------

Result<InferenceOutput> LlamaCppModelRuntime::infer(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal) {
    // Non-streaming path: delegate to inferStream with a null sink so there is
    // exactly one place where the HTTP protocol is implemented. A second copy
    // would drift.
    return inferStream(request, seal, nullptr);
}

Result<InferenceOutput> LlamaCppModelRuntime::inferStream(
    const InferenceRequest& request,
    const RuntimeInvocationSeal& seal,
    const InferenceStreamSink& sink) {
    const auto seal_status = validateSeal(request, seal);
    if (!seal_status.ok) {
        return Result<InferenceOutput>::Failure(seal_status);
    }

    const auto start = std::chrono::steady_clock::now();

    if (!isServerReady()) {
        if (!waitForServer(std::chrono::seconds(30))) {
            return Result<InferenceOutput>::Failure(Status::Error(
                "inference", "INFERENCE_RUNTIME_UNAVAILABLE",
                "llama-server is not reachable at " + config_.endpoint,
                true));
        }
    }

    const auto [host, port] = splitEndpoint(config_.endpoint);
    const int fd = tcpConnect(host, port);
    if (fd < 0) {
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_UNAVAILABLE",
            "could not connect to llama-server", true));
    }

    // Always ask for a stream. When sink is null we still consume the SSE
    // stream and concatenate, because using two different server code paths
    // for streaming and non-streaming would make them diverge in behavior.
    const std::string body = buildRequestBody(request, true);
    std::ostringstream req;
    req << "POST /v1/chat/completions HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    if (!sendAll(fd, req.str())) {
        ::close(fd);
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_IO_FAILED",
            "failed to send request to llama-server", true));
    }

    // ---- Read and parse the SSE stream ----
    std::string accumulated;
    std::string finish_reason;
    std::uint32_t chunk_index = 0;
    bool aborted = false;
    bool saw_done = false;
    std::string pending;      // incomplete line carry-over
    bool headers_done = false;

    std::array<char, 4096> buf{};
    while (true) {
        const auto n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;  // server closed

        pending.append(buf.data(), static_cast<size_t>(n));

        if (!headers_done) {
            const auto sep = pending.find("\r\n\r\n");
            if (sep == std::string::npos) continue;
            const std::string headers = pending.substr(0, sep);
            if (headers.find(" 200 ") == std::string::npos) {
                ::close(fd);
                return Result<InferenceOutput>::Failure(Status::Error(
                    "inference", "INFERENCE_RUNTIME_REJECTED",
                    "llama-server returned a non-200 response", true));
            }
            pending.erase(0, sep + 4);
            headers_done = true;
        }

        // Consume complete lines
        size_t nl = 0;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            // llama-server may use chunked transfer encoding; hex length
            // lines are not SSE payloads.
            if (line.rfind("data:", 0) != 0) continue;

            std::string payload = line.substr(5);
            while (!payload.empty() && payload.front() == ' ') {
                payload.erase(0, 1);
            }
            if (payload == "[DONE]") {
                saw_done = true;
                continue;
            }

            const std::string delta = extractJsonString(payload, "content");
            const std::string fr = extractJsonString(payload, "finish_reason");
            if (!fr.empty()) finish_reason = fr;
            if (delta.empty()) continue;

            accumulated += delta;

            if (sink) {
                InferenceChunk chunk;
                chunk.invocation_id = seal.invocation_id;
                chunk.chunk_index = chunk_index++;
                chunk.delta = delta;
                chunk.final = false;
                if (sink(chunk) == StreamControl::Abort) {
                    // Cooperative preemption: stop reading and let the
                    // framework decide whether the partial output commits.
                    aborted = true;
                    break;
                }
            } else {
                ++chunk_index;
            }
        }
        if (aborted) break;
    }
    ::close(fd);

    if (finish_reason.empty()) {
        finish_reason = aborted ? "abort" : (saw_done ? "stop" : "length");
    }

    // Exactly one final chunk, delivered last, as the ABI requires.
    if (sink) {
        InferenceChunk chunk;
        chunk.invocation_id = seal.invocation_id;
        chunk.chunk_index = chunk_index++;
        chunk.delta.clear();
        chunk.final = true;
        chunk.finish_reason = finish_reason;
        (void)sink(chunk);
    }

    if (accumulated.empty() && !aborted) {
        return Result<InferenceOutput>::Failure(Status::Error(
            "inference", "INFERENCE_RUNTIME_EMPTY_OUTPUT",
            "llama-server produced no output", true));
    }

    InferenceOutput out;
    out.raw_output = accumulated;
    out.finish_reason = finish_reason;
    echoSeal(out, seal, request);
    out.prompt_token_count = approxTokens(request.prompt);
    out.generated_token_count = approxTokens(accumulated);
    out.total_latency_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    out.output_digest = inferenceOutputDigest(out);
    return Result<InferenceOutput>::Success(std::move(out));
}

}  // namespace master_agent::inference

