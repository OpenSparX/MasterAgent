/**
 * @file request_validation.cpp
 * @brief Implements shared boundary, UTF-8 and Provider-result validation.
 *
 * Design mapping: Preprocessing design Sections 2.3,
 * 2.4 and 5.
 */

#include "include/preprocess_validation.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

namespace master_agent::preprocess {

std::string toString(StateDomain domain) {
    switch (domain) {
        case StateDomain::Vehicle:
            return "VEHICLE";
        case StateDomain::Environment:
            return "ENVIRONMENT";
    }
    return "UNKNOWN";
}

namespace detail {
namespace {

bool decodeUtf8Scalar(
    const std::string& input,
    std::size_t offset,
    std::uint32_t* scalar,
    std::size_t* width) {
    if (!scalar || !width || offset >= input.size()) {
        return false;
    }
    const auto byte0 =
        static_cast<unsigned char>(input[offset]);
    if (byte0 <= 0x7FU) {
        *scalar = byte0;
        *width = 1;
        return true;
    }

    std::size_t length = 0;
    std::uint32_t value = 0;
    if (byte0 >= 0xC2U && byte0 <= 0xDFU) {
        length = 2;
        value = byte0 & 0x1FU;
    } else if (byte0 >= 0xE0U && byte0 <= 0xEFU) {
        length = 3;
        value = byte0 & 0x0FU;
    } else if (byte0 >= 0xF0U && byte0 <= 0xF4U) {
        length = 4;
        value = byte0 & 0x07U;
    } else {
        return false;
    }
    if (length > input.size() - offset) {
        return false;
    }

    const auto byte1 =
        static_cast<unsigned char>(input[offset + 1]);
    if ((byte1 & 0xC0U) != 0x80U ||
        (byte0 == 0xE0U && byte1 < 0xA0U) ||
        (byte0 == 0xEDU && byte1 > 0x9FU) ||
        (byte0 == 0xF0U && byte1 < 0x90U) ||
        (byte0 == 0xF4U && byte1 > 0x8FU)) {
        return false;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation =
            static_cast<unsigned char>(input[offset + index]);
        if ((continuation & 0xC0U) != 0x80U) {
            return false;
        }
        value =
            (value << 6U) | (continuation & 0x3FU);
    }
    if (value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        return false;
    }
    *scalar = value;
    *width = length;
    return true;
}

bool isWhitespaceOrControl(std::uint32_t scalar) {
    return scalar <= 0x20U ||
           (scalar >= 0x7FU && scalar <= 0x9FU) ||
           scalar == 0x00A0U || scalar == 0x1680U ||
           (scalar >= 0x2000U && scalar <= 0x200AU) ||
           scalar == 0x2028U || scalar == 0x2029U ||
           scalar == 0x202FU || scalar == 0x205FU ||
           scalar == 0x3000U;
}

bool validPriority(TaskPriority priority) {
    return isValidTaskPriority(priority);
}

bool validTriggerType(const std::string& trigger_type) {
    return trigger_type == "TEXT_INPUT" ||
           trigger_type == "PERCEPTION_EVENT" ||
           trigger_type == "RULE_EVENT";
}

bool validStateDomain(StateDomain domain) {
    return domain == StateDomain::Vehicle ||
           domain == StateDomain::Environment;
}

}  // namespace

bool validUtf8(const std::string& input) {
    std::size_t offset = 0;
    while (offset < input.size()) {
        std::uint32_t scalar = 0;
        std::size_t width = 0;
        if (!decodeUtf8Scalar(
                input, offset, &scalar, &width)) {
            return false;
        }
        offset += width;
    }
    return true;
}

bool cleanUtf8(
    const std::string& input,
    std::size_t maximum_bytes,
    std::string* output) {
    if (!output) {
        return false;
    }
    output->clear();
    output->reserve(
        std::min(input.size(), maximum_bytes));
    bool pending_space = false;
    bool accepting_prefix = true;
    std::size_t offset = 0;
    while (offset < input.size()) {
        std::uint32_t scalar = 0;
        std::size_t width = 0;
        if (!decodeUtf8Scalar(
                input, offset, &scalar, &width)) {
            output->clear();
            return false;
        }
        if (isWhitespaceOrControl(scalar)) {
            if (!output->empty()) {
                pending_space = true;
            }
        } else if (accepting_prefix) {
            const std::size_t separator =
                pending_space ? 1U : 0U;
            if (separator <=
                    maximum_bytes - output->size() &&
                width <= maximum_bytes -
                    output->size() - separator) {
                if (pending_space) {
                    output->push_back(' ');
                }
                output->append(input, offset, width);
            } else {
                accepting_prefix = false;
            }
            pending_space = false;
        }
        offset += width;
    }
    return true;
}

bool validOpaqueId(
    const std::string& value,
    bool allow_empty) {
    if (value.empty()) {
        return allow_empty;
    }
    if (value.size() > kMaximumOpaqueIdBytes ||
        !validUtf8(value)) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < value.size()) {
        std::uint32_t scalar = 0;
        std::size_t width = 0;
        if (!decodeUtf8Scalar(
                value, offset, &scalar, &width) ||
            isWhitespaceOrControl(scalar)) {
            return false;
        }
        offset += width;
    }
    return true;
}

bool validRequestMetadata(
    const interaction::StandardRequest& request) {
    return validOpaqueId(request.request_id) &&
           validOpaqueId(request.trace_id) &&
           validOpaqueId(request.user_id) &&
           validOpaqueId(request.session_id) &&
           request.user_id.find(':') == std::string::npos &&
           request.session_id.find(':') == std::string::npos &&
           request.turn_id != 0 &&
           request.timestamp_utc_ms > 0 &&
           request.deadline_mono_ns > 0 &&
           validPriority(request.priority) &&
           request.trigger_type.size() <=
               kMaximumTriggerTypeBytes &&
           validTriggerType(request.trigger_type) &&
           validOpaqueId(request.resume_task_id, true);
}

bool validStateFieldName(const std::string& field) {
    if (field.empty() || field.size() > 64U) {
        return false;
    }
    if (field.front() < 'a' ||
        field.front() > 'z') {
        return false;
    }
    return std::all_of(
        field.begin(), field.end(),
        [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') ||
                   (byte >= '0' && byte <= '9') ||
                   byte == '_';
        });
}

bool validCapability(
    const StateCapability& capability) {
    if (!validStateDomain(capability.state_type) ||
        capability.fields.empty() ||
        capability.fields.size() > 64U) {
        return false;
    }
    std::set<std::string> unique;
    for (const auto& field : capability.fields) {
        if (!validStateFieldName(field) ||
            !unique.insert(field).second) {
            return false;
        }
    }
    return true;
}

bool validStateValue(const std::string& value) {
    if (value.empty() ||
        value.size() > kMaximumStateValueBytes) {
        return false;
    }
    std::string cleaned;
    return cleanUtf8(
               value, kMaximumStateValueBytes, &cleaned) &&
           cleaned == value;
}

Status validateCallBoundary(
    const CallContext& call,
    const std::shared_ptr<IRuntimeClock>& clock) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        return Status::Error(
            "preprocess",
            "PREPROCESS_CALLER_NOT_ALLOWED",
            "only AgentService may call preprocessing");
    }
    if (!clock) {
        return Status::Error(
            "preprocess", "PREPROCESS_NOT_READY",
            "preprocessing clock is not configured");
    }
    if (!validOpaqueId(call.request_id) ||
        !validOpaqueId(call.trace_id) ||
        !validOpaqueId(call.principal_id_hash) ||
        !validPriority(call.priority) ||
        call.deadline_mono_ns <= 0 ||
        (call.priority == TaskPriority::P0 &&
         !validOpaqueId(call.authorization_ref))) {
        return Status::Error(
            "preprocess",
            "PREPROCESS_CALL_CONTEXT_INVALID",
            "preprocessing call context is incomplete or malformed");
    }
    if (deadlineExpired(
            call.deadline_mono_ns, *clock)) {
        return Status::Error(
            "preprocess", "PREPROCESS_CALL_EXPIRED",
            "preprocessing call deadline has expired");
    }
    return Status::Ok();
}

Status validateStateQueryCallBoundary(
    const CallContext& call,
    const std::shared_ptr<IRuntimeClock>& clock) {
    if (!hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        return Status::Error(
            "preprocess",
            "PREPROCESS_CALLER_NOT_ALLOWED",
            "state queries require AgentService mediation");
    }
    return validateCallBoundary(call, clock);
}

Status providerFailure(
    const std::string& code,
    const std::string& summary) {
    return Status::Error(
        "preprocess", code, summary, false,
        SideEffectState::NotApplicable);
}

PreprocessResult invalidPreprocessResult(
    std::string safe_error) {
    PreprocessResult result;
    result.valid = false;
    result.error_message = std::move(safe_error);
    return result;
}

Result<StateQueryResult> sealProviderQueryResult(
    const StateQuery& query,
    const StateQueryResult& provider_result,
    std::int64_t now_utc_ms) {
    if (provider_result.values.size() >
            query.fields.size() ||
        provider_result.missing_fields.size() >
            query.fields.size()) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                "state Provider returned too many fields"));
    }
    if (provider_result.timestamp_utc_ms <= 0 ||
        provider_result.timestamp_utc_ms >
            now_utc_ms + kFreshFutureWindowMs) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                "state Provider returned an invalid timestamp"));
    }

    std::set<std::string> requested(
        query.fields.begin(), query.fields.end());
    std::set<std::string> represented;
    StateQueryResult sealed;
    sealed.timestamp_utc_ms =
        provider_result.timestamp_utc_ms;
    for (const auto& [field, value] :
         provider_result.values) {
        if (requested.count(field) == 0 ||
            !validStateValue(value) ||
            !represented.insert(field).second) {
            return Result<StateQueryResult>::Failure(
                providerFailure(
                    "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                    "state Provider returned an invalid field value"));
        }
        sealed.values.emplace(field, value);
    }
    for (const auto& field :
         provider_result.missing_fields) {
        if (requested.count(field) == 0 ||
            !represented.insert(field).second) {
            return Result<StateQueryResult>::Failure(
                providerFailure(
                    "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                    "state Provider returned invalid missing fields"));
        }
        sealed.missing_fields.push_back(field);
    }
    if (represented.size() != requested.size() ||
        provider_result.success !=
            provider_result.missing_fields.empty() ||
        (provider_result.success &&
         !provider_result.error_message.empty())) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_PROTOCOL_INVALID",
                "state Provider result is incomplete or contradictory"));
    }
    sealed.success = sealed.missing_fields.empty();
    if (!sealed.success) {
        sealed.error_message =
            "one or more requested fields are unavailable";
    }
    return Result<StateQueryResult>::Success(
        std::move(sealed));
}

}  // namespace detail
}  // namespace master_agent::preprocess
