/**
 * @file blocker_index.cpp
 * @brief Maintains dependency blockers and selects ready nodes.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {

namespace {

const nlohmann::json* readJsonPath(
    const nlohmann::json& root, const std::string& path) {
    const nlohmann::json* current = &root;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto dot = path.find('.', start);
        const auto token = path.substr(
            start, dot == std::string::npos
                       ? std::string::npos
                       : dot - start);
        if (token.empty() || !current->is_object()) {
            return nullptr;
        }
        const auto found = current->find(token);
        if (found == current->end()) return nullptr;
        current = &*found;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return current;
}

bool evaluatePredicate(
    const nlohmann::json& result,
    const nlohmann::json& predicate) {
    const auto field = predicate.value(
        "field_path",
        predicate.value("field", std::string{}));
    const auto operation = predicate.value(
        "operator",
        predicate.value("op", std::string{"EQ"}));
    const auto* actual = readJsonPath(result, field);
    if (operation == "EXISTS") return actual != nullptr;
    if (!actual || !predicate.contains("value")) return false;
    const auto& expected = predicate.at("value");
    if (operation == "EQ") return *actual == expected;
    if (operation == "NE") return *actual != expected;
    if (!actual->is_number() || !expected.is_number()) {
        return false;
    }
    const auto left = actual->get<double>();
    const auto right = expected.get<double>();
    if (operation == "GT") return left > right;
    if (operation == "GTE") return left >= right;
    if (operation == "LT") return left < right;
    if (operation == "LTE") return left <= right;
    return false;
}

// Returns 1 when the edge is satisfied, 0 while it is pending, and -1
// when the selected branch is no longer reachable.
int edgeOutcome(
    const DAGEdge& edge,
    const NodeRuntimeSnapshot& source) {
    if (!nodeTerminal(source.state)) return 0;
    if (edge.edge_type == "SUCCESS" ||
        edge.edge_type == "DATA") {
        return source.state == ActivationState::Succeeded ? 1 : -1;
    }
    if (edge.edge_type == "FAILURE") {
        return source.state == ActivationState::Failed ? 1 : -1;
    }
    if (source.state != ActivationState::Succeeded) return -1;
    const bool predicate =
        evaluatePredicate(source.result, edge.predicate);
    if (edge.edge_type == "RESULT_TRUE") {
        return predicate ? 1 : -1;
    }
    if (edge.edge_type == "RESULT_FALSE") {
        return predicate ? -1 : 1;
    }
    return -1;
}

std::set<std::string> executionResources(
    const NodeRuntimeSnapshot& node) {
    std::set<std::string> resources(
        node.definition.resource_requirements.begin(),
        node.definition.resource_requirements.end());
    // Tool-level serialization is the conservative fallback when a plan does
    // not carry a finer-grained trusted resource lease. It prevents a later
    // fencing token for the same side-effect domain from invalidating work
    // that the executor already accepted.
    if (resources.empty() &&
        node.definition.executor == "atomic_service") {
        resources.insert(
            "atomic-action:" + node.definition.action);
    }
    return resources;
}

bool executionActive(ActivationState state) {
    return state == ActivationState::DispatchPending ||
           state == ActivationState::Queued ||
           state == ActivationState::Running ||
           state == ActivationState::Reconciling;
}

}  // namespace

void Orchestrator::refreshBlockers(PlanRuntime& plan) {
    if (plan.snapshot.state != PlanState::Running &&
        plan.snapshot.state != PlanState::Committed) {
        return;
    }
    for (auto& pair : plan.snapshot.nodes) {
        auto& node = pair.second;
        if (node.state != ActivationState::Blocked) continue;
        const bool retry_due = node.retry_at_mono_ns > 0;
        if (retry_due &&
            clock_->monotonicNowNs() < node.retry_at_mono_ns) {
            continue;
        }
        node.blockers.clear();
        if (!node.trigger_satisfied) {
            if (node.definition.trigger.type ==
                    TriggerType::Timer &&
                node.next_fire_at_utc_ms > 0 &&
                clock_->utcNowMs() >=
                    node.next_fire_at_utc_ms) {
                node.trigger_satisfied = true;
                node.activation_id =
                    ids_->next("activation");
                ++node.activation_count;
                node.trigger_instance_id =
                    ids_->next("trigger-timer");
                ++plan.snapshot.version;
                emit(plan, &node, "TIMER_TRIGGERED");
            } else {
                node.blockers.push_back(
                    node.definition.trigger.type ==
                            TriggerType::Timer
                        ? "TRIGGER_TIMER"
                        : "TRIGGER_SIGNAL");
            }
        }

        std::size_t satisfied = 0;
        std::size_t pending = 0;
        std::size_t required = 0;
        std::set<std::string> edge_sources;
        for (const auto& edge : plan.edges) {
            if (!edge.required ||
                edge.to_node_id != pair.first) {
                continue;
            }
            ++required;
            edge_sources.insert(edge.from_node_id);
            const auto outcome = edgeOutcome(
                edge,
                plan.snapshot.nodes.at(edge.from_node_id));
            if (outcome > 0) {
                ++satisfied;
            } else if (outcome == 0) {
                ++pending;
                node.blockers.push_back(
                    "EDGE:" + edge.edge_id);
            }
        }
        for (const auto& dependency :
             node.definition.dependencies) {
            if (edge_sources.count(dependency) != 0) continue;
            ++required;
            const auto& source =
                plan.snapshot.nodes.at(dependency);
            if (source.state == ActivationState::Succeeded) {
                ++satisfied;
            } else if (!nodeTerminal(source.state)) {
                ++pending;
                node.blockers.push_back(
                    "DEPENDENCY:" + dependency);
            }
        }
        std::size_t threshold = required;
        if (node.definition.join_policy.kind == JoinKind::Any) {
            threshold = required == 0 ? 0 : 1;
        } else if (
            node.definition.join_policy.kind ==
            JoinKind::Quorum) {
            threshold =
                node.definition.join_policy.quorum;
        }
        const bool join_satisfied = satisfied >= threshold;
        const bool impossible =
            satisfied + pending < threshold;
        if (impossible) {
            node.state = ActivationState::Skipped;
            node.error_code = "DEPENDENCY_PATH_IMPOSSIBLE";
            node.blockers.clear();
            ++plan.snapshot.version;
            emit(plan, &node, "NODE_SKIPPED");
        } else if (join_satisfied &&
                   node.trigger_satisfied) {
            if (retry_due) {
                node.execution_id.clear();
                node.operation_id.clear();
                node.fencing_token = 0;
                node.result = nlohmann::json::object();
                node.error_code.clear();
                node.side_effect_state =
                    SideEffectState::NotStarted;
                node.retryable_hint = false;
                node.retry_at_mono_ns = 0;
            }
            node.state = ActivationState::Ready;
            node.blockers.clear();
            plan.node_ready_sequence[pair.first] = ++ready_sequence_;
            ++plan.snapshot.version;
            emit(plan, &node,
                 retry_due ? "NODE_RETRY_READY" : "NODE_READY");
        }
    }
}

std::optional<std::pair<std::string, std::string>>
Orchestrator::selectReady() const {
    const NodeRuntimeSnapshot* best = nullptr;
    std::string best_plan;
    std::string best_node;
    std::uint64_t best_sequence = 0;
    for (const auto& plan_pair : plans_) {
        if (plan_pair.second.snapshot.state != PlanState::Running &&
            plan_pair.second.snapshot.state !=
                PlanState::Committed) {
            continue;
        }
        for (const auto& node_pair : plan_pair.second.snapshot.nodes) {
            const auto& node = node_pair.second;
            if (node.state != ActivationState::Ready) continue;
            const auto candidate_resources = executionResources(node);
            bool resource_busy = false;
            for (const auto& owner_plan : plans_) {
                for (const auto& owner_pair :
                     owner_plan.second.snapshot.nodes) {
                    const auto& owner = owner_pair.second;
                    if (!executionActive(owner.state)) continue;
                    const auto owner_resources =
                        executionResources(owner);
                    if (std::any_of(
                            candidate_resources.begin(),
                            candidate_resources.end(),
                            [&owner_resources](const auto& resource) {
                                return owner_resources.count(resource) != 0;
                            })) {
                        resource_busy = true;
                        break;
                    }
                }
                if (resource_busy) break;
            }
            if (resource_busy) continue;
            const auto sequence =
                plan_pair.second.node_ready_sequence.at(node_pair.first);
            if (!best ||
                std::tie(node.effective_priority,
                         node.definition.deadline_mono_ns, sequence) <
                    std::tie(best->effective_priority,
                             best->definition.deadline_mono_ns,
                             best_sequence)) {
                best = &node;
                best_plan = plan_pair.first;
                best_node = node_pair.first;
                best_sequence = sequence;
            }
        }
    }
    return best ? std::optional<std::pair<std::string, std::string>>(
                      {best_plan, best_node})
                : std::nullopt;
}


}  // namespace master_agent::orchestrator
