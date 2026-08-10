/**
 * @file cleaner.cpp
 * @brief Implements Cleaner text and parameter sanitation.
 *
 * Design mapping: Sections 2.2, 2.3, 4 and 5.
 */

#include "include/text_pipeline.h"

#include <utility>

namespace master_agent::preprocess::detail {

Result<CleanedData> Cleaner::clean(
    const std::string& text,
    const std::map<std::string, std::string>& params) const {
    if (text.size() > kMaximumIngressTextBytes) {
        return Result<CleanedData>::Failure(Status::Error(
            "preprocess", "PREPROCESS_TEXT_TOO_LARGE",
            "input text exceeds the ingress limit"));
    }
    if (params.size() > kMaximumParameterCount) {
        return Result<CleanedData>::Failure(Status::Error(
            "preprocess", "PREPROCESS_TOO_MANY_PARAMETERS",
            "too many input parameters"));
    }

    CleanedData cleaned;
    if (!cleanUtf8(
            text, kMaximumNormalizedTextBytes,
            &cleaned.text)) {
        return Result<CleanedData>::Failure(Status::Error(
            "preprocess", "PREPROCESS_TEXT_UTF8_INVALID",
            "input text is not valid UTF-8"));
    }

    std::size_t total_bytes = 0;
    for (const auto& [raw_key, raw_value] : params) {
        if (raw_key.size() >
                kMaximumParameterTotalBytes - total_bytes ||
            raw_value.size() >
                kMaximumParameterTotalBytes -
                    total_bytes - raw_key.size()) {
            return Result<CleanedData>::Failure(Status::Error(
                "preprocess",
                "PREPROCESS_PARAMETER_BYTES_EXCEEDED",
                "input parameter bytes exceed the limit"));
        }
        total_bytes += raw_key.size() + raw_value.size();

        // Section 5 requires empty parameter names or values to be
        // filtered, not promoted into a request-level failure.
        if (raw_key.empty() || raw_value.empty()) {
            continue;
        }
        if (!validUtf8(raw_key) ||
            !validUtf8(raw_value)) {
            return Result<CleanedData>::Failure(Status::Error(
                "preprocess",
                "PREPROCESS_PARAMETER_UTF8_INVALID",
                "input parameter is not valid UTF-8"));
        }
        if (raw_key.size() >
            kMaximumParameterKeyBytes) {
            return Result<CleanedData>::Failure(Status::Error(
                "preprocess",
                "PREPROCESS_PARAMETER_KEY_TOO_LARGE",
                "input parameter key exceeds the limit"));
        }
        if (raw_value.size() >
            kMaximumParameterValueBytes) {
            return Result<CleanedData>::Failure(Status::Error(
                "preprocess",
                "PREPROCESS_PARAMETER_VALUE_TOO_LARGE",
                "input parameter value exceeds the limit"));
        }

        std::string key;
        std::string value;
        if (!cleanUtf8(
                raw_key, kMaximumParameterKeyBytes, &key) ||
            !cleanUtf8(
                raw_value, kMaximumParameterValueBytes,
                &value)) {
            return Result<CleanedData>::Failure(Status::Error(
                "preprocess",
                "PREPROCESS_PARAMETER_CLEAN_FAILED",
                "input parameter normalization failed"));
        }
        if (key.empty() || value.empty()) {
            continue;
        }
        cleaned.params.emplace(
            std::move(key), std::move(value));
    }
    return Result<CleanedData>::Success(
        std::move(cleaned));
}

}  // namespace master_agent::preprocess::detail
