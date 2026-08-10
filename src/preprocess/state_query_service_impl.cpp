/**
 * @file state_query_service_impl.cpp
 * @brief Implements capability discovery and on-demand state queries.
 *
 * Design mapping: Sections 2.4, 3.1, 3.3, 4 and 5.
 */

#include "include/state_query_service.h"

#include <set>
#include <utility>

namespace master_agent::preprocess::detail {
namespace {

bool validStateDomain(StateDomain domain) {
    return domain == StateDomain::Vehicle ||
           domain == StateDomain::Environment;
}

}  // namespace

StateQueryServiceImpl::StateQueryServiceImpl(
    std::shared_ptr<IRuntimeClock> clock,
    std::shared_ptr<VehicleStateCollector> collector)
    : clock_(std::move(clock)),
      collector_(std::move(collector)) {}

Result<std::vector<StateCapability>>
StateQueryServiceImpl::getCapabilities(
    const CallContext& call) const {
    const auto boundary =
        validateStateQueryCallBoundary(call, clock_);
    if (!boundary.ok) {
        return Result<
            std::vector<StateCapability>>::Failure(
            boundary);
    }
    if (!collector_) {
        return Result<
            std::vector<StateCapability>>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_NOT_CONFIGURED",
                "state collector is not configured"));
    }

    const auto capabilities =
        collector_->getCapabilities();
    if (!capabilities.status.ok) {
        return capabilities;
    }
    if (deadlineExpired(
            call.deadline_mono_ns, *clock_)) {
        return Result<
            std::vector<StateCapability>>::Failure(
            Status::Error(
                "preprocess",
                "PREPROCESS_CAPABILITY_RESULT_AFTER_DEADLINE",
                "capability lookup crossed the caller deadline"));
    }
    return capabilities;
}

Result<StateQueryResult>
StateQueryServiceImpl::queryRuntimeState(
    const StateQuery& query,
    const CallContext& call) const {
    const auto boundary =
        validateStateQueryCallBoundary(call, clock_);
    if (!boundary.ok) {
        return Result<StateQueryResult>::Failure(
            boundary);
    }
    if (!collector_) {
        return Result<StateQueryResult>::Failure(
            providerFailure(
                "PREPROCESS_STATE_PROVIDER_NOT_CONFIGURED",
                "state collector is not configured"));
    }
    if (query.request_id != call.request_id ||
        !validOpaqueId(query.session_id) ||
        query.session_id.find(':') !=
            std::string::npos ||
        query.turn_id == 0 ||
        !validStateDomain(query.state_type) ||
        query.fields.empty() ||
        query.fields.size() >
            kMaximumQueryFieldCount) {
        return Result<StateQueryResult>::Failure(
            Status::Error(
                "preprocess",
                "PREPROCESS_STATE_QUERY_INVALID",
                "state query identity or field selection is invalid"));
    }

    std::set<std::string> requested_fields;
    for (const auto& field : query.fields) {
        if (!validStateFieldName(field) ||
            !requested_fields.insert(field).second) {
            return Result<StateQueryResult>::Failure(
                Status::Error(
                    "preprocess",
                    "PREPROCESS_STATE_QUERY_INVALID",
                    "state query fields are malformed or duplicated"));
        }
    }

    const auto state = collector_->query(query);
    if (!state.status.ok) {
        return state;
    }
    if (deadlineExpired(
            call.deadline_mono_ns, *clock_)) {
        return Result<StateQueryResult>::Failure(
            Status::Error(
                "preprocess",
                "PREPROCESS_STATE_RESULT_AFTER_DEADLINE",
                "state query crossed the caller deadline"));
    }
    return state;
}

}  // namespace master_agent::preprocess::detail
