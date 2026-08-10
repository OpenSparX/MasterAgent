#pragma once

/**
 * @file master_agent_runtime.h
 * @brief Defines the default single-process composition root.
 */

#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include "master_agent/agent_dispatch/agent_dispatch.h"
#include "master_agent/agent_service/agent_service.h"
#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"
#include "master_agent/data_log/data_log_service.h"
#include "master_agent/exception/exception_manager.h"
#include "master_agent/inference/inference_framework.h"
#include "master_agent/intent/intent_engine.h"
#include "master_agent/interaction/interaction_layer.h"
#include "master_agent/kv_cache/kv_cache_manager.h"
#include "master_agent/memory/memory_service.h"
#include "master_agent/orchestrator/orchestrator.h"
#include "master_agent/preprocess/preprocess_engine.h"
#include "master_agent/prompt/prompt_engine.h"
#include "master_agent/skill/skill_engine.h"
#include "master_agent/sub_agents/sub_agent.h"

namespace master_agent::runtime {

/**
 * @brief Owns and wires every module in the default deployment.
 *
 * The runtime is a composition root, not a policy owner. It creates modules in
 * dependency order, injects local adapters, gates new ingress during shutdown,
 * and flushes durable observability state. Business state remains owned by the
 * individual module state machines.
 *
 * The single-process deployment preserves the same asynchronous acceptance,
 * query, terminal-event, priority, preemption, idempotency, and reconciliation
 * contracts used by an optional IPC deployment.
 */
class MasterAgentRuntime final {
public:
    /**
     * @brief Creates and initializes an isolated runtime.
     * @param runtime_directory Root for journals, snapshots, and test state.
     * @param clock Injectable clock; a system clock is used when null.
     * @param simulated_work_units Bounded work per mock model/provider call.
     */
    static Result<std::shared_ptr<MasterAgentRuntime>> create(
        const std::filesystem::path& runtime_directory,
        std::shared_ptr<IRuntimeClock> clock = nullptr,
        std::uint32_t simulated_work_units = 1);

    ~MasterAgentRuntime();

    MasterAgentRuntime(const MasterAgentRuntime&) = delete;
    MasterAgentRuntime& operator=(const MasterAgentRuntime&) = delete;

    /**
     * @brief Submits text through InteractionIngress.
     *
     * Calls for the same user/session are serialized before durable turn-id
     * allocation. Calls for independent sessions may proceed concurrently.
     */
    agent_service::TurnResult submitText(
        const interaction::TextInput& input);

    /**
     * @brief Stops accepting ingress and flushes durable log state.
     *
     * This method is idempotent. Component destructors then release resources
     * in reverse dependency order.
     */
    Status shutdown();

    std::shared_ptr<interaction::InteractionLayer> interaction() const;
    std::shared_ptr<preprocess::PreprocessEngine> preprocess() const;
    std::shared_ptr<memory::MemoryService> memory() const;
    std::shared_ptr<intent::IntentEngine> intent() const;
    std::shared_ptr<agent_service::AgentService> agentService() const;
    std::shared_ptr<orchestrator::Orchestrator> orchestrator() const;
    std::shared_ptr<atomic_service::AtomicServiceManager> atomic() const;
    std::shared_ptr<inference::InferenceFramework> inference() const;
    std::shared_ptr<agent_dispatch::AgentDispatch> dispatch() const;
    std::shared_ptr<data_log::DataLogService> dataLog() const;
    std::shared_ptr<exception::ExceptionManager> exceptions() const;
    std::shared_ptr<atomic_service::DeterministicClimateProvider>
    climateProvider() const;
    std::shared_ptr<inference::MockModelRuntime> modelRuntime() const;

private:
    MasterAgentRuntime() = default;

    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<interaction::InteractionLayer> interaction_;
    std::shared_ptr<preprocess::PreprocessEngine> preprocess_;
    std::shared_ptr<memory::MemoryService> memory_;
    std::shared_ptr<kv_cache::KvCacheManager> kv_cache_;
    std::shared_ptr<inference::MockModelRuntime> model_runtime_;
    std::shared_ptr<inference::InferenceFramework> inference_;
    std::shared_ptr<atomic_service::DeterministicClimateProvider>
        climate_provider_;
    std::shared_ptr<atomic_service::AtomicServiceManager> atomic_;
    std::shared_ptr<sub_agents::DeterministicSubAgent> trip_agent_;
    std::shared_ptr<agent_dispatch::AgentDispatch> dispatch_;
    std::shared_ptr<intent_support::IIntentSkillResolver> skill_;
    std::shared_ptr<intent_support::IIntentPromptAssembler> prompt_;
    std::shared_ptr<intent::IntentEngine> intent_;
    std::shared_ptr<orchestrator::Orchestrator> orchestrator_;
    std::shared_ptr<data_log::DataLogService> log_;
    std::shared_ptr<exception::ExceptionManager> exceptions_;
    std::shared_ptr<agent_service::AgentService> agent_service_;

    mutable std::mutex session_lanes_mutex_;
    std::map<std::string, std::weak_ptr<std::mutex>> session_lanes_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool accepting_ingress_ = true;
    std::size_t active_turns_ = 0;
    bool shutdown_started_ = false;
    bool shutdown_complete_ = false;
    Status shutdown_status_ = Status::Ok();
};

}  // namespace master_agent::runtime
