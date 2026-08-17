#pragma once
/**
 * @file sparx_cloud_fusion.h
 * @brief Cloud-Device Fusion (端云融合) — adaptive inference routing.
 *
 * When enabled, complex tasks are dispatched to a cloud model endpoint for
 * higher-quality inference while simple or latency-sensitive tasks stay on
 * the local device. The user controls this via a toggle in agent.yaml:
 *
 *   cloud_fusion:
 *     enabled: true
 *     endpoint: "https://api.example.com/v1/chat/completions"
 *     api_key_env: "SPARX_CLOUD_KEY"
 *     model: "qwen3-235b"
 *     complexity_threshold: 0.6
 *     max_cloud_tokens: 4096
 *     timeout_ms: 30000
 *     fallback_to_local: true
 *
 * Complexity is estimated from the input's token count, structural markers
 * (multi-step reasoning, code generation, long context), and historical
 * latency on similar intents.
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sparx {
namespace cloud_fusion {

// ─── Configuration ───────────────────────────────────────────────────────────

struct CloudFusionConfig {
    /// Master toggle: when false, all inference stays local regardless of
    /// complexity. Controlled by agent.yaml `cloud_fusion.enabled`.
    bool enabled = false;

    /// OpenAI-compatible endpoint for cloud inference.
    std::string endpoint;

    /// Environment variable name that holds the API key.
    std::string api_key_env = "SPARX_CLOUD_KEY";

    /// Cloud model identifier sent in the request body.
    std::string model = "qwen3-235b";

    /// Complexity score in [0, 1] above which the task routes to cloud.
    /// 0.0 = always cloud (when enabled), 1.0 = never cloud.
    float complexity_threshold = 0.6f;

    /// Maximum tokens the cloud model may generate.
    int max_cloud_tokens = 4096;

    /// HTTP timeout for cloud requests.
    int timeout_ms = 30000;

    /// If cloud inference fails (timeout, error), fall back to local model.
    bool fallback_to_local = true;

    /// Temperature for cloud model sampling.
    float temperature = 0.7f;
};

// ─── Complexity Estimation ───────────────────────────────────────────────────

/// Signals used to estimate task complexity.
struct ComplexitySignals {
    /// Approximate token count of the input.
    int token_count = 0;

    /// Number of sentences / logical steps detected.
    int step_count = 0;

    /// Contains code-generation markers (```, function, class, def, etc.)
    bool has_code_markers = false;

    /// Contains multi-step reasoning markers (first, then, finally, etc.)
    bool has_reasoning_chain = false;

    /// Contains explicit instruction to analyze / compare / evaluate
    bool has_analytical_request = false;

    /// Historical average latency (ms) for similar intents; 0 if unknown.
    uint32_t historical_latency_ms = 0;

    /// Context length already consumed (multi-turn).
    int context_tokens_used = 0;
};

/// Estimates task complexity in [0, 1] from input signals.
/// A score above config.complexity_threshold triggers cloud routing.
class ComplexityEstimator {
public:
    /// Analyze raw input text and return complexity signals.
    ComplexitySignals analyze(const std::string& input) const;

    /// Compute a scalar complexity score from signals.
    float score(const ComplexitySignals& signals) const;

    /// Convenience: analyze + score in one call.
    float estimate(const std::string& input) const {
        return score(analyze(input));
    }
};

// ─── Routing Decision ────────────────────────────────────────────────────────

enum class InferenceRoute {
    Local,   // Execute on device (NPU/CPU/GPU)
    Cloud,   // Dispatch to cloud endpoint
};

struct RoutingDecision {
    InferenceRoute route = InferenceRoute::Local;
    float complexity_score = 0.0f;
    std::string reason;  // Human-readable explanation for tracing
};

/// Decides whether a given input should route to cloud or stay local.
RoutingDecision decideRoute(const CloudFusionConfig& config,
                            const ComplexityEstimator& estimator,
                            const std::string& input);

// ─── Cloud Inference Client ──────────────────────────────────────────────────

struct CloudResponse {
    bool success = false;
    std::string content;           // Generated text
    int tokens_used = 0;
    int latency_ms = 0;
    std::string error;             // Non-empty on failure
};

/// Callback for streaming cloud tokens (mirrors local streaming pattern).
using CloudStreamSink = std::function<bool(const std::string& delta, bool final)>;

/// Lightweight HTTP client for OpenAI-compatible cloud endpoints.
/// Uses POSIX sockets + TLS (or libcurl if available) — no heavy deps.
class CloudInferenceClient {
public:
    explicit CloudInferenceClient(const CloudFusionConfig& config);

    /// Synchronous (blocking) inference call. Returns the full response.
    CloudResponse infer(const std::string& prompt,
                        const std::string& system_prompt = "") const;

    /// Streaming inference. Calls sink for each token chunk.
    CloudResponse inferStream(const std::string& prompt,
                              CloudStreamSink sink,
                              const std::string& system_prompt = "") const;

    /// Check if the client is properly configured (has API key, endpoint).
    bool isConfigured() const;

private:
    CloudFusionConfig config_;
    std::string api_key_;  // Resolved from environment at construction
};

// ─── Fusion Controller (top-level orchestrator) ──────────────────────────────

/// Integrates complexity estimation, routing, and cloud client into a single
/// interface that cmd_run can call without knowing the internals.
class FusionController {
public:
    explicit FusionController(const CloudFusionConfig& config);

    /// Returns true if cloud fusion is fully operational (enabled + configured).
    bool isActive() const;

    /// Get the routing decision for a given input.
    RoutingDecision route(const std::string& input) const;

    /// Execute cloud inference (only call when route() returns Cloud).
    CloudResponse cloudInfer(const std::string& prompt,
                             const std::string& system_prompt = "") const;

    /// Execute cloud inference with streaming.
    CloudResponse cloudInferStream(const std::string& prompt,
                                   CloudStreamSink sink,
                                   const std::string& system_prompt = "") const;

    /// Runtime toggle: enable/disable without restarting.
    void setEnabled(bool enabled);
    bool isEnabled() const { return config_.enabled; }

    /// Update complexity threshold at runtime (e.g. from a /cloud command).
    void setThreshold(float threshold);
    float getThreshold() const { return config_.complexity_threshold; }

    const CloudFusionConfig& config() const { return config_; }

private:
    CloudFusionConfig config_;
    ComplexityEstimator estimator_;
    std::unique_ptr<CloudInferenceClient> client_;
};

// ─── YAML Parsing Helper ─────────────────────────────────────────────────────

/// Parse the `cloud_fusion:` section from agent.yaml lines.
/// Integrates with the existing sparx_agent_config.h minimal parser.
CloudFusionConfig parseCloudFusionConfig(const std::string& yaml_path);

}  // namespace cloud_fusion
}  // namespace sparx
