#pragma once
/**
 * @file sparx_cloud_backend.h
 * @brief Cloud Backend — pluggable interface for cloud LLM API calls.
 *
 * Minimal-design: prompt template already compressed by IPromptEngine,
 * cloud backend just does the HTTP call and parses the response.
 * No agent logic, no tool calling — raw model inference only.
 *
 * Supports:
 *   - OpenAI-compatible /v1/chat/completions
 *   - Anthropic /v1/messages
 *   - Custom HTTP endpoints (via template)
 *
 * All implementations are async-capable: the caller fires and moves on,
 * checking for results later or using a callback.
 */

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace sparx {
namespace harness {

// ─── Configuration ──────────────────────────────────────────────────────────

struct CloudBackendConfig {
    /// Provider type for protocol selection.
    enum class Provider {
        OpenAICompatible,   // OpenAI, DeepSeek, Qwen, vLLM, etc.
        Anthropic,          // Anthropic Messages API
        Custom,             // Raw HTTP POST with template body
    };

    Provider provider = Provider::OpenAICompatible;

    /// API endpoint URL.
    std::string endpoint;

    /// API key (resolved from env var or direct value).
    std::string api_key;

    /// Environment variable name for API key (takes precedence over api_key).
    std::string api_key_env = "SPARX_CLOUD_KEY";

    /// Model identifier.
    std::string model = "qwen3-235b";

    /// Maximum tokens for cloud response.
    int max_tokens = 2048;

    /// Sampling temperature.
    float temperature = 0.7f;

    /// HTTP request timeout.
    int timeout_ms = 3000;

    /// Custom request body template (for Provider::Custom).
    /// Placeholders: {{model}}, {{prompt}}, {{system}}, {{max_tokens}}, {{temperature}}
    std::string custom_body_template;

    /// Custom response content JSONPath (for Provider::Custom).
    /// Default: "choices[0].message.content" (OpenAI format)
    std::string custom_response_path = "choices[0].message.content";
};

// ─── Response ───────────────────────────────────────────────────────────────

struct CloudResult {
    bool success = false;
    std::string content;            // Generated text
    int input_tokens = 0;           // Tokens consumed (input)
    int output_tokens = 0;          // Tokens generated (output)
    int latency_ms = 0;             // Wall-clock latency
    std::string error;              // Non-empty on failure
    std::string model;              // Actual model used (from response)
    std::string request_id;         // Cloud-side request ID (for debugging)
};

// ─── Interface ──────────────────────────────────────────────────────────────

/// Async cloud inference callback.
using CloudCallback = std::function<void(CloudResult)>;

/// Abstract cloud backend interface. Pluggable via harness config.
class ICloudBackend {
public:
    virtual ~ICloudBackend() = default;

    /// Synchronous inference call. Blocks until response or timeout.
    virtual CloudResult infer(
        const std::string& user_prompt,
        const std::string& system_prompt = ""
    ) const = 0;

    /// Asynchronous inference call. Returns a future.
    virtual std::future<CloudResult> inferAsync(
        const std::string& user_prompt,
        const std::string& system_prompt = ""
    ) const = 0;

    /// Fire-and-forget with callback (for deadline-based arbitration).
    virtual void inferWithCallback(
        const std::string& user_prompt,
        const std::string& system_prompt,
        CloudCallback callback
    ) const = 0;

    /// Check if the backend is properly configured and ready.
    virtual bool isReady() const = 0;

    /// Backend name (for tracing).
    virtual std::string name() const = 0;
};

// ─── Implementations ────────────────────────────────────────────────────────

/// OpenAI-compatible backend (covers DeepSeek, Qwen, vLLM, Ollama, etc.)
class OpenAICompatBackend : public ICloudBackend {
public:
    explicit OpenAICompatBackend(const CloudBackendConfig& config);

    CloudResult infer(
        const std::string& user_prompt,
        const std::string& system_prompt = ""
    ) const override;

    std::future<CloudResult> inferAsync(
        const std::string& user_prompt,
        const std::string& system_prompt = ""
    ) const override;

    void inferWithCallback(
        const std::string& user_prompt,
        const std::string& system_prompt,
        CloudCallback callback
    ) const override;

    bool isReady() const override;
    std::string name() const override { return "openai_compatible"; }

private:
    CloudBackendConfig config_;
    std::string resolved_api_key_;
    std::string buildRequestBody(const std::string& user_prompt,
                                  const std::string& system_prompt) const;
    CloudResult parseResponse(const std::string& body, int latency_ms) const;
    CloudResult doHttpPost(const std::string& body) const;
};

/// Null backend for testing — returns a configurable canned response.
class MockCloudBackend : public ICloudBackend {
public:
    explicit MockCloudBackend(const std::string& response = "mock cloud response",
                              int latency_ms = 50);

    CloudResult infer(
        const std::string& user_prompt,
        const std::string& system_prompt = ""
    ) const override;

    std::future<CloudResult> inferAsync(
        const std::string& user_prompt,
        const std::string& system_prompt = ""
    ) const override;

    void inferWithCallback(
        const std::string& user_prompt,
        const std::string& system_prompt,
        CloudCallback callback
    ) const override;

    bool isReady() const override { return true; }
    std::string name() const override { return "mock"; }

private:
    std::string response_;
    int latency_ms_;
};

// ─── Factory ────────────────────────────────────────────────────────────────

/// Create a cloud backend from config. Returns nullptr if provider is unknown.
std::unique_ptr<ICloudBackend> createCloudBackend(const CloudBackendConfig& config);

}  // namespace harness
}  // namespace sparx
