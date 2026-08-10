#pragma once

/**
 * @file text_pipeline.h
 * @brief Private Cleaner, Normalizer, and TimeAligner component contracts.
 *
 * This header is private to Preprocess and is not part of the installed API.
 */

#include "preprocess_validation.h"

namespace master_agent::preprocess::detail {

/**
 * @brief Cleaner component from Section 2.2.
 *
 * Design mapping: Sections 2.2, 2.3, 4 and 5. It validates UTF-8, replaces
 * controls/whitespace, safely truncates text and drops empty parameters.
 */
class Cleaner {
public:
    Result<CleanedData> clean(
        const std::string& text,
        const std::map<std::string, std::string>& params) const;
};

/**
 * @brief Normalizer component from Section 2.2.
 *
 * Design mapping: Sections 2.2, 2.3, 4 and 5. It normalizes parameter names,
 * protects reserved fields and creates trigger/text event fields.
 */
class Normalizer {
public:
    Result<NormalizedData> normalize(
        const CleanedData& cleaned,
        const std::string& trigger_type) const;
};

/**
 * @brief TimeAligner component from Section 2.2.
 *
 * Design mapping: Sections 2.2, 2.3, 4 and 5. Freshness is computed from the
 * original timestamp, while the normalized request receives the safe value.
 */
class TimeAligner {
public:
    AlignedData align(
        std::int64_t request_timestamp_utc_ms,
        std::int64_t now_utc_ms,
        std::map<std::string, std::string> event_schema) const;
};

}  // namespace master_agent::preprocess::detail

