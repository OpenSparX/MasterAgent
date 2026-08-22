#pragma once
/**
 * @file sparx_confidence_scorer.h
 * @brief Confidence Scorer — evaluates local inference output quality.
 *
 * Used by the arbiter to decide whether to trust local results or prefer cloud.
 * Two-phase scoring:
 *   1. Pre-score (before inference): heuristic based on intent type + history
 *   2. Post-score (after inference): based on output quality signals (logprob, etc.)
 *
 * Pluggable interface — different scoring strategies can be registered.
 */

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sparx {
namespace harness {

// ─── Data Types ─────────────────────────────────────────────────────────────

/// Signals available before local inference starts.
struct PreScoreSignals {
    std::string intent_type;         // Detected intent category
    float speculative_hit_rate = 0.0f;  // Historical hit rate for this intent
    int input_token_count = 0;       // Input complexity proxy
    bool is_deterministic = false;   // Can be handled by skill engine
    int similar_intent_successes = 0;  // Past successes on similar intents
    int similar_intent_failures = 0;   // Past failures on similar intents
};

/// Signals available after local inference completes.
struct PostScoreSignals {
    float avg_logprob = 0.0f;        // Average log probability of output tokens
    float min_logprob = 0.0f;        // Minimum log probability (uncertainty peak)
    int output_token_count = 0;      // Output length
    bool truncated = false;          // Output was cut off by max_tokens
    float perplexity = 0.0f;         // Perplexity of the output
    bool has_repetition = false;     // Detected repetitive patterns
    bool format_valid = true;        // Output matches expected format
};

/// Combined confidence score with breakdown.
struct ConfidenceScore {
    float overall = 0.0f;            // [0.0, 1.0] — composite confidence
    float pre_score = 0.0f;          // Pre-inference confidence
    float post_score = 0.0f;         // Post-inference confidence (0 if not yet computed)
    std::string reason;              // Human-readable explanation
};

// ─── Interface ──────────────────────────────────────────────────────────────

/// Abstract confidence scorer interface.
class IConfidenceScorer {
public:
    virtual ~IConfidenceScorer() = default;

    /// Pre-inference confidence estimate (fast, heuristic).
    /// Used to decide whether to fire cloud request in parallel.
    virtual ConfidenceScore preScore(const PreScoreSignals& signals) const = 0;

    /// Post-inference confidence (uses output quality signals).
    /// Used by arbiter to choose between local and cloud.
    virtual ConfidenceScore postScore(
        const PreScoreSignals& pre_signals,
        const PostScoreSignals& post_signals
    ) const = 0;

    /// Scorer name (for tracing).
    virtual std::string name() const = 0;
};

// ─── Implementations ────────────────────────────────────────────────────────

/// Heuristic confidence scorer — rule-based, fast, no model needed.
class HeuristicScorer : public IConfidenceScorer {
public:
    struct Config {
        /// Confidence boost for deterministic intents.
        float deterministic_boost = 0.5f;

        /// Weight for historical hit rate.
        float history_weight = 0.3f;

        /// Logprob threshold below which confidence drops.
        float logprob_threshold = -2.0f;

        /// Perplexity threshold above which confidence drops.
        float perplexity_threshold = 50.0f;
    };

    HeuristicScorer();
    explicit HeuristicScorer(const Config& config);

    ConfidenceScore preScore(const PreScoreSignals& signals) const override;
    ConfidenceScore postScore(
        const PreScoreSignals& pre_signals,
        const PostScoreSignals& post_signals
    ) const override;

    std::string name() const override { return "heuristic"; }

private:
    Config config_;
};

/// Threshold configuration for confidence-gated routing.
struct ConfidenceThresholds {
    float high = 0.85f;    // Above: local only, skip cloud
    float low = 0.4f;      // Below: cloud primary, local fallback
    // Between high and low: concurrent execution, arbiter chooses
};

}  // namespace harness
}  // namespace sparx
