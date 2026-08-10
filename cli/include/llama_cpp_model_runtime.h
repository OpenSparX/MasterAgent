/**
 * @file llama_cpp_model_runtime.h
 * @brief IModelRuntime adapter for llama.cpp — CPU/GPU inference on any ARM host.
 *
 * This is the workhorse for Experience A (local development). It spawns or
 * connects to a llama-server process exposing the OpenAI-compatible /v1/chat
 * endpoint, streams chunks through the framework's verified sink, and echoes
 * the seal contract exactly.
 *
 * Why llama.cpp and not GGML raw:
 *  - llama-server already handles KV cache, context shifting, and batched decode.
 *  - It ships as a single static binary (or NDK .so) — no Python dependency.
 *  - The /v1/chat/completions SSE stream maps 1:1 to InferenceChunk.
 *
 * Lifecycle: the runtime manages a child llama-server process when constructed
 * with a model path. It can also attach to an existing server (e.g. started by
 * the user) via a URL.
 */

#pragma once

#include "master_agent/inference/inference_framework.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <sys/types.h>

namespace master_agent::inference {

struct LlamaCppConfig {
    /// Path to the GGUF model file.
    std::string model_path;
    /// Path to the llama-server binary. If empty, searches $PATH.
    std::string server_binary;
    /// Host:port for the HTTP API. If a server is already listening here,
    /// the runtime attaches without spawning a child.
    std::string endpoint = "127.0.0.1:8080";
    /// Context window to request from the server.
    std::uint32_t context_length = 4096;
    /// Number of threads (-t flag). 0 = let llama.cpp decide.
    std::uint32_t threads = 0;
    /// GPU layers to offload (-ngl). 0 = CPU only.
    std::uint32_t gpu_layers = 0;
    /// Timeout for a single inference call. If 0, uses seal.deadline_mono_ns.
    std::chrono::milliseconds timeout{0};
    /// Runtime tag used in model_digest validation. Must match the executor's
    /// seal construction for this runtime (currently a wiring TODO: the
    /// model_executor hardcodes "mock-runtime"; deploying this runtime requires
    /// parameterizing that to "llama-cpp").
    std::string runtime_tag = "llama-cpp";
};

/**
 * @brief Streams tokens from llama-server through the framework's verified sink.
 *
 * Streaming protocol:
 *  1. POST /v1/chat/completions with stream:true
 *  2. Read SSE lines: each `data: {...}` yields one InferenceChunk
 *  3. On `data: [DONE]`, emit the final chunk
 *  4. Return InferenceOutput with raw_output = concatenation of all deltas
 *
 * The framework independently verifies that concatenation matches, so this
 * runtime cannot misreport what it streamed.
 */
class LlamaCppModelRuntime final : public IModelRuntime {
public:
    explicit LlamaCppModelRuntime(LlamaCppConfig config);
    ~LlamaCppModelRuntime() override;

    std::uint32_t requiredWorkUnits(
        const InferenceRequest& request) const override;

    std::string runtimeTag() const override { return config_.runtime_tag; }

    Result<InferenceOutput> infer(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal) override;

    bool supportsStreaming() const override { return true; }

    Result<InferenceOutput> inferStream(
        const InferenceRequest& request,
        const RuntimeInvocationSeal& seal,
        const InferenceStreamSink& sink) override;

    /// True after the server responds to /health.
    bool isServerReady() const;

    /// Blocks until the server is ready or timeout elapses.
    bool waitForServer(std::chrono::milliseconds timeout) const;

private:
    /// Validates seal fields that this runtime can check independently.
    Status validateSeal(const InferenceRequest& request,
                        const RuntimeInvocationSeal& seal) const;

    /// Builds the JSON body for the /v1/chat/completions request.
    std::string buildRequestBody(const InferenceRequest& request,
                                 bool stream) const;

    /// Populates output fields from the seal (the echo contract).
    void echoSeal(InferenceOutput& out,
                  const RuntimeInvocationSeal& seal,
                  const InferenceRequest& request) const;

    /// Spawns llama-server if needed.
    void ensureServer();
    void stopServer();

    LlamaCppConfig config_;
    std::atomic<bool> server_managed_{false};
    pid_t server_pid_ = -1;
};

}  // namespace master_agent::inference
