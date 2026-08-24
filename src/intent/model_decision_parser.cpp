/**
 * @file model_decision_parser.cpp
 * @brief Validates model decisions and converts them into safe task graphs.
 */

#include "include/intent_text_rules.h"
#include "include/intent_deadline.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

namespace master_agent::intent {
namespace {

bool containsAny(
    const std::string& text,
    const std::initializer_list<const char*>& candidates) {
    return std::any_of(
        candidates.begin(), candidates.end(),
        [&text](const char* candidate) {
            return text.find(candidate) != std::string::npos;
        });
}

bool hasUnverifiedEffectClaim(const std::string& reply) {
    std::string ascii_lower = reply;
    std::transform(
        ascii_lower.begin(), ascii_lower.end(), ascii_lower.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    const bool completion_claim = containsAny(
        reply,
        {u8"已经", u8"已为你", u8"已帮你", u8"已完成", u8"完成了",
         u8"调整好了", u8"设置好了", u8"打开了", u8"关闭了",
         u8"已调整", u8"已设置", u8"已打开", u8"已关闭",
         u8"已切换", u8"已启动", u8"已停止", u8"已发送",
         u8"已创建", u8"已删除", u8"已预订", u8"已购买",
         u8"已支付"}) ||
        containsAny(
            ascii_lower,
            {"has been set", "have been set", "has been adjusted",
             "have been adjusted", "turned on", "turned off",
             "i have set", "i've set", "completed successfully",
             "booking is confirmed", "payment completed"});
    if (!completion_claim) return false;

    return containsAny(
               reply,
               {u8"设置", u8"调整", u8"打开", u8"关闭", u8"切换",
                u8"启动", u8"停止", u8"发送", u8"创建", u8"删除",
                u8"预订", u8"购买", u8"支付", u8"温度", u8"风量",
                u8"空调", u8"循环", u8"座椅", u8"车窗", u8"导航",
                u8"音乐"}) ||
           containsAny(
               ascii_lower,
               {"set", "adjust", "turn", "send", "create", "delete",
                "book", "purchase", "payment", "temperature", "fan",
                "climate", "seat", "window", "navigation", "music"});
}

}  // namespace

Result<IntentOrchestrationResult>
IntentEngine::parseModelDecision(
    const interaction::StandardRequest& request,
    const IntentContext& context,
    const inference::InferenceOutput& output,
    const CallContext& call) const {

    if (!clock_ ||
        call.request_id != request.request_id ||
        call.trace_id != request.trace_id ||
        deadlineExpired(context.deadline_mono_ns, *clock_)) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", "INTENT_RESULT_AFTER_DEADLINE",
            "intent model output is no longer commit eligible"));
    }
    IntentOrchestrationResult result;
    result.request_id = request.request_id;
    result.trace_id = request.trace_id;
    result.completed_at_utc_ms = clock_->utcNowMs();
    try {
        const auto decoded = nlohmann::json::parse(output.raw_output);
        if (!decoded.is_object() ||
            !decoded.contains("outcome") ||
            !decoded.at("outcome").is_string()) {
            throw std::runtime_error("final outcome is missing");
        }
        const auto outcome = decoded.at("outcome").get<std::string>();
        if (outcome == "REPLY") {
            if (decoded.size() != 2 ||
                !decoded.contains("reply") ||
                !decoded.at("reply").is_string()) {
                throw std::runtime_error("invalid REPLY outcome");
            }
            const auto reply =
                decoded.at("reply").get<std::string>();
            if (reply.empty() || reply.size() > 2048) {
                throw std::runtime_error("REPLY is outside bounds");
            }
            if (hasUnverifiedEffectClaim(reply)) {
                return Result<IntentOrchestrationResult>::Failure(
                    Status::Error(
                        "intent", "INTENT_MODEL_FALSE_EXECUTION_CLAIM",
                        "REPLY claimed an effect without execution evidence"));
            }
            result.outcome_type = IntentOutcomeType::DirectReply;
            result.user_reply = reply;
            result.reason_code = "MODEL_DIRECT_REPLY";
            return Result<IntentOrchestrationResult>::Success(
                std::move(result));
        }
        if (outcome == "ASK") {
            static const std::set<std::string> allowed_slots{
                "location", "time",       "person", "seat",
                "target",   "value",      "content", "reference",
                "preference", "constraint"};
            if (decoded.size() != 3 ||
                !decoded.contains("slot") ||
                !decoded.at("slot").is_string() ||
                !decoded.contains("reply") ||
                !decoded.at("reply").is_string()) {
                throw std::runtime_error("invalid ASK outcome");
            }
            const auto slot =
                decoded.at("slot").get<std::string>();
            const auto reply =
                decoded.at("reply").get<std::string>();
            if (allowed_slots.count(slot) == 0 ||
                reply.empty() || reply.size() > 2048) {
                throw std::runtime_error("ASK is outside bounds");
            }
            result.outcome_type = IntentOutcomeType::Clarify;
            result.user_reply = reply;
            result.reason_code = "MODEL_CLARIFY_" + slot;
            return Result<IntentOrchestrationResult>::Success(
                std::move(result));
        }
        if (outcome == "FAIL") {
            static const std::set<std::string> allowed_reasons{
                "unsupported", "contradiction",
                "no_valid_binding"};
            if (decoded.size() != 3 ||
                !decoded.contains("reason_code") ||
                !decoded.at("reason_code").is_string() ||
                !decoded.contains("reply") ||
                !decoded.at("reply").is_string()) {
                throw std::runtime_error("invalid FAIL outcome");
            }
            const auto reason =
                decoded.at("reason_code").get<std::string>();
            const auto reply =
                decoded.at("reply").get<std::string>();
            if (allowed_reasons.count(reason) == 0 ||
                reply.empty() || reply.size() > 2048) {
                throw std::runtime_error("FAIL is outside bounds");
            }
            result.outcome_type = IntentOutcomeType::Failed;
            result.user_reply = reply;
            if (reason == "unsupported") {
                result.reason_code =
                    "INTENT_MODEL_UNSUPPORTED";
            } else if (reason == "contradiction") {
                result.reason_code =
                    "INTENT_MODEL_CONTRADICTION";
            } else {
                result.reason_code =
                    "INTENT_MODEL_NO_VALID_BINDING";
            }
            return Result<IntentOrchestrationResult>::Success(
                std::move(result));
        }
        if (outcome != "PLAN" || !decoded.at("nodes").is_array() ||
            decoded.at("nodes").empty() ||
            decoded.at("nodes").size() > 8) {
            throw std::runtime_error("invalid model outcome");
        }
        orchestrator::IntentDAG dag;
        dag.dag_id = ids_->next("intent-dag");
        dag.request_id = request.request_id;
        dag.priority = context.priority;
        dag.deadline_mono_ns = context.deadline_mono_ns;
        dag.idempotency_key =
            "intent-plan|" + request.request_id + "|" +
            std::to_string(request.turn_id);
        std::set<std::string> node_ids;
        for (const auto& encoded : decoded.at("nodes")) {
            orchestrator::DAGNode node;
            node.node_id = encoded.at("node_id").get<std::string>();
            node.executor = encoded.at("executor").get<std::string>();
            node.action = encoded.at("action").get<std::string>();
            node.target_agent =
                encoded.value("target_agent", std::string{});
            node.params =
                encoded.value("params", nlohmann::json::object());
            node.dependencies =
                encoded.value("dependencies",
                              std::vector<std::string>{});
            node.base_priority = context.priority;
            node.deadline_mono_ns = context.deadline_mono_ns;
            node.max_attempts = 1;
            if (node.node_id.empty() ||
                !node_ids.insert(node.node_id).second ||
                node.executor != "agent_dispatch" ||
                node.action.empty() || !node.params.is_object()) {
                throw std::runtime_error("unsafe model node");
            }
            dag.nodes.push_back(std::move(node));
        }
        for (const auto& node : dag.nodes) {
            std::set<std::string> unique_dependencies;
            for (const auto& dependency : node.dependencies) {
                if (dependency == node.node_id ||
                    node_ids.count(dependency) == 0 ||
                    !unique_dependencies.insert(dependency).second) {
                    throw std::runtime_error(
                        "unsafe model dependency");
                }
            }
        }
        result.outcome_type = IntentOutcomeType::DeterministicPlan;
        result.task_dag = std::move(dag);
        result.user_reply =
            decoded.value("reply", u8"正在处理你的请求。");
        result.reason_code = "MODEL_VALIDATED_PLAN";
        return Result<IntentOrchestrationResult>::Success(std::move(result));
    } catch (...) {
        return Result<IntentOrchestrationResult>::Failure(Status::Error(
            "intent", "INTENT_MODEL_PROTOCOL_INVALID",
            "model output failed strict protocol validation"));
    }
}

}  // namespace master_agent::intent
