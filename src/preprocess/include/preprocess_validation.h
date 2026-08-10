#pragma once

/**
 * @file preprocess_validation.h
 * @brief Private preprocessing bounds, value objects, and boundary validation contracts.
 *
 * This header is private to Preprocess and is not part of the installed API.
 */

#include "master_agent/preprocess/preprocess_engine.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace master_agent::preprocess::detail {

inline constexpr std::size_t kMaximumIngressTextBytes = 16U * 1024U;
inline constexpr std::size_t kMaximumNormalizedTextBytes = 2048U;
inline constexpr std::size_t kMaximumOpaqueIdBytes = 256U;
inline constexpr std::size_t kMaximumTriggerTypeBytes = 32U;
inline constexpr std::size_t kMaximumParameterCount = 64U;
inline constexpr std::size_t kMaximumParameterKeyBytes = 128U;
inline constexpr std::size_t kMaximumParameterValueBytes = 2048U;
inline constexpr std::size_t kMaximumParameterTotalBytes = 16U * 1024U;
inline constexpr std::size_t kMaximumQueryFieldCount = 32U;
inline constexpr std::size_t kMaximumStateValueBytes = 256U;
inline constexpr std::int64_t kAlignmentWindowMs = 1000;
inline constexpr std::int64_t kFreshPastWindowMs = 5000;
inline constexpr std::int64_t kFreshFutureWindowMs = 1000;

struct CleanedData {
    std::string text;
    std::map<std::string, std::string> params;
};

struct NormalizedData {
    std::string text;
    std::map<std::string, std::string> params;
    std::map<std::string, std::string> event_schema;
};

struct AlignedData {
    std::int64_t aligned_timestamp_utc_ms = 0;
    bool is_fresh = false;
    std::map<std::string, std::string> event_schema;
};

bool validUtf8(const std::string& input);
bool cleanUtf8(
    const std::string& input,
    std::size_t maximum_bytes,
    std::string* output);
bool validOpaqueId(
    const std::string& value, bool allow_empty = false);
bool validRequestMetadata(
    const interaction::StandardRequest& request);
bool validStateFieldName(const std::string& field);
bool validCapability(const StateCapability& capability);
bool validStateValue(const std::string& value);

Status validateCallBoundary(
    const CallContext& call,
    const std::shared_ptr<IRuntimeClock>& clock);

Status validateStateQueryCallBoundary(
    const CallContext& call,
    const std::shared_ptr<IRuntimeClock>& clock);

Status providerFailure(
    const std::string& code, const std::string& summary);

PreprocessResult invalidPreprocessResult(std::string safe_error);

Result<StateQueryResult> sealProviderQueryResult(
    const StateQuery& query,
    const StateQueryResult& provider_result,
    std::int64_t now_utc_ms);

}  // namespace master_agent::preprocess::detail

