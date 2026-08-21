#pragma once
/**
 * @file sparx_arbiter.h
 * @brief Arbiter — local arbitration between on-device and cloud results.
 *
 * Receives results from both inference paths and selects the final output.
 * Strategies:
 *   - CloudPrefer: when both available, prefer cloud (stronger model)
 *   - LatencyFirst: use whichever arrives first within deadline
 *   - Confidence: use post-score to pick the more reliable result
 *
 * The arbiter also handles deadline enforcement: if a path hasn't returned
 * by the deadline, the available result is used immediately.
 */

#include "sparx_cloud_backend.h"
#include "sparx_confidence_scorer.h"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace sparx {
namespace harness {

// ─── Data Types ─────────────────────────────────────────────────────────────

/// Result from the local inference path.
struct LocalResult {
    bool success = false;
    std::string content;
    int latency_ms = 0;
    ConfidenceScore confidence;
    std::string error;
};

/// The final arbitration output.
struct ArbiterOutput {
    enum class Source { Local, Cloud, Fallback };

    std::string content;             // Final selected output
    Source source = Source::Local;   // Which path was selected
    std::string reason;              // Why this path was chosen
    int total_latency_ms = 0;       // Wall-clock from request start

    // Metadata for observability
    std::optional<LocalResult> local_result;
    std::optional<CloudResult> cloud_result;
};

/// Arbiter strategy enum (maps to config).
enum class ArbiterStrategy {
    CloudPrefer,     // Both available → prefer cloud
    LatencyFirst,    // Both available → prefer faster
    Confidence,      // Both available → prefer higher confidence
    LocalOnly,       // Never use cloud (override / offline mode)
};

/// Arbiter configuration.
struct ArbiterConfig {
    ArbiterStrategy strategy = ArbiterStrategy::CloudPrefer;

    /// Maximum time to wait for any result path.
    int deadline_ms = 3000;

    /// Per-intent deadline overrides (intent_type → deadline_ms).
    std::unordered_map<std::string, int> intent_deadlines;

    /// Minimum confidence gap to prefer one result over another.
    /// Only used with Confidence strategy.
    float confidence_gap_threshold = 0.15f;

    /// Fallback when both paths fail.
    std::string fallback_message = "I'm unable to process this request right now.";
};

// ─── Interface ──────────────────────────────────────────────────────────────

/// Abstract arbiter interface.
class IArbiter {
public:
    virtual ~IArbiter() = default;

    /// Arbitrate between local and cloud results.
    /// Either result may be absent (nullopt) if the path failed or timed out.
    virtual ArbiterOutput arbitrate(
        const std::optional<LocalResult>& local,
        const std::optional<CloudResult>& cloud,
        const std::string& intent_type = ""
    ) const = 0;

    /// Get the effective deadline for a given intent type.
    virtual int getDeadline(const std::string& intent_type = "") const = 0;

    /// Strategy name (for tracing).
    virtual std::string name() const = 0;
};

// ─── Implementations ────────────────────────────────────────────────────────

/// Cloud-prefer arbiter: when both are available, pick cloud.
class CloudPreferArbiter : public IArbiter {
public:
    explicit CloudPreferArbiter(const ArbiterConfig& config);

    ArbiterOutput arbitrate(
        const std::optional<LocalResult>& local,
        const std::optional<CloudResult>& cloud,
        const std::string& intent_type = ""
    ) const override;

    int getDeadline(const std::string& intent_type = "") const override;
    std::string name() const override { return "cloud_prefer"; }

private:
    ArbiterConfig config_;
};

/// Latency-first arbiter: pick whichever is available (arrived first).
class LatencyFirstArbiter : public IArbiter {
public:
    explicit LatencyFirstArbiter(const ArbiterConfig& config);

    ArbiterOutput arbitrate(
        const std::optional<LocalResult>& local,
        const std::optional<CloudResult>& cloud,
        const std::string& intent_type = ""
    ) const override;

    int getDeadline(const std::string& intent_type = "") const override;
    std::string name() const override { return "latency_first"; }

private:
    ArbiterConfig config_;
};

/// Confidence-based arbiter: pick the result with higher confidence.
class ConfidenceArbiter : public IArbiter {
public:
    explicit ConfidenceArbiter(const ArbiterConfig& config);

    ArbiterOutput arbitrate(
        const std::optional<LocalResult>& local,
        const std::optional<CloudResult>& cloud,
        const std::string& intent_type = ""
    ) const override;

    int getDeadline(const std::string& intent_type = "") const override;
    std::string name() const override { return "confidence"; }

private:
    ArbiterConfig config_;
};

// ─── Factory ────────────────────────────────────────────────────────────────

/// Create an arbiter from strategy enum.
std::unique_ptr<IArbiter> createArbiter(const ArbiterConfig& config);

}  // namespace harness
}  // namespace sparx
