/**
 * @file master_agent_runtime.cpp
 * @brief Implements single-process module composition and lifecycle.
 */

#include "master_agent/runtime/master_agent_runtime.h"

#include "master_agent/agent_service/intent_query_gateway.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <utility>

namespace master_agent::runtime {
namespace {

bool safeMetadataToken(const std::string& value,
                       std::size_t max_size = 128) {
    return !value.empty() && value.size() <= max_size &&
           std::all_of(
               value.begin(), value.end(),
               [](unsigned char character) {
                   return std::isalnum(character) != 0 ||
                          character == '_' || character == '-' ||
                          character == '.';
               });
}

std::string normalizedErrorCode(const std::string& code) {
    return safeMetadataToken(code)
               ? code
               : "UNTRUSTED_MODULE_FAILURE";
}

std::string safeClientErrorMessage() {
    return "request could not be completed";
}

class ScopeExit final {
public:
    explicit ScopeExit(std::function<void()> action)
        : action_(std::move(action)) {}

    ~ScopeExit() {
        if (action_) action_();
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    std::function<void()> action_;
};

}  // namespace

Result<std::shared_ptr<MasterAgentRuntime>>
MasterAgentRuntime::create(
    const std::filesystem::path& runtime_directory,
    std::shared_ptr<IRuntimeClock> clock,
    std::uint32_t simulated_work_units) {
    auto runtime = std::shared_ptr<MasterAgentRuntime>(
        new MasterAgentRuntime());
    runtime->clock_ = clock ? std::move(clock)
                            : std::make_shared<SystemRuntimeClock>();
    const auto boot =
        "boot-" + std::to_string(runtime->clock_->utcNowMs()) + "-" +
        std::to_string(runtime->clock_->monotonicNowNs());
    runtime->ids_ = std::make_shared<IdGenerator>(boot);

    // Initialization follows dependency order. Observability is brought up
    // before business modules so later failures can be recorded.
    runtime->log_ = std::make_shared<data_log::DataLogService>(
        runtime_directory / "data_log", runtime->clock_, runtime->ids_);
    const auto log_ready = runtime->log_->initialize();
    if (!log_ready.ok) {
        return Result<std::shared_ptr<MasterAgentRuntime>>::Failure(
            log_ready);
    }
    runtime->exceptions_ =
        std::make_shared<exception::ExceptionManager>(
            runtime_directory / "exceptions", runtime->clock_,
            runtime->ids_, runtime->log_,
            exception::ExceptionManager::DurabilitySync{}, false);
    const auto exception_ready = runtime->exceptions_->initialize();
    if (!exception_ready.ok) {
        return Result<std::shared_ptr<MasterAgentRuntime>>::Failure(
            exception_ready);
    }

    const auto memory_client =
        memory::createJournalMemoryClient(runtime_directory / "memory");
    interaction::TurnFloorLookup turn_floor_lookup =
        [memory_client](const std::string& user_id,
                        const std::string& session_id) {
            master_agent::memory::GetContextRequest request;
            request.user_id = user_id;
            request.session_id = session_id;
            request.query = "turn-sequence-recovery";
            request.scene = "cockpit";
            request.token_budget = std::numeric_limits<int>::max();
            request.max_turns = std::numeric_limits<int>::max();
            const auto recalled = memory_client->getContext(request);
            if (!recalled.success) {
                return Result<std::uint64_t>::Failure(Status::Error(
                    "interaction",
                    recalled.error_code.empty()
                        ? "INTERACTION_TURN_MIGRATION_FAILED"
                        : recalled.error_code,
                    recalled.error_message, true));
            }
            std::uint64_t highest = 0;
            for (const auto& block : recalled.context_blocks) {
                highest = std::max(highest, block.turn_id);
            }
            return Result<std::uint64_t>::Success(highest);
        };

    runtime->interaction_ = std::make_shared<interaction::InteractionLayer>(
        runtime->clock_, runtime->ids_,
        runtime_directory / "interaction" / "turn_sequences",
        std::move(turn_floor_lookup));
    preprocess::PreprocessDependencies preprocess_dependencies;
    preprocess_dependencies.clock = runtime->clock_;
    preprocess_dependencies.ids = runtime->ids_;
    preprocess_dependencies.log_service = runtime->log_;
    preprocess_dependencies.exception_manager =
        runtime->exceptions_;
    runtime->preprocess_ =
        std::make_shared<preprocess::PreprocessEngine>(
            std::move(preprocess_dependencies));
    runtime->memory_ = std::make_shared<memory::MemoryService>(
        memory_client, runtime->clock_);
    runtime->kv_cache_ = std::make_shared<kv_cache::KvCacheManager>(
        runtime->clock_, runtime->ids_);
    runtime->model_runtime_ =
        std::make_shared<inference::MockModelRuntime>(
            simulated_work_units);
    runtime->dispatch_ =
        std::make_shared<agent_dispatch::AgentDispatch>(
            runtime_directory / "agent_dispatch",
            runtime->clock_, runtime->ids_);
    const auto dispatch_ready = runtime->dispatch_->initialize();
    if (!dispatch_ready.ok) {
        return Result<std::shared_ptr<MasterAgentRuntime>>::Failure(
            dispatch_ready);
    }
    runtime->inference_ =
        std::make_shared<inference::InferenceFramework>(
            runtime->clock_, runtime->ids_, runtime->model_runtime_,
            runtime->kv_cache_, 2, runtime->dispatch_);
    runtime->climate_provider_ =
        std::make_shared<atomic_service::DeterministicClimateProvider>();
    runtime->atomic_ =
        std::make_shared<atomic_service::AtomicServiceManager>(
            runtime->clock_, runtime->ids_, 1, runtime->dispatch_,
            runtime_directory / "atomic_service");

    CallContext bootstrap{CallerModuleId::AgentService,
                          "bootstrap", "bootstrap-trace", "system",
                          TaskPriority::P1,
                          runtime->clock_->monotonicNowNs() +
                              30LL * 1000LL * 1000LL * 1000LL};
    const auto tool_status = runtime->atomic_->registerTools(
        atomic_service::defaultClimateMcpTools(),
        atomic_service::defaultClimateRuntimePolicies(
            simulated_work_units),
        runtime->climate_provider_, bootstrap);
    if (!tool_status.ok) {
        return Result<std::shared_ptr<MasterAgentRuntime>>::Failure(
            tool_status);
    }

    // The concrete business sub-Agent is a deterministic boundary provider.
    // Its internal implementation is intentionally outside Master Agent scope.
    sub_agents::AgentManifest trip_manifest;
    trip_manifest.agent_id = "trip-agent";
    trip_manifest.capabilities = {"com_sgm_agent_trip_plan"};
    trip_manifest.max_concurrency = 1;
    runtime->trip_agent_ =
        std::make_shared<sub_agents::DeterministicSubAgent>(
            trip_manifest, runtime->clock_, runtime->ids_,
            simulated_work_units);
    const auto agent_status =
        runtime->dispatch_->registerAgent(runtime->trip_agent_, bootstrap);
    if (!agent_status.ok) {
        return Result<std::shared_ptr<MasterAgentRuntime>>::Failure(
            agent_status);
    }

    auto skill_owner = skill::createSkillEngine();
    auto prompt_owner = prompt::createPromptEngine();
    if (!skill_owner || !prompt_owner) {
        return Result<std::shared_ptr<MasterAgentRuntime>>::Failure(
            Status::Error(
                "runtime", "KNOWLEDGE_ENGINE_NOT_READY",
                "Prompt or Skill engine creation failed"));
    }
    std::shared_ptr<skill::ISkillEngine> skill_library(
        std::move(skill_owner));
    std::shared_ptr<prompt::IPromptEngine> prompt_engine(
        std::move(prompt_owner));
    runtime->skill_ = intent_support::createIntentSkillResolver(
        runtime->atomic_, skill_library);
    runtime->prompt_ = intent_support::createIntentPromptAssembler(
        runtime->atomic_, prompt_engine, skill_library);
    auto intent_queries =
        intent::createModuleIntentQueryExecutor(
            agent_service::createIntentStateQueryGateway(
                runtime->preprocess_),
            runtime->memory_,
            runtime->skill_, runtime->atomic_,
            runtime->clock_);
    runtime->intent_ = std::make_shared<intent::IntentEngine>(
        runtime->clock_, runtime->ids_, runtime->skill_, runtime->prompt_,
        runtime->inference_,
        intent::createDeterministicMockBgeClassifier(
            runtime->clock_),
        std::move(intent_queries));
    runtime->orchestrator_ =
        std::make_shared<orchestrator::Orchestrator>(
            runtime->clock_, runtime->ids_, runtime->atomic_,
            runtime->dispatch_,
            runtime_directory / "orchestrator");
    runtime->agent_service_ =
        std::make_shared<agent_service::AgentService>(
            runtime->clock_, runtime->ids_, runtime->preprocess_,
            runtime->memory_, runtime->intent_, runtime->orchestrator_,
            runtime->atomic_, runtime->log_, runtime->exceptions_);
    return Result<std::shared_ptr<MasterAgentRuntime>>::Success(
        std::move(runtime));
}

MasterAgentRuntime::~MasterAgentRuntime() {
    (void)shutdown();
}

agent_service::TurnResult MasterAgentRuntime::submitText(
    const interaction::TextInput& input) {
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!accepting_ingress_) {
            agent_service::TurnResult result;
            result.success = false;
            result.reply = u8"系统正在关闭，暂不接受新请求。";
            result.error_code = "RUNTIME_SHUTTING_DOWN";
            result.error_message = safeClientErrorMessage();
            return result;
        }
        ++active_turns_;
    }
    const auto finish_turn = [this]() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (active_turns_ > 0) --active_turns_;
        lifecycle_cv_.notify_all();
    };
    ScopeExit active_turn_guard(finish_turn);

    // Acquire the session lane before durable TurnID allocation. Otherwise a
    // later turn could reach memory or side effects before the previous turn.
    const auto session_key = secureDigest(
        std::to_string(input.user_id.size()) + ":" + input.user_id +
        std::to_string(input.session_id.size()) + ":" +
        input.session_id);
    std::shared_ptr<std::mutex> session_lane;
    {
        std::lock_guard<std::mutex> registry_lock(
            session_lanes_mutex_);
        for (auto lane = session_lanes_.begin();
             lane != session_lanes_.end();) {
            if (lane->second.expired()) {
                lane = session_lanes_.erase(lane);
            } else {
                ++lane;
            }
        }
        auto& weak_lane = session_lanes_[session_key];
        session_lane = weak_lane.lock();
        if (!session_lane) {
            session_lane = std::make_shared<std::mutex>();
            weak_lane = session_lane;
        }
    }

    std::unique_lock<std::mutex> session_guard(*session_lane);
    const auto request = interaction_->submitText(input);
    if (!request.status.ok || !request.value) {
        agent_service::TurnResult result;
        result.success = false;
        result.reply = u8"输入无效，请重新输入。";
        result.error_code =
            normalizedErrorCode(request.status.error.code);
        result.error_message = safeClientErrorMessage();
        return result;
    }
    CallContext call{CallerModuleId::InteractionIngress,
                     request.value->request_id, request.value->trace_id,
                     secureDigest(input.user_id), request.value->priority,
                     request.value->deadline_mono_ns};
    return agent_service_->runTurn(*request.value, call);
}

Status MasterAgentRuntime::shutdown() {
    bool performs_shutdown = false;
    {
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
        accepting_ingress_ = false;
        lifecycle_cv_.wait(
            lifecycle_lock,
            [this] { return active_turns_ == 0; });
        if (shutdown_started_) {
            lifecycle_cv_.wait(
                lifecycle_lock,
                [this] { return shutdown_complete_; });
            return shutdown_status_;
        }
        shutdown_started_ = true;
        performs_shutdown = true;
    }
    Status status = Status::Ok();
    if (performs_shutdown && log_ && clock_) {
        CallContext flush_call{
            CallerModuleId::DataLogManager,
            "runtime-shutdown",
            "runtime-shutdown-trace",
            "system",
            TaskPriority::P1,
            clock_->monotonicNowNs() + 5'000'000'000LL};
        status = log_->flush(flush_call);
    }
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        shutdown_status_ = status;
        shutdown_complete_ = true;
        lifecycle_cv_.notify_all();
    }
    return status;
}

std::shared_ptr<interaction::InteractionLayer>
MasterAgentRuntime::interaction() const {
    return interaction_;
}

std::shared_ptr<preprocess::PreprocessEngine>
MasterAgentRuntime::preprocess() const {
    return preprocess_;
}

std::shared_ptr<memory::MemoryService>
MasterAgentRuntime::memory() const {
    return memory_;
}

std::shared_ptr<intent::IntentEngine>
MasterAgentRuntime::intent() const {
    return intent_;
}

std::shared_ptr<agent_service::AgentService>
MasterAgentRuntime::agentService() const {
    return agent_service_;
}

std::shared_ptr<orchestrator::Orchestrator>
MasterAgentRuntime::orchestrator() const {
    return orchestrator_;
}

std::shared_ptr<atomic_service::AtomicServiceManager>
MasterAgentRuntime::atomic() const {
    return atomic_;
}

std::shared_ptr<inference::InferenceFramework>
MasterAgentRuntime::inference() const {
    return inference_;
}

std::shared_ptr<agent_dispatch::AgentDispatch>
MasterAgentRuntime::dispatch() const {
    return dispatch_;
}

std::shared_ptr<data_log::DataLogService>
MasterAgentRuntime::dataLog() const {
    return log_;
}

std::shared_ptr<exception::ExceptionManager>
MasterAgentRuntime::exceptions() const {
    return exceptions_;
}

std::shared_ptr<atomic_service::DeterministicClimateProvider>
MasterAgentRuntime::climateProvider() const {
    return climate_provider_;
}

std::shared_ptr<inference::MockModelRuntime>
MasterAgentRuntime::modelRuntime() const {
    return model_runtime_;
}

}  // namespace master_agent::runtime
