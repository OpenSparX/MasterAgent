/**
 * @file deterministic_skill_adapter.cpp
 * @brief Adapts deterministic slot binding to the standalone Skill library.
 */

#include "master_agent/intent/intent_support.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace master_agent::intent_support {
namespace {

bool contains(const std::string& text, const std::string& token) {
    return text.find(token) != std::string::npos;
}

std::optional<std::string> circulationMode(const std::string& text) {
    if (contains(text, u8"内循环")) return "INTERNAL";
    if (contains(text, u8"外循环")) return "EXTERNAL";
    if (contains(text, u8"自动循环") ||
        (contains(text, u8"循环") && contains(text, u8"自动"))) {
        return "AUTO";
    }
    return std::nullopt;
}

std::optional<std::string> fanMode(const std::string& text) {
    if (contains(text, u8"低") || contains(text, "LOW")) return "LOW";
    if (contains(text, u8"正常") || contains(text, u8"中") ||
        contains(text, "NORMAL")) {
        return "NORMAL";
    }
    if (contains(text, u8"高") || contains(text, "HIGH")) return "HIGH";
    return std::nullopt;
}

std::string fanLocation(const std::string& text) {
    return contains(text, u8"后排") || contains(text, "REAR") ? "REAR"
                                                              : "FRONT";
}

class IntentSkillResolver final : public IIntentSkillResolver {
public:
    IntentSkillResolver(
        std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
        std::shared_ptr<skill::ISkillEngine> skill_library)
        : atomic_(std::move(atomic)),
          skill_library_(std::move(skill_library)) {}

    Result<std::vector<atomic_service::McpToolDefinition>>
    listAvailableTools(const CallContext& call) const override {
        if (!atomic_) {
            return Result<
                std::vector<atomic_service::McpToolDefinition>>::Failure(
                Status::Error("intent_support", "SKILL_NOT_READY",
                              "atomic service is not configured"));
        }
        if (!hasHostModuleIdentity(call, call.caller) ||
            (call.caller != CallerModuleId::IntentRecognitionEngine &&
             call.caller != CallerModuleId::AgentService)) {
            return Result<
                std::vector<atomic_service::McpToolDefinition>>::Failure(
                Status::Error(
                    "intent_support", "SKILL_CALLER_NOT_ALLOWED",
                    "skill lookup requires IntentEngine or AgentService"));
        }
        auto atomic_call =
            makeChildCallContext(call, CallerModuleId::SkillEngine);
        return atomic_->listTools(atomic_call);
    }

    DeterministicResolution resolveDeterministic(
        const std::string& normalized_text,
        const CallContext& call) const override {
        const auto tools = listAvailableTools(call);
        if (!tools.status.ok || !tools.value) {
            return {
                DeterministicResolutionState::Failed,
                {},
                nlohmann::json::object(),
                {},
                tools.status.error.code};
        }
        auto available = [&tools](const std::string& name) {
            return std::any_of(
                tools.value->begin(), tools.value->end(),
                [&name](const atomic_service::McpToolDefinition& tool) {
                    return tool.name == name;
                });
        };

        if (contains(normalized_text, u8"循环")) {
            const auto mode = circulationMode(normalized_text);
            if (!mode) {
                return {
                    DeterministicResolutionState::Clarify,
                    {},
                    nlohmann::json::object(),
                    u8"请问要设置为内循环、外循环还是自动循环？",
                    "CIRCULATION_MODE_MISSING"};
            }
            const std::string tool =
                "com_sgm_service_climate_setAirCirculationMode";
            if (!available(tool)) {
                return {
                    DeterministicResolutionState::Failed,
                    {},
                    nlohmann::json::object(),
                    {},
                    "TOOL_NOT_IN_FROZEN_CATALOG"};
            }
            return {
                DeterministicResolutionState::Resolved,
                tool,
                nlohmann::json{{"mode", *mode}},
                u8"好的，正在设置空气循环模式。",
                "RULE_RESOLVED"};
        }
        if (contains(normalized_text, u8"风速") ||
            contains(normalized_text, u8"风量")) {
            const auto mode = fanMode(normalized_text);
            if (!mode) {
                return {
                    DeterministicResolutionState::Clarify,
                    {},
                    nlohmann::json::object(),
                    u8"请问自动风速要设置为低、正常还是高？",
                    "FAN_MODE_MISSING"};
            }
            const std::string tool =
                "com_sgm_service_climate_setAutoFanSpeed";
            if (!available(tool)) {
                return {
                    DeterministicResolutionState::Failed,
                    {},
                    nlohmann::json::object(),
                    {},
                    "TOOL_NOT_IN_FROZEN_CATALOG"};
            }
            return {
                DeterministicResolutionState::Resolved,
                tool,
                nlohmann::json{
                    {"location", fanLocation(normalized_text)},
                    {"mode", *mode}},
                u8"好的，正在设置自动空调风速。",
                "RULE_RESOLVED"};
        }

        // The standalone Skill engine supplies model-facing candidates. It does
        // not convert a fuzzy library match into an executable side effect.
        if (skill_library_) {
            skill::SkillRouteRequest route;
            route.query = normalized_text;
            route.top_k = 1;
            (void)skill_library_->routeSkills(route);
        }
        return {
            DeterministicResolutionState::NoMatch,
            {},
            nlohmann::json::object(),
            {},
            "NO_DETERMINISTIC_RULE"};
    }

    Result<nlohmann::json> querySkillCandidates(
        const std::string& normalized_text,
        std::size_t top_k,
        const CallContext& call) const override {
        if (!hasHostModuleIdentity(call, call.caller) ||
            call.caller !=
                CallerModuleId::IntentRecognitionEngine ||
            normalized_text.empty() || top_k == 0 ||
            top_k > 16 || !skill_library_) {
            return Result<nlohmann::json>::Failure(
                Status::Error(
                    "intent_support",
                    "SKILL_QUERY_INVALID",
                    "Skill candidate query is invalid"));
        }
        skill::SkillRouteRequest route;
        route.query = normalized_text;
        route.top_k = top_k;
        const auto matches =
            skill_library_->routeSkills(route);
        nlohmann::json result;
        result["total_candidates"] =
            matches.total_candidates;
        result["matches"] =
            matches.matched_skill_json_list;
        return Result<nlohmann::json>::Success(
            std::move(result));
    }

private:
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic_;
    std::shared_ptr<skill::ISkillEngine> skill_library_;
};

}  // namespace

std::shared_ptr<IIntentSkillResolver> createIntentSkillResolver(
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<skill::ISkillEngine> skill_library) {
    return std::make_shared<IntentSkillResolver>(
        std::move(atomic), std::move(skill_library));
}

}  // namespace master_agent::intent_support
