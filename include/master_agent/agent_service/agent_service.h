#pragma once

/**
 * @file agent_service.h
 * @brief Defines turn-level coordination interfaces.
 */

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"
#include "master_agent/intent/intent_engine.h"
#include "master_agent/interaction/interaction_layer.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/orchestrator/orchestrator.h"
#include "master_agent/preprocess/preprocess_engine.h"

namespace master_agent::agent_service {

/**
 * @brief Closed result returned to the interaction boundary.
 *
 * pending means that a plan was durably accepted but is not terminal.
 * plan_state == Unknown means that an external side effect cannot yet be
 * confirmed; callers must reconcile by plan_id instead of resubmitting.
 */
struct TurnResult {
    std::string request_id;
    std::string trace_id;
    std::string session_id;
    std::uint64_t turn_id = 0;
    std::string reply;
    bool success = false;
    bool pending = false;
    std::string error_code;
    std::string error_message;
    std::string plan_id;
    std::optional<orchestrator::PlanState> plan_state;
    std::string turn_summary;
};

/**
 * @brief Coordinates one complete user turn across all governed modules.
 *
 * Only InteractionIngress may submit a turn. Direct replies and clarification
 * requests return without a plan; executable outcomes are delegated to the
 * orchestrator and reported through TurnResult.
 */
class IAgentService {
public:
    virtual ~IAgentService() = default;

    /**
     * @brief Executes one governed turn.
     * @param request Frozen request produced by InteractionLayer.
     * @param call Authenticated ingress identity matching the request.
     * @return A user-facing reply plus machine-readable plan and error state.
     *
     * Model text is never returned directly. Intent output is parsed into a
     * closed outcome before AgentService creates the final reply.
     */
    virtual TurnResult runTurn(
        const interaction::StandardRequest& request,
        const CallContext& call) = 0;
};

class AgentService final : public IAgentService {
public:
    AgentService(
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::shared_ptr<preprocess::IPreprocessEngine> preprocess,
        std::shared_ptr<memory::IMemoryService> memory,
        std::shared_ptr<intent::IIntentEngine> intent,
        std::shared_ptr<orchestrator::IOrchestrator> orchestrator,
        std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
        std::shared_ptr<data_log::IDataLogService> log,
        std::shared_ptr<exception::IExceptionManager> exceptions);

    TurnResult runTurn(
        const interaction::StandardRequest& request,
        const CallContext& call) override;

private:
    TurnResult runTurnImpl(
        const interaction::StandardRequest& request,
        const CallContext& call);

    void logEvent(const interaction::StandardRequest& request,
                  const std::string& event_type,
                  const std::string& operation,
                  const std::string& outcome,
                  data_log::EventSeverity severity,
                  data_log::DurabilityClass durability,
                  const std::string& plan_id = {},
                  const std::string& error_ref = {});

    void reportFailure(const interaction::StandardRequest& request,
                       const StructuredError& error,
                       const std::string& source_module,
                       const std::string& operation,
                       const std::string& plan_id = {});

    TurnResult failureResult(
        const interaction::StandardRequest& request,
        const Status& status,
        const std::string& source_module,
        const std::string& operation);

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<preprocess::IPreprocessEngine> preprocess_;
    std::shared_ptr<memory::IMemoryService> memory_;
    std::shared_ptr<intent::IIntentEngine> intent_;
    std::shared_ptr<orchestrator::IOrchestrator> orchestrator_;
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic_;
    std::shared_ptr<data_log::IDataLogService> log_;
    std::shared_ptr<exception::IExceptionManager> exceptions_;
    std::uint64_t producer_epoch_ = 1;
    mutable std::mutex log_producer_mutex_;
    mutable std::mutex exception_producer_mutex_;
    std::atomic<std::uint64_t> log_observation_sequence_{1};
    std::atomic<std::uint64_t> exception_observation_sequence_{1};
};

}  // namespace master_agent::agent_service
