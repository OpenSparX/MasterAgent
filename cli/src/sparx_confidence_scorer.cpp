/**
 * @file sparx_confidence_scorer.cpp
 * @brief Confidence Scorer — heuristic confidence estimation for routing decisions.
 *
 * The scorer provides two-phase confidence:
 *   Pre-score: fast heuristic to decide whether to fire cloud path
 *   Post-score: quality assessment of local inference output
 *
 * Scoring philosophy for cockpit scenario:
 *   - Deterministic intents (vehicle control, media) → high confidence
 *   - Well-known patterns with good history → medium-high confidence
 *   - Open-ended / novel queries → low confidence → fire cloud
 */

#include "sparx_confidence_scorer.h"

#include <algorithm>
#include <cmath>

namespace sparx {
namespace harness {

// ═══════════════════════════════════════════════════════════════════════════════
// HeuristicScorer
// ═══════════════════════════════════════════════════════════════════════════════

HeuristicScorer::HeuristicScorer() : config_{} {}

HeuristicScorer::HeuristicScorer(const Config& config) : config_(config) {}

ConfidenceScore HeuristicScorer::preScore(const PreScoreSignals& signals) const {
    ConfidenceScore score;
    float s = 0.0f;
    std::string reason;

    // Deterministic intents are always high confidence
    if (signals.is_deterministic) {
        score.overall = 0.95f;
        score.pre_score = 0.95f;
        score.reason = "deterministic intent (skill engine)";
        return score;
    }

    // Base confidence from intent type hit rate
    s += config_.history_weight * signals.speculative_hit_rate;

    // Historical success/failure ratio
    int total = signals.similar_intent_successes + signals.similar_intent_failures;
    if (total > 0) {
        float success_rate = static_cast<float>(signals.similar_intent_successes) / total;
        s += 0.3f * success_rate;
        reason += "history_rate=" + std::to_string(static_cast<int>(success_rate * 100)) + "% ";
    }

    // Input complexity penalty: longer inputs are harder for local model
    if (signals.input_token_count > 200) {
        float penalty = std::min(0.3f,
            0.1f * (static_cast<float>(signals.input_token_count - 200) / 300.0f));
        s -= penalty;
        reason += "long_input ";
    } else {
        s += 0.2f;  // Short inputs are easier
    }

    // Base confidence floor
    s += 0.2f;

    score.overall = std::clamp(s, 0.0f, 1.0f);
    score.pre_score = score.overall;
    score.reason = reason.empty() ? "heuristic pre-score" : reason;
    return score;
}

ConfidenceScore HeuristicScorer::postScore(
    const PreScoreSignals& pre_signals,
    const PostScoreSignals& post_signals) const {

    // Start from pre-score
    auto score = preScore(pre_signals);
    float post = 0.0f;
    std::string reason;

    // Log probability assessment
    if (post_signals.avg_logprob != 0.0f) {
        if (post_signals.avg_logprob > config_.logprob_threshold) {
            post += 0.4f;  // Good confidence from model
        } else {
            post += 0.1f;  // Model is uncertain
            reason += "low_logprob ";
        }

        // Minimum logprob check (worst token)
        if (post_signals.min_logprob < config_.logprob_threshold * 2.0f) {
            post -= 0.1f;
            reason += "very_uncertain_token ";
        }
    } else {
        // No logprob available, use output length as proxy
        if (post_signals.output_token_count > 5 &&
            post_signals.output_token_count < 500) {
            post += 0.3f;  // Reasonable length
        }
    }

    // Perplexity check
    if (post_signals.perplexity > 0.0f) {
        if (post_signals.perplexity < config_.perplexity_threshold) {
            post += 0.2f;
        } else {
            post -= 0.2f;
            reason += "high_perplexity ";
        }
    }

    // Penalty for truncation
    if (post_signals.truncated) {
        post -= 0.2f;
        reason += "truncated ";
    }

    // Penalty for repetition
    if (post_signals.has_repetition) {
        post -= 0.3f;
        reason += "repetitive ";
    }

    // Format validity bonus
    if (post_signals.format_valid) {
        post += 0.1f;
    } else {
        post -= 0.15f;
        reason += "bad_format ";
    }

    score.post_score = std::clamp(post, 0.0f, 1.0f);

    // Combined: weighted average of pre and post
    score.overall = 0.3f * score.pre_score + 0.7f * score.post_score;
    score.overall = std::clamp(score.overall, 0.0f, 1.0f);
    score.reason = reason.empty() ? "post-score OK" : reason;

    return score;
}

}  // namespace harness
}  // namespace sparx
