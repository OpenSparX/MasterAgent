/**
 * @file skill_path.cpp
 * @brief Converts deterministic skill results into orchestration output.
 */

#include "include/intent_text_rules.h"
#include "include/intent_deadline.h"

namespace master_agent::intent {

IntentOrchestrationResult IntentEngine::fromSkill(
    const interaction::StandardRequest& request,
    const IntentContext& context,
    const intent_support::DeterministicResolution& resolution,
    const CallContext&) const {

    IntentOrchestrationResult result;
    result.request_id = request.request_id;
    result.user_reply = resolution.user_reply;
    result.reason_code = resolution.reason_code;
    result.trace_id = request.trace_id;
    result.completed_at_utc_ms = clock_->utcNowMs();
    if (resolution.state ==
        intent_support::DeterministicResolutionState::Clarify) {
        result.outcome_type = IntentOutcomeType::Clarify;
        return result;
    }
    if (resolution.state ==
        intent_support::DeterministicResolutionState::Failed) {
        result.outcome_type = IntentOutcomeType::Failed;
        return result;
    }
    orchestrator::IntentDAG dag;
    dag.dag_id = ids_->next("intent-dag");
    dag.request_id = request.request_id;
    dag.priority = context.priority;
    dag.deadline_mono_ns = context.deadline_mono_ns;
    dag.idempotency_key =
        "intent-plan|" + request.request_id + "|" +
        std::to_string(request.turn_id);
    orchestrator::DAGNode node;
    node.node_id = "T1";
    node.executor = "atomic_service";
    node.action = resolution.tool_name;
    node.params = resolution.arguments;
    node.base_priority = context.priority;
    node.deadline_mono_ns = nodeDeadline(context);
    node.max_attempts = 2;
    dag.nodes.push_back(std::move(node));
    result.outcome_type = IntentOutcomeType::DeterministicPlan;
    result.task_dag = std::move(dag);
    return result;
}


}  // namespace master_agent::intent
