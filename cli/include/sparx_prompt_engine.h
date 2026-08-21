#pragma once
/**
 * @file sparx_prompt_engine.h
 * @brief Prompt Engine — local prompt compression & rendering before cloud dispatch.
 *
 * The Prompt Engine sits between the user intent and the cloud backend. Its job:
 *   1. Intent Distillation: reduce natural language to structured semantics
 *   2. Context Pruning: keep only relevant conversation history
 *   3. Template Rendering: fill a domain-specific compressed template
 *   4. Token Budget: enforce a max token budget for cloud payloads
 *
 * Designed as a pluggable interface (IPromptEngine) so different strategies
 * can be swapped via harness configuration without code changes.
 */

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sparx {
namespace harness {

// ─── Data Types ─────────────────────────────────────────────────────────────

/// A single turn in the conversation history.
struct ConversationTurn {
    std::string role;     // "user" | "assistant" | "system"
    std::string content;
    int64_t timestamp_ms = 0;
    float relevance = 1.0f;  // Set by pruning pass
};

/// Structured intent extracted from user input.
struct DistilledIntent {
    std::string task_type;          // e.g. "navigation", "vehicle_control", "media"
    std::string query;              // Core question/command
    std::unordered_map<std::string, std::string> params;  // Extracted parameters
    float confidence = 0.0f;        // Distillation confidence
};

/// The output of the Prompt Engine — ready to send to cloud.
struct CompressedPrompt {
    std::string system_prompt;      // Minimal system context for cloud
    std::string user_prompt;        // Compressed user payload
    int estimated_tokens = 0;       // Pre-estimated input token count
    DistilledIntent intent;         // Structured intent (for tracing)
};

/// Configuration for the prompt engine.
struct PromptEngineConfig {
    /// Maximum tokens to send to cloud (input budget).
    int max_cloud_input_tokens = 500;

    /// Maximum conversation turns to retain after pruning.
    int max_history_turns = 3;

    /// Minimum relevance score to keep a history turn.
    float relevance_threshold = 0.3f;

    /// Path to template directory.
    std::string template_dir = "templates";

    /// Default template name (without extension).
    std::string default_template = "default";

    /// Whether to include structured parameters as JSON.
    bool structured_output = true;
};

// ─── Interface ──────────────────────────────────────────────────────────────

/// Abstract prompt engine interface. Implementations can be swapped via harness.
class IPromptEngine {
public:
    virtual ~IPromptEngine() = default;

    /// Compress a user input + conversation history into a cloud-ready prompt.
    virtual CompressedPrompt compress(
        const std::string& user_input,
        const std::vector<ConversationTurn>& history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const = 0;

    /// Render a prompt for local inference (may be fuller than cloud version).
    virtual std::string renderLocal(
        const std::string& user_input,
        const std::vector<ConversationTurn>& history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const = 0;

    /// Get the engine name (for tracing/logging).
    virtual std::string name() const = 0;
};

// ─── Implementations ────────────────────────────────────────────────────────

/// Default compressed prompt engine — distill + prune + template.
class CompressedPromptEngine : public IPromptEngine {
public:
    explicit CompressedPromptEngine(const PromptEngineConfig& config);

    CompressedPrompt compress(
        const std::string& user_input,
        const std::vector<ConversationTurn>& history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const override;

    std::string renderLocal(
        const std::string& user_input,
        const std::vector<ConversationTurn>& history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const override;

    std::string name() const override { return "compressed"; }

    /// Distill user input into a structured intent.
    DistilledIntent distill(const std::string& input) const;

    /// Prune conversation history to relevant turns only.
    std::vector<ConversationTurn> prune(
        const std::vector<ConversationTurn>& history,
        const DistilledIntent& intent
    ) const;

    /// Estimate token count of a string.
    int estimateTokens(const std::string& text) const;

private:
    PromptEngineConfig config_;
    std::string loadTemplate(const std::string& template_name) const;
    std::string renderTemplate(
        const std::string& tmpl,
        const DistilledIntent& intent,
        const std::vector<ConversationTurn>& pruned_history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const;
};

/// Verbose prompt engine — sends full context (for debugging/testing).
class VerbosePromptEngine : public IPromptEngine {
public:
    explicit VerbosePromptEngine(const PromptEngineConfig& config);

    CompressedPrompt compress(
        const std::string& user_input,
        const std::vector<ConversationTurn>& history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const override;

    std::string renderLocal(
        const std::string& user_input,
        const std::vector<ConversationTurn>& history,
        const std::unordered_map<std::string, std::string>& context_vars
    ) const override;

    std::string name() const override { return "verbose"; }

private:
    PromptEngineConfig config_;
};

}  // namespace harness
}  // namespace sparx
