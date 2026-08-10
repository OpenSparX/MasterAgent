#pragma once

/**
 * @file plan_response.h
 * @brief Private plan-state presentation helpers.
 *
 * This header is private to Agent Service and is not part of the installed API.
 */

#include "master_agent/agent_service/agent_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <random>
#include <set>
#include <utility>

namespace master_agent::agent_service {
namespace {

std::string replyForPlan(const orchestrator::TaskPlanSnapshot& plan) {
    if (plan.state == orchestrator::PlanState::Succeeded) {
        const bool has_dispatch = std::any_of(
            plan.nodes.begin(), plan.nodes.end(), [](const auto& pair) {
                return pair.second.definition.executor == "agent_dispatch";
            });
        return has_dispatch ? u8"行程规划已完成。"
                            : u8"设置已成功完成。";
    }
    if (plan.state == orchestrator::PlanState::Unknown) {
        return u8"操作结果暂时无法确认，系统正在对账，请勿重复操作。";
    }
    if (plan.state == orchestrator::PlanState::Committed ||
        plan.state == orchestrator::PlanState::Running) {
        const bool reconciling = std::any_of(
            plan.nodes.begin(), plan.nodes.end(),
            [](const auto& pair) {
                return pair.second.state ==
                       orchestrator::ActivationState::Reconciling;
            });
        return reconciling
                   ? u8"操作已受理，当前结果仍在对账，请勿重复操作。"
                   : u8"操作已受理并正在执行。";
    }
    return u8"本次操作没有完成，请稍后再试。";
}

bool planSuccess(orchestrator::PlanState state) {
    return state == orchestrator::PlanState::Succeeded;
}

bool planTerminal(orchestrator::PlanState state) {
    return state == orchestrator::PlanState::Succeeded ||
           state == orchestrator::PlanState::Failed ||
           state == orchestrator::PlanState::Cancelled ||
           state == orchestrator::PlanState::Unknown;
}

}  // namespace
}  // namespace master_agent::agent_service

