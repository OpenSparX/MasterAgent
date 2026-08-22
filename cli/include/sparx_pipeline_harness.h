#pragma once
/**
 * @file sparx_pipeline_harness.h
 * @brief Pipeline Harness — pluggable orchestrator for edge-cloud inference.
 *
 * Inspired by DeepSeek Harness design: all components are pluggable slots.
 * The harness wires together:
 *   - IPromptEngine: prompt compression for cloud, full prompt for local
 *   - ICloudBackend: cloud LLM API caller
 *   - IArbiter: decision logic to pick local vs cloud
 *   - IConfidenceScorer: quality estimation for routing
 *
 * Execution flow:
 *   1. Speculative cache check (existing) → hit? return immediately
 *   2. Pre-score confidence
 *   3. If confidence < high_threshold: fire cloud path (async)
 *   4. Run local inference
 *   5. Post-score local result
 *   6. Wait for cloud (bounded by deadline)
 *   7. Arbiter picks final output
 *
 * Configuration is YAML-driven. Components are hot-swappable at runtime.
 */

#include "sparx_arbiter.h"
#include "sparx_cloud_backend.h"
#include "sparx_confidence_scorer.h"
#include "sparx_prompt_engine.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sparx {
namespace harness {

// ─── Harness Configuration ──────────────────────────────────────────────────

struct HarnessConfig {
    /// Which prompt engine to activate.
    std::string prompt_engine = "compressed";

    /// Which cloud backend to activate.
    std::string cloud_backend = "openai_compatible";

    /// Which arbiter strategy to use.
    std::string arbiter = "cloud_prefer";

    /// Which confidence scorer to use.
    std::string confidence_scorer = "heuristic";

    /// Confidence thresholds for routing.
    ConfidenceThresholds confidence_thresholds;

    /// Cloud backend config.
    CloudBackendConfig cloud_config;

    /// Prompt engine config.
    PromptEngineConfig prompt_config;

    /// Arbiter config.
    ArbiterConfig arbiter_config;

    /// Master enable switch for cloud path.
    bool cloud_enabled = true;

    /// Trace/log every arbitration decision.
    bool trace_decisions = false;
};

// ─── Request / Response ─────────────────────────────────────────────────────

/// Input to the harness pipeline.
struct PipelineRequest {
    std::string user_input;
    std::vector<ConversationTurn> history;
    std::unordered_map<std::string, std::string> context_vars;

    /// Intent type (if already classified by speculative engine).
    std::string intent_type;

    /// Whether speculative cache was already checked (and missed).
    bool speculative_miss = true;

    /// Priority level for deadline selection.
    enum class Priority { RealTime, Interactive, Batch } priority = Priority::Interactive;
};

/// Output from the harness pipeline.
struct PipelineResponse {
    ArbiterOutput result;

    /// Timing breakdown.
    int local_latency_ms = 0;
    int cloud_latency_ms = 0;
    int total_latency_ms = 0;

    /// Token usage (cloud path).
    int cloud_input_tokens = 0;
    int cloud_output_tokens = 0;

    /// Routing decision trace.
    ConfidenceScore confidence;
    bool cloud_fired = false;
    std::string prompt_engine_used;
};

// ─── Local Inference Adapter ────────────────────────────────────────────────

/// Interface for local inference. The harness calls this for the device path.
/// This wraps whatever IModelRuntime the system is using (llama.cpp, QNN, etc.)
class ILocalInference {
public:
    virtual ~ILocalInference() = default;

    /// Run local inference on the given prompt. Returns content + quality signals.
    virtual LocalResult infer(const std::string& prompt) const = 0;

    /// Get post-score signals from the last inference run.
    virtual PostScoreSignals getLastPostSignals() const = 0;

    /// Check if a local model is loaded and ready.
    virtual bool isReady() const = 0;

    /// Backend name (for tracing).
    virtual std::string name() const = 0;
};

// ─── Pipeline Harness ───────────────────────────────────────────────────────

class PipelineHarness {
public:
    PipelineHarness();
    ~PipelineHarness();

    // ─── Component Registration (pluggable slots) ───

    void registerPromptEngine(const std::string& name,
                              std::shared_ptr<IPromptEngine> engine);
    void registerCloudBackend(const std::string& name,
                              std::shared_ptr<ICloudBackend> backend);
    void registerArbiter(const std::string& name,
                         std::shared_ptr<IArbiter> arbiter);
    void registerConfidenceScorer(const std::string& name,
                                  std::shared_ptr<IConfidenceScorer> scorer);
    void registerLocalInference(const std::string& name,
                                std::shared_ptr<ILocalInference> local);

    // ─── Configuration ───

    /// Load full config from YAML file.
    void loadConfig(const std::string& yaml_path);

    /// Apply a config struct directly.
    void applyConfig(const HarnessConfig& config);

    /// Get current config (read-only).
    const HarnessConfig& config() const { return config_; }

    // ─── Execution ───

    /// Execute the full pipeline: local + cloud (if needed) + arbitration.
    PipelineResponse execute(const PipelineRequest& request);

    /// Execute cloud-only (bypass local). For testing or forced cloud mode.
    PipelineResponse executeCloudOnly(const PipelineRequest& request);

    /// Execute local-only (bypass cloud). For offline mode.
    PipelineResponse executeLocalOnly(const PipelineRequest& request);

    // ─── Runtime Control ───

    /// Enable/disable cloud path at runtime.
    void setCloudEnabled(bool enabled);
    bool isCloudEnabled() const;

    /// Switch active components at runtime.
    void setActivePromptEngine(const std::string& name);
    void setActiveCloudBackend(const std::string& name);
    void setActiveArbiter(const std::string& name);

    /// Check if the harness is fully initialized and ready to execute.
    bool isReady() const;

private:
    HarnessConfig config_;
    mutable std::mutex mutex_;

    // Registered component pools
    std::unordered_map<std::string, std::shared_ptr<IPromptEngine>> prompt_engines_;
    std::unordered_map<std::string, std::shared_ptr<ICloudBackend>> cloud_backends_;
    std::unordered_map<std::string, std::shared_ptr<IArbiter>> arbiters_;
    std::unordered_map<std::string, std::shared_ptr<IConfidenceScorer>> scorers_;
    std::unordered_map<std::string, std::shared_ptr<ILocalInference>> local_inferences_;

    // Active component pointers (resolved from config + registry)
    std::shared_ptr<IPromptEngine> active_prompt_engine_;
    std::shared_ptr<ICloudBackend> active_cloud_backend_;
    std::shared_ptr<IArbiter> active_arbiter_;
    std::shared_ptr<IConfidenceScorer> active_scorer_;
    std::shared_ptr<ILocalInference> active_local_;

    // Internal helpers
    void resolveActiveComponents();
    PreScoreSignals buildPreScoreSignals(const PipelineRequest& request) const;
    bool shouldFireCloud(const ConfidenceScore& pre_score) const;
};

// ─── YAML Config Parser ─────────────────────────────────────────────────────

/// Parse harness configuration from YAML file.
HarnessConfig parseHarnessConfig(const std::string& yaml_path);

}  // namespace harness
}  // namespace sparx
