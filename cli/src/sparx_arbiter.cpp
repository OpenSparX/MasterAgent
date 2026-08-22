/**
 * @file sparx_arbiter.cpp
 * @brief Arbiter implementations — local arbitration between device and cloud results.
 *
 * Arbitration principles for cockpit scenarios:
 *   1. Never block longer than the deadline (time-sensitive)
 *   2. Prefer the result with higher quality when both are available
 *   3. Always have a usable output (graceful degradation)
 */

#include "sparx_arbiter.h"

#include <algorithm>

namespace sparx {
namespace harness {

// ─── Helper ─────────────────────────────────────────────────────────────────

namespace {

ArbiterOutput makeLocalOutput(const LocalResult& local, const std::string& reason) {
    ArbiterOutput out;
    out.content = local.content;
    out.source = ArbiterOutput::Source::Local;
    out.reason = reason;
    out.total_latency_ms = local.latency_ms;
    out.local_result = local;
    return out;
}

ArbiterOutput makeCloudOutput(const CloudResult& cloud, const std::string& reason) {
    ArbiterOutput out;
    out.content = cloud.content;
    out.source = ArbiterOutput::Source::Cloud;
    out.reason = reason;
    out.total_latency_ms = cloud.latency_ms;
    out.cloud_result = cloud;
    return out;
}

ArbiterOutput makeFallback(const std::string& message,
                           const std::optional<LocalResult>& local,
                           const std::optional<CloudResult>& cloud) {
    ArbiterOutput out;
    out.content = message;
    out.source = ArbiterOutput::Source::Fallback;
    out.reason = "both paths failed";
    out.local_result = local;
    out.cloud_result = cloud;
    return out;
}

int lookupDeadline(const ArbiterConfig& config, const std::string& intent_type) {
    if (!intent_type.empty()) {
        auto it = config.intent_deadlines.find(intent_type);
        if (it != config.intent_deadlines.end()) {
            return it->second;
        }
    }
    return config.deadline_ms;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
// CloudPreferArbiter
// ═══════════════════════════════════════════════════════════════════════════════

CloudPreferArbiter::CloudPreferArbiter(const ArbiterConfig& config) : config_(config) {}

ArbiterOutput CloudPreferArbiter::arbitrate(
    const std::optional<LocalResult>& local,
    const std::optional<CloudResult>& cloud,
    const std::string& intent_type) const {

    bool has_local = local.has_value() && local->success;
    bool has_cloud = cloud.has_value() && cloud->success;

    // Both available → prefer cloud (stronger model)
    if (has_local && has_cloud) {
        return makeCloudOutput(*cloud, "cloud_prefer: both available, selecting cloud");
    }

    // Only cloud available
    if (has_cloud) {
        return makeCloudOutput(*cloud, "cloud_prefer: only cloud available");
    }

    // Only local available
    if (has_local) {
        return makeLocalOutput(*local, "cloud_prefer: only local available (cloud failed/timeout)");
    }

    // Neither available → fallback
    return makeFallback(config_.fallback_message, local, cloud);
}

int CloudPreferArbiter::getDeadline(const std::string& intent_type) const {
    return lookupDeadline(config_, intent_type);
}

// ═══════════════════════════════════════════════════════════════════════════════
// LatencyFirstArbiter
// ═══════════════════════════════════════════════════════════════════════════════

LatencyFirstArbiter::LatencyFirstArbiter(const ArbiterConfig& config) : config_(config) {}

ArbiterOutput LatencyFirstArbiter::arbitrate(
    const std::optional<LocalResult>& local,
    const std::optional<CloudResult>& cloud,
    const std::string& intent_type) const {

    bool has_local = local.has_value() && local->success;
    bool has_cloud = cloud.has_value() && cloud->success;

    // Both available → pick faster
    if (has_local && has_cloud) {
        if (local->latency_ms <= cloud->latency_ms) {
            return makeLocalOutput(*local, "latency_first: local faster (" +
                std::to_string(local->latency_ms) + "ms vs " +
                std::to_string(cloud->latency_ms) + "ms)");
        } else {
            return makeCloudOutput(*cloud, "latency_first: cloud faster (" +
                std::to_string(cloud->latency_ms) + "ms vs " +
                std::to_string(local->latency_ms) + "ms)");
        }
    }

    if (has_local) {
        return makeLocalOutput(*local, "latency_first: only local available");
    }
    if (has_cloud) {
        return makeCloudOutput(*cloud, "latency_first: only cloud available");
    }

    return makeFallback(config_.fallback_message, local, cloud);
}

int LatencyFirstArbiter::getDeadline(const std::string& intent_type) const {
    return lookupDeadline(config_, intent_type);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ConfidenceArbiter
// ═══════════════════════════════════════════════════════════════════════════════

ConfidenceArbiter::ConfidenceArbiter(const ArbiterConfig& config) : config_(config) {}

ArbiterOutput ConfidenceArbiter::arbitrate(
    const std::optional<LocalResult>& local,
    const std::optional<CloudResult>& cloud,
    const std::string& intent_type) const {

    bool has_local = local.has_value() && local->success;
    bool has_cloud = cloud.has_value() && cloud->success;

    if (has_local && has_cloud) {
        float local_conf = local->confidence.overall;
        // Cloud gets a default confidence (cloud models are generally stronger)
        float cloud_conf = 0.8f;

        float gap = cloud_conf - local_conf;

        if (gap > config_.confidence_gap_threshold) {
            return makeCloudOutput(*cloud, "confidence: cloud wins (gap=" +
                std::to_string(static_cast<int>(gap * 100)) + "%)");
        } else if (-gap > config_.confidence_gap_threshold) {
            return makeLocalOutput(*local, "confidence: local wins (conf=" +
                std::to_string(static_cast<int>(local_conf * 100)) + "%)");
        } else {
            // Within gap threshold — prefer cloud (tie-breaker)
            return makeCloudOutput(*cloud, "confidence: tie, defaulting to cloud");
        }
    }

    if (has_local) {
        return makeLocalOutput(*local, "confidence: only local available");
    }
    if (has_cloud) {
        return makeCloudOutput(*cloud, "confidence: only cloud available");
    }

    return makeFallback(config_.fallback_message, local, cloud);
}

int ConfidenceArbiter::getDeadline(const std::string& intent_type) const {
    return lookupDeadline(config_, intent_type);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Factory
// ═══════════════════════════════════════════════════════════════════════════════

std::unique_ptr<IArbiter> createArbiter(const ArbiterConfig& config) {
    switch (config.strategy) {
        case ArbiterStrategy::CloudPrefer:
            return std::make_unique<CloudPreferArbiter>(config);
        case ArbiterStrategy::LatencyFirst:
            return std::make_unique<LatencyFirstArbiter>(config);
        case ArbiterStrategy::Confidence:
            return std::make_unique<ConfidenceArbiter>(config);
        case ArbiterStrategy::LocalOnly:
            // LocalOnly uses CloudPrefer but cloud never fires
            return std::make_unique<CloudPreferArbiter>(config);
    }
    return std::make_unique<CloudPreferArbiter>(config);
}

}  // namespace harness
}  // namespace sparx
