/**
 * @file preprocess_engine_impl.cpp
 * @brief Implements the preprocessing facade and orchestration component.
 *
 * Design mapping:
 * - Section 2.3: six-step base preprocessing flow.
 * - Section 2.4: state queries remain independent from process().
 * - Sections 3 and 4: public and internal interface boundaries.
 * - Section 5: metadata, input and time algorithms.
 */

#include "include/preprocess_engine_impl.h"

#include <utility>

namespace master_agent::preprocess {
namespace {

PreprocessDependencies clockOnlyDependencies(
    std::shared_ptr<IRuntimeClock> clock) {
    PreprocessDependencies dependencies;
    dependencies.clock = std::move(clock);
    return dependencies;
}

PreprocessDependencies providerDependencies(
    std::shared_ptr<IRuntimeClock> clock,
    std::vector<std::shared_ptr<IRuntimeStateProvider>>
        providers) {
    PreprocessDependencies dependencies;
    dependencies.clock = std::move(clock);
    dependencies.providers = std::move(providers);
    return dependencies;
}

}  // namespace

PreprocessEngineImpl::PreprocessEngineImpl(
    PreprocessDependencies dependencies)
    : clock_(std::move(dependencies.clock)) {
    if (!dependencies.ids) {
        dependencies.ids =
            std::make_shared<IdGenerator>("preprocess");
    }
    if (dependencies.providers.empty()) {
        dependencies.providers.push_back(
            std::make_shared<
                detail::HostTestStateProvider>(
                clock_,
                StateDomain::Vehicle,
                std::map<std::string, std::string>{
                    {"battery_soc", "85"},
                    {"engine_on", "true"},
                    {"is_parked", "true"},
                    {"speed_kmh", "0"}}));
        dependencies.providers.push_back(
            std::make_shared<
                detail::HostTestStateProvider>(
                clock_,
                StateDomain::Environment,
                std::map<std::string, std::string>{
                    {"outside_temperature", "25"},
                    {"weather", "clear"}}));
    }

    auto collector =
        std::make_shared<detail::VehicleStateCollector>(
            std::move(dependencies.providers), clock_);
    state_query_service_ =
        std::make_shared<detail::StateQueryServiceImpl>(
            clock_, std::move(collector));
    observer_ =
        std::make_shared<detail::PreprocessObserver>(
            clock_, std::move(dependencies.ids),
            std::move(dependencies.log_service),
            std::move(dependencies.exception_manager));
}

Result<PreprocessResult> PreprocessEngineImpl::process(
    const interaction::StandardRequest& request,
    const CallContext& call) const {
    const auto boundary =
        detail::validateCallBoundary(call, clock_);
    if (!boundary.ok) {
        return Result<PreprocessResult>::Failure(
            boundary);
    }
    if (call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        call.priority != request.priority ||
        call.deadline_mono_ns !=
            request.deadline_mono_ns) {
        return Result<PreprocessResult>::Failure(
            Status::Error(
                "preprocess",
                "PREPROCESS_CALL_IDENTITY_INVALID",
                "preprocessing call does not bind "
                "the active request"));
    }

    const auto finish =
        [this, &call, &request](
            PreprocessResult result,
            const std::string& error_code) {
            if (deadlineExpired(
                    call.deadline_mono_ns, *clock_)) {
                return Result<PreprocessResult>::Failure(
                    Status::Error(
                        "preprocess",
                        "PREPROCESS_RESULT_AFTER_DEADLINE",
                        "preprocessing completed after "
                        "the caller deadline"));
            }
            if (observer_) {
                observer_->record(
                    call, request.session_id, "process",
                    result.valid ? "SUCCESS" : "INVALID",
                    error_code);
            }
            return Result<PreprocessResult>::Success(
                std::move(result));
        };

    if (!detail::validRequestMetadata(request)) {
        return finish(
            detail::invalidPreprocessResult(
                "request metadata is invalid"),
            "PREPROCESS_REQUEST_METADATA_INVALID");
    }

    const auto cleaned =
        cleaner_.clean(request.text, request.params);
    if (!cleaned.status.ok || !cleaned.value) {
        const auto code = cleaned.status.ok
                              ? "PREPROCESS_CLEAN_RESULT_MISSING"
                              : cleaned.status.error.code;
        const auto message = cleaned.status.ok
                                 ? "preprocessing clean result is missing"
                                 : cleaned.status.error.message;
        return finish(
            detail::invalidPreprocessResult(message),
            code);
    }
    if (request.trigger_type == "TEXT_INPUT" &&
        cleaned.value->text.empty()) {
        return finish(
            detail::invalidPreprocessResult(
                "normalized user text is empty"),
            "PREPROCESS_USER_TEXT_EMPTY");
    }
    if (request.trigger_type != "TEXT_INPUT" &&
        cleaned.value->text.empty() &&
        cleaned.value->params.empty()) {
        return finish(
            detail::invalidPreprocessResult(
                "event input requires text or "
                "a valid parameter"),
            "PREPROCESS_EVENT_CONTENT_EMPTY");
    }

    const auto normalized =
        normalizer_.normalize(
            *cleaned.value, request.trigger_type);
    if (!normalized.status.ok || !normalized.value) {
        const auto code = normalized.status.ok
                              ? "PREPROCESS_NORMALIZE_RESULT_MISSING"
                              : normalized.status.error.code;
        const auto message = normalized.status.ok
                                 ? "preprocessing normalized result is missing"
                                 : normalized.status.error.message;
        return finish(
            detail::invalidPreprocessResult(message),
            code);
    }

    auto event_schema =
        normalized.value->event_schema;
    event_schema["request_id"] = request.request_id;
    event_schema["session_id"] = request.session_id;
    event_schema["turn_id"] =
        std::to_string(request.turn_id);
    for (const auto& [key, value] :
         normalized.value->params) {
        event_schema.emplace(key, value);
    }
    const auto aligned = time_aligner_.align(
        request.timestamp_utc_ms,
        clock_->utcNowMs(),
        std::move(event_schema));

    PreprocessResult result;
    result.normalized_request = request;
    result.normalized_request.text =
        normalized.value->text;
    result.normalized_request.params =
        normalized.value->params;
    result.normalized_request.timestamp_utc_ms =
        aligned.aligned_timestamp_utc_ms;
    result.event_schema = aligned.event_schema;
    result.valid = true;
    return finish(std::move(result), {});
}

Result<StateQueryResult>
PreprocessEngineImpl::queryRuntimeState(
    const StateQuery& query,
    const CallContext& call) const {
    if (!state_query_service_) {
        return Result<StateQueryResult>::Failure(
            Status::Error(
                "preprocess", "PREPROCESS_NOT_READY",
                "state query service is not configured"));
    }
    const auto result =
        state_query_service_->queryRuntimeState(
            query, call);
    if (observer_ &&
        hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        const bool success =
            result.status.ok && result.value &&
            result.value->success;
        const std::string error_code =
            !result.status.ok
                ? result.status.error.code
                : success
                      ? std::string{}
                      : "PREPROCESS_STATE_UNAVAILABLE";
        observer_->record(
            call, query.session_id,
            "queryRuntimeState",
            success ? "SUCCESS" : "FAILED",
            error_code);
    }
    return result;
}

Result<std::vector<StateCapability>>
PreprocessEngineImpl::getCapabilities(
    const CallContext& call) const {
    if (!state_query_service_) {
        return Result<
            std::vector<StateCapability>>::Failure(
            Status::Error(
                "preprocess", "PREPROCESS_NOT_READY",
                "state query service is not configured"));
    }
    const auto result =
        state_query_service_->getCapabilities(call);
    if (observer_ &&
        hasHostModuleIdentity(
            call, CallerModuleId::AgentService)) {
        observer_->record(
            call, {}, "getCapabilities",
            result.status.ok ? "SUCCESS" : "FAILED",
            result.status.ok
                ? std::string{}
                : result.status.error.code);
    }
    return result;
}

PreprocessEngine::PreprocessEngine(
    std::shared_ptr<IRuntimeClock> clock)
    : PreprocessEngine(
          clockOnlyDependencies(std::move(clock))) {}

PreprocessEngine::PreprocessEngine(
    std::shared_ptr<IRuntimeClock> clock,
    std::vector<std::shared_ptr<IRuntimeStateProvider>>
        providers)
    : PreprocessEngine(providerDependencies(
          std::move(clock), std::move(providers))) {}

PreprocessEngine::PreprocessEngine(
    PreprocessDependencies dependencies)
    : impl_(std::make_shared<PreprocessEngineImpl>(
          std::move(dependencies))) {}

Result<PreprocessResult> PreprocessEngine::process(
    const interaction::StandardRequest& request,
    const CallContext& call) const {
    return impl_->process(request, call);
}

Result<StateQueryResult>
PreprocessEngine::queryRuntimeState(
    const StateQuery& query,
    const CallContext& call) const {
    return impl_->queryRuntimeState(query, call);
}

Result<std::vector<StateCapability>>
PreprocessEngine::getCapabilities(
    const CallContext& call) const {
    return impl_->getCapabilities(call);
}

}  // namespace master_agent::preprocess
