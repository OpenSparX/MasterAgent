/**
 * @file time_aligner.cpp
 * @brief Implements request-time alignment and freshness classification.
 *
 * Design mapping: Sections 2.2, 2.3, 4 and 5.
 */

#include "include/text_pipeline.h"

#include <utility>

namespace master_agent::preprocess::detail {

AlignedData TimeAligner::align(
    std::int64_t request_timestamp_utc_ms,
    std::int64_t now_utc_ms,
    std::map<std::string, std::string> event_schema) const {
    const auto safe_past =
        now_utc_ms > kAlignmentWindowMs
            ? now_utc_ms - kAlignmentWindowMs
            : 1;
    std::int64_t aligned = request_timestamp_utc_ms;
    if (request_timestamp_utc_ms < safe_past) {
        aligned = safe_past;
    } else if (
        request_timestamp_utc_ms >
        now_utc_ms + kAlignmentWindowMs) {
        aligned = now_utc_ms;
    }

    const auto fresh_past =
        now_utc_ms > kFreshPastWindowMs
            ? now_utc_ms - kFreshPastWindowMs
            : 1;
    const bool fresh =
        request_timestamp_utc_ms >= fresh_past &&
        request_timestamp_utc_ms <=
            now_utc_ms + kFreshFutureWindowMs;

    event_schema["aligned_timestamp"] =
        std::to_string(aligned);
    event_schema["processing_time"] =
        std::to_string(now_utc_ms);
    event_schema["is_fresh"] =
        fresh ? "true" : "false";
    if (!fresh) {
        event_schema["stale_warning"] = "true";
    }

    AlignedData result;
    result.aligned_timestamp_utc_ms = aligned;
    result.is_fresh = fresh;
    result.event_schema = std::move(event_schema);
    return result;
}

}  // namespace master_agent::preprocess::detail
