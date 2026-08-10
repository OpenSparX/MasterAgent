/**
 * @file normalizer.cpp
 * @brief Implements parameter-name and event-field normalization.
 *
 * Design mapping: Sections 2.2, 2.3, 4 and 5.
 */

#include "include/text_pipeline.h"

#include <set>
#include <utility>

namespace master_agent::preprocess::detail {
namespace {

std::string normalizedTriggerType(
    const std::string& trigger_type) {
    if (trigger_type == "TEXT_INPUT") {
        return "text_input";
    }
    if (trigger_type == "PERCEPTION_EVENT") {
        return "perception_event";
    }
    if (trigger_type == "RULE_EVENT") {
        return "rule_event";
    }
    return "unknown";
}

bool isReservedParameter(const std::string& key) {
    static const std::set<std::string> reserved{
        "request_id", "trace_id", "session_id", "user_id",
        "turn_id", "timestamp", "timestamp_utc_ms",
        "deadline_mono_ns", "trigger_type", "priority",
        "principal_id_hash", "authorization_ref", "is_fresh",
        "aligned_timestamp", "processing_time", "text"};
    return reserved.count(key) != 0;
}

bool normalizeParameterKey(
    const std::string& raw,
    std::string* normalized) {
    if (!normalized ||
        raw.size() > kMaximumParameterKeyBytes) {
        return false;
    }
    normalized->clear();
    normalized->reserve(raw.size());
    for (const unsigned char byte : raw) {
        if (byte >= 'A' && byte <= 'Z') {
            if (!normalized->empty() &&
                normalized->back() != '_') {
                normalized->push_back('_');
            }
            normalized->push_back(
                static_cast<char>(
                    byte + ('a' - 'A')));
        } else if (
            byte == ' ' || byte == '-' || byte == '_') {
            if (!normalized->empty() &&
                normalized->back() != '_') {
                normalized->push_back('_');
            }
        } else {
            normalized->push_back(
                static_cast<char>(byte));
        }
        if (normalized->size() >
            kMaximumParameterKeyBytes) {
            normalized->clear();
            return false;
        }
    }
    while (!normalized->empty() &&
           normalized->back() == '_') {
        normalized->pop_back();
    }
    return true;
}

}  // namespace

Result<NormalizedData> Normalizer::normalize(
    const CleanedData& cleaned,
    const std::string& trigger_type) const {
    NormalizedData normalized;
    normalized.text = cleaned.text;
    for (const auto& [raw_key, value] :
         cleaned.params) {
        std::string key;
        if (!normalizeParameterKey(raw_key, &key)) {
            return Result<NormalizedData>::Failure(
                Status::Error(
                    "preprocess",
                    "PREPROCESS_PARAMETER_NAME_INVALID",
                    "input parameter name normalization failed"));
        }
        if (key.empty() ||
            isReservedParameter(key)) {
            continue;
        }
        if (!normalized.params.emplace(
                 std::move(key), value).second) {
            return Result<NormalizedData>::Failure(
                Status::Error(
                    "preprocess",
                    "PREPROCESS_PARAMETER_NAME_COLLISION",
                    "input parameter names collide after normalization"));
        }
    }

    normalized.event_schema["text"] = normalized.text;
    normalized.event_schema["trigger_type"] =
        normalizedTriggerType(trigger_type);
    return Result<NormalizedData>::Success(
        std::move(normalized));
}

}  // namespace master_agent::preprocess::detail
