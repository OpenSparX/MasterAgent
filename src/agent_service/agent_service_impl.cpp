/**
 * @file agent_service_impl.cpp
 * @brief Owns service construction and the exception-safe public turn boundary.
 */

#include "include/plan_response.h"
#include "include/capability_policy.h"
#include "include/client_error_sanitization.h"

namespace master_agent::agent_service {

AgentService::AgentService(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<preprocess::IPreprocessEngine> preprocess,
    std::shared_ptr<memory::IMemoryService> memory,
    std::shared_ptr<intent::IIntentEngine> intent,
    std::shared_ptr<orchestrator::IOrchestrator> orchestrator,
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<data_log::IDataLogService> log,
    std::shared_ptr<exception::IExceptionManager> exceptions)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      preprocess_(std::move(preprocess)),
      memory_(std::move(memory)),
      intent_(std::move(intent)),
      orchestrator_(std::move(orchestrator)),
      atomic_(std::move(atomic)),
      log_(std::move(log)),
      exceptions_(std::move(exceptions)),
      producer_epoch_(nextProducerEpoch()) {}

// This is the process-level exception boundary for a user turn. No exception
// may escape to ingress; containment still preserves Unknown side-effect truth.
TurnResult AgentService::runTurn(
    const interaction::StandardRequest& request,
    const CallContext& call) {
    try {
        return runTurnImpl(request, call);
    } catch (...) {
        const auto failure = Status::Error(
            "agent_service", "AGENT_SERVICE_UNHANDLED_EXCEPTION",
            "an internal module failed outside its declared result contract",
            false, SideEffectState::Unknown);
        try {
            if (clock_ && ids_ && log_ && exceptions_) {
                return failureResult(
                    request, failure, "agent_service", "runTurn");
            }
        } catch (...) {
            // The containment path itself must never reopen the host process
            // boundary.
        }
        TurnResult result;
        result.request_id = request.request_id;
        result.trace_id = request.trace_id;
        result.session_id = request.session_id;
        result.turn_id = request.turn_id;
        result.success = false;
        result.error_code =
            normalizedErrorCode(failure.error.code);
        result.error_message = safeClientErrorMessage();
        result.turn_summary = "failed";
        return result;
    }
}

// A turn is coordinated as one bounded transaction: validate identity, obtain
// frozen context, resolve intent, commit or observe execution, persist memory,
// and emit the client-safe result.

}  // namespace master_agent::agent_service
