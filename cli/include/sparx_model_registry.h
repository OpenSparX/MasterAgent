#pragma once
/**
 * @file sparx_model_registry.h
 * @brief Unified Model Registry — configure local + cloud models in one place.
 *
 * Replaces the separate `model:` and `cloud_fusion:` config sections with a
 * single `models:` registry. Each entry declares a model's type, endpoint,
 * capabilities, and routing role.
 *
 * Example agent.yaml:
 *
 *   models:
 *     - name: qwen3-4b
 *       type: local              # local GGUF via llama-server
 *       path: "./models/qwen3-4b-q4.gguf"
 *       context_length: 4096
 *       max_output_tokens: 512
 *       role: default            # primary inference model
 *
 *     - name: qwen3-235b
 *       type: cloud              # OpenAI-compatible API
 *       endpoint: "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
 *       api_key_env: "DASHSCOPE_API_KEY"
 *       context_length: 131072
 *       max_output_tokens: 4096
 *       temperature: 0.7
 *       role: cloud              # used for complex tasks
 *
 *     - name: deepseek-r1
 *       type: cloud
 *       endpoint: "https://api.deepseek.com/v1/chat/completions"
 *       api_key_env: "DEEPSEEK_API_KEY"
 *       context_length: 65536
 *       max_output_tokens: 8192
 *       temperature: 0.6
 *       role: reasoning          # fallback for chain-of-thought
 *
 *   routing:
 *     default_model: qwen3-4b
 *     cloud_model: qwen3-235b
 *     reasoning_model: deepseek-r1
 *     complexity_threshold: 0.6
 *     cloud_enabled: true
 *     fallback_to_local: true
 *     timeout_ms: 30000
 */

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sparx {

// ─── Model Entry ────────────────────────────────────────────────────────────

/// Model backend type.
enum class ModelType : uint8_t {
    Local,      // GGUF file via llama-server (on-device NPU/CPU)
    Server,     // Remote llama-server instance (self-hosted)
    Cloud,      // OpenAI-compatible HTTP API (third-party)
};

/// Role this model plays in the routing pipeline.
enum class ModelRole : uint8_t {
    Default,    // Primary model for all tasks
    Cloud,      // Cloud model for complex tasks (端云融合)
    Reasoning,  // Specialized reasoning/chain-of-thought model
    Embedding,  // Embedding model (for RAG/search)
    Fallback,   // Fallback when primary is unavailable
};

/// A single model definition in the registry.
struct ModelEntry {
    /// Unique name (referenced by routing config).
    std::string name;

    /// Backend type.
    ModelType type = ModelType::Local;

    /// Role in routing.
    ModelRole role = ModelRole::Default;

    // ── Local model fields ──
    /// Path to GGUF file (type=local only). Supports env vars: $HOME/models/...
    std::string path;

    // ── Server/Cloud fields ──
    /// API endpoint URL.
    std::string endpoint;
    /// Environment variable holding the API key (cloud only).
    std::string api_key_env;

    // ── Common fields ──
    /// Model identifier sent in API requests / used for display.
    std::string model_id;
    /// Maximum context window size (tokens).
    int context_length = 4096;
    /// Maximum output tokens per generation.
    int max_output_tokens = 512;
    /// Sampling temperature (0.0 = deterministic).
    float temperature = 0.7f;

    // ── Capabilities (for routing decisions) ──
    /// Can handle code generation tasks.
    bool supports_code = true;
    /// Can handle reasoning / chain-of-thought.
    bool supports_reasoning = true;
    /// Can handle tool use / function calling.
    bool supports_tools = false;
    /// Can handle vision/multimodal input.
    bool supports_vision = false;
    /// Estimated cost tier (0=free/local, 1=cheap, 2=moderate, 3=expensive).
    int cost_tier = 0;
};

// ─── Routing Configuration ──────────────────────────────────────────────────

struct RoutingConfig {
    /// Name of the default model for normal tasks.
    std::string default_model;
    /// Name of the cloud model for complex tasks.
    std::string cloud_model;
    /// Name of the reasoning model (chain-of-thought fallback).
    std::string reasoning_model;
    /// Name of the embedding model (optional).
    std::string embedding_model;

    /// Enable cloud dispatch for complex tasks.
    bool cloud_enabled = false;
    /// Complexity threshold [0,1] — above this, route to cloud_model.
    float complexity_threshold = 0.6f;
    /// Fall back to default_model if cloud fails.
    bool fallback_to_local = true;
    /// HTTP timeout for cloud/server requests (ms).
    int timeout_ms = 30000;

    /// Prefer deterministic skills before model inference.
    bool deterministic_first = true;
    /// Confidence threshold for skill matching.
    float confidence_threshold = 0.85f;
};

// ─── Model Registry ─────────────────────────────────────────────────────────

/**
 * @brief Central registry of all configured models.
 *
 * Parsed from agent.yaml `models:` section at startup. Provides lookup by
 * name and role, validates configuration, and integrates with the cloud
 * fusion routing engine.
 */
class ModelRegistry {
public:
    ModelRegistry() = default;

    /// Add a model entry to the registry.
    void add(ModelEntry entry);

    /// Get model by name. Returns nullptr if not found.
    const ModelEntry* get(const std::string& name) const;

    /// Get first model matching a role.
    const ModelEntry* getByRole(ModelRole role) const;

    /// Get the default (primary) model.
    const ModelEntry* defaultModel() const;

    /// Get the cloud model for complex tasks.
    const ModelEntry* cloudModel() const;

    /// Get the reasoning model.
    const ModelEntry* reasoningModel() const;

    /// Get all registered models.
    const std::vector<ModelEntry>& all() const { return models_; }

    /// Number of registered models.
    size_t size() const { return models_.size(); }

    /// Check if a model name exists.
    bool has(const std::string& name) const;

    /// Set routing configuration.
    void setRouting(RoutingConfig config) { routing_ = std::move(config); }

    /// Get routing configuration.
    const RoutingConfig& routing() const { return routing_; }

    /// Validate the registry (check all routing refs exist, required fields).
    struct ValidationResult {
        bool valid = true;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };
    ValidationResult validate() const;

    /// Generate a summary string (for `sparx doctor` output).
    std::string summary() const;

private:
    std::vector<ModelEntry> models_;
    RoutingConfig routing_;
};

// ─── Parser ─────────────────────────────────────────────────────────────────

/// Parse `models:` and `routing:` sections from agent.yaml content.
/// This extends the existing minimal YAML parser.
bool parseModelRegistry(const std::string& yaml_content, ModelRegistry& registry);

// ─── Helpers ────────────────────────────────────────────────────────────────

/// Convert ModelType to string.
const char* modelTypeStr(ModelType type);

/// Convert ModelRole to string.
const char* modelRoleStr(ModelRole role);

/// Parse ModelType from string.
ModelType parseModelType(const std::string& s);

/// Parse ModelRole from string.
ModelRole parseModelRole(const std::string& s);

}  // namespace sparx
