/**
 * @file failure_mapping.cpp
 * @brief Maps internal failures to bounded client-safe turn results.
 */

#include "include/plan_response.h"
#include "include/capability_policy.h"
#include "include/client_error_sanitization.h"

namespace master_agent::agent_service {

TurnResult AgentService::failureResult(
    const interaction::StandardRequest& request, const Status& status,
    const std::string& source_module, const std::string& operation) {

    TurnResult result;
    result.request_id = request.request_id;
    result.trace_id = request.trace_id;
    result.session_id = request.session_id;
    result.turn_id = request.turn_id;
    result.reply = u8"抱歉，本次请求暂时无法完成，请稍后再试。";
    result.success = false;
    result.error_code =
        normalizedErrorCode(status.error.code);
    result.error_message = safeClientErrorMessage();
    result.turn_summary = "failed";
    if (exceptions_ && ids_ && clock_) {
        reportFailure(request, status.error, source_module, operation);
    }
    if (log_ && ids_ && clock_) {
        logEvent(request, "TURN_FAILED", operation, "failed",
                 data_log::EventSeverity::Error,
                 data_log::DurabilityClass::D3Fsynced, {},
                 normalizedErrorCode(status.error.code));
    }
    return result;
}

}  // namespace master_agent::agent_service
