/**
 * @file intent_query_gateway.cpp
 * @brief Implements the Agent Service mediation boundary for state evidence.
 */

#include "master_agent/agent_service/intent_query_gateway.h"

#include <utility>

namespace master_agent::agent_service {
namespace {

class IntentStateQueryGateway final : public preprocess::IStateQuery {
public:
    explicit IntentStateQueryGateway(
        std::shared_ptr<preprocess::IStateQuery> target)
        : target_(std::move(target)) {}

    Result<preprocess::StateQueryResult> queryRuntimeState(
        const preprocess::StateQuery& query,
        const CallContext& call) const override {
        if (!target_ ||
            !hasHostModuleIdentity(
                call, CallerModuleId::IntentRecognitionEngine)) {
            return Result<preprocess::StateQueryResult>::Failure(
                Status::Error(
                    "agent_service",
                    "AGENT_SERVICE_STATE_QUERY_CALLER_NOT_ALLOWED",
                    "state evidence requires an authenticated Intent Engine"));
        }
        return target_->queryRuntimeState(
            query,
            makeChildCallContext(call, CallerModuleId::AgentService));
    }

    Result<std::vector<preprocess::StateCapability>> getCapabilities(
        const CallContext& call) const override {
        if (!target_ ||
            !hasHostModuleIdentity(
                call, CallerModuleId::IntentRecognitionEngine)) {
            return Result<std::vector<preprocess::StateCapability>>::Failure(
                Status::Error(
                    "agent_service",
                    "AGENT_SERVICE_STATE_QUERY_CALLER_NOT_ALLOWED",
                    "state capability discovery requires an authenticated "
                    "Intent Engine"));
        }
        return target_->getCapabilities(
            makeChildCallContext(call, CallerModuleId::AgentService));
    }

private:
    std::shared_ptr<preprocess::IStateQuery> target_;
};

}  // namespace

std::shared_ptr<preprocess::IStateQuery>
createIntentStateQueryGateway(
    std::shared_ptr<preprocess::IStateQuery> preprocess) {
    return std::make_shared<IntentStateQueryGateway>(
        std::move(preprocess));
}

}  // namespace master_agent::agent_service
