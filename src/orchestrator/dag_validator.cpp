/**
 * @file dag_validator.cpp
 * @brief Validates task graphs, retry policies, and acyclic dependencies.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {

static bool safeRetryIdentifier(
    const std::string& value, std::size_t max_size = 256) {
    if (value.empty() || value.size() > max_size) return false;
    return std::all_of(
        value.begin(), value.end(), [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') ||
                   ch == '_' || ch == '-' || ch == '.';
        });
}

static bool retryingPolicy(const std::string& value) {
    return value == "READ_ONLY" || value == "TARGET_STATE" ||
           value == "TRANSACTION_KEY";
}

std::string retryPoliciesDigest(
    const std::map<std::string, CapabilityRetryPolicy>& policies) {
    nlohmann::json encoded = nlohmann::json::array();
    for (const auto& item : policies) {
        encoded.push_back(
            {{"capability", item.first},
             {"idempotency_policy",
              item.second.idempotency_policy},
             {"retryable_errors",
              item.second.retryable_errors},
             {"base_backoff_ns",
              item.second.base_backoff_ns},
             {"max_backoff_ns",
              item.second.max_backoff_ns}});
    }
    return secureDigest(encoded.dump());
}

ValidationResult Orchestrator::validateDAG(
    const IntentDAG& dag, const AdmissionContext& admission,
    const CallContext& call) const {

    ValidationResult result;
    const auto caller = validateAgentServiceCaller(call);
    if (!caller.ok) {
        result.reject_code = caller.error.code;
        return result;
    }
    const auto normalized = normalizeEdges(dag);
    if (!clock_ || !atomic_ || !dispatch_ || dag.dag_id.empty() ||
        dag.request_id.empty() || dag.nodes.empty() ||
        dag.schema_version != 2 ||
        dag.nodes.size() > admission.max_nodes ||
        dag.idempotency_key.empty() ||
        dag.deadline_mono_ns <= 0 ||
        call.deadline_mono_ns <= 0 ||
        call.request_id != dag.request_id ||
        call.trace_id.empty() ||
        call.principal_id_hash != admission.principal_id_hash ||
        call.priority != admission.granted_priority ||
        call.authorization_ref != admission.authorization_ref ||
        !isValidTaskPriority(call.priority) ||
        !isValidTaskPriority(admission.granted_priority) ||
        !isValidTaskPriority(dag.priority) ||
        call.deadline_mono_ns != admission.deadline_mono_ns ||
        admission.allowed_capabilities.empty() ||
        admission.principal_id_hash.empty() ||
        admission.policy_snapshot_id.empty() ||
        admission.policy_digest !=
            secureDigest(admission.policy_snapshot_id) ||
        admission.authorization_ref.empty() ||
        isHigherPriority(dag.priority,
                         admission.granted_priority) ||
        deadlineExpired(dag.deadline_mono_ns, *clock_) ||
        dag.deadline_mono_ns > admission.deadline_mono_ns) {
        result.reject_code = "ORCHESTRATOR_DAG_INVALID";
        return result;
    }
    if (admission.retry_policies.size() > 128 ||
        (!admission.retry_policies.empty() &&
         (admission.retry_policy_digest.empty() ||
          admission.retry_policy_digest !=
              retryPoliciesDigest(
                  admission.retry_policies)))) {
        result.reject_code =
            "ORCHESTRATOR_RETRY_POLICY_DIGEST_INVALID";
        return result;
    }
    for (const auto& retry : admission.retry_policies) {
        const auto& policy = retry.second;
        if (!safeRetryIdentifier(retry.first) ||
            admission.allowed_capabilities.count(
                retry.first) == 0 ||
            !retryingPolicy(policy.idempotency_policy) ||
            policy.retryable_errors.empty() ||
            policy.retryable_errors.size() > 64 ||
            policy.base_backoff_ns <= 0 ||
            policy.max_backoff_ns <
                policy.base_backoff_ns ||
            policy.max_backoff_ns >
                60LL * 1000LL * 1000LL * 1000LL ||
            std::any_of(
                policy.retryable_errors.begin(),
                policy.retryable_errors.end(),
                [](const auto& error) {
                    return !safeRetryIdentifier(error);
                })) {
            result.reject_code =
                "ORCHESTRATOR_RETRY_POLICY_INVALID";
            result.details.push_back(retry.first);
            return result;
        }
    }
    std::set<std::string> ids;
    for (const auto& node : normalized.nodes) {
        const bool trigger_enum_valid =
            static_cast<std::uint8_t>(node.trigger.type) <=
                static_cast<std::uint8_t>(TriggerType::Signal) &&
            static_cast<std::uint8_t>(
                node.trigger.repeat_policy) <=
                static_cast<std::uint8_t>(
                    RepeatPolicy::BoundedCron) &&
            static_cast<std::uint8_t>(
                node.trigger.missed_fire_policy) <=
                static_cast<std::uint8_t>(
                    MissedFirePolicy::CatchUpBounded) &&
            static_cast<std::uint8_t>(
                node.trigger.overlap_policy) <=
                static_cast<std::uint8_t>(
                    OverlapPolicy::ParallelBounded);
        const bool join_enum_valid =
            static_cast<std::uint8_t>(
                node.join_policy.kind) <=
            static_cast<std::uint8_t>(JoinKind::Quorum);
        const bool timer_valid =
            node.trigger.type != TriggerType::Timer ||
            (node.trigger.next_fire_at_utc_ms > 0 &&
             !node.trigger.source.empty());
        const bool signal_valid =
            node.trigger.type != TriggerType::Signal ||
            (!node.trigger.source.empty() &&
             node.trigger.expression.is_object() &&
             node.trigger.expression.contains("event_name") &&
             node.trigger.expression.at("event_name")
                 .is_string() &&
             !node.trigger.expression.at("event_name")
                  .get<std::string>()
                  .empty());
        const bool lifecycle_valid =
            node.lifecycle.max_activations > 0 &&
            node.trigger.catch_up_limit > 0 &&
            node.trigger.max_parallel > 0 &&
            ((node.trigger.repeat_policy == RepeatPolicy::Once &&
              node.lifecycle.max_activations == 1) ||
             (node.trigger.repeat_policy != RepeatPolicy::Once &&
              node.trigger.type != TriggerType::Immediate &&
              node.lifecycle.repeat_interval_ms > 0 &&
              node.lifecycle.max_activations > 1)) &&
            // Same-PID parallel execution requires a capability-level safety
            // declaration. The current Tool and Agent manifests do not expose
            // that declaration, so admission fails closed instead of silently
            // serializing a requested parallel policy.
            node.trigger.overlap_policy !=
                OverlapPolicy::ParallelBounded;
        const bool compensation_valid =
            !node.compensation.enabled ||
            ((node.compensation.executor ==
                  "atomic_service" ||
              node.compensation.executor ==
                  "agent_dispatch") &&
             !node.compensation.action.empty() &&
             node.compensation.params.is_object() &&
             admission.allowed_capabilities.count(
                 node.compensation.action) != 0);
        if (node.node_id.empty() || !ids.insert(node.node_id).second ||
            node.node_type != "ATOMIC" ||
            (node.executor != "atomic_service" &&
             node.executor != "agent_dispatch") ||
            node.action.empty() || !node.params.is_object() ||
            node.input_schema_version == 0 ||
            node.expected_output_schema_version == 0 ||
            node.max_attempts == 0 ||
            node.deadline_mono_ns <= 0 ||
            deadlineExpired(node.deadline_mono_ns, *clock_) ||
            (node.deadline_mono_ns > dag.deadline_mono_ns) ||
            !isValidTaskPriority(node.base_priority) ||
            !trigger_enum_valid || !join_enum_valid ||
            !node.trigger.expression.is_object() ||
            !timer_valid || !signal_valid ||
            !lifecycle_valid || !compensation_valid ||
            isHigherPriority(node.base_priority,
                             admission.granted_priority)) {
            result.reject_code = "ORCHESTRATOR_NODE_INVALID";
            result.details.push_back(node.node_id);
            return result;
        }
        if ((node.join_policy.kind == JoinKind::Quorum &&
             node.join_policy.quorum == 0) ||
            (node.join_policy.kind != JoinKind::Quorum &&
             node.join_policy.quorum != 0)) {
            result.reject_code =
                "ORCHESTRATOR_JOIN_POLICY_INVALID";
            result.details.push_back(node.node_id);
            return result;
        }
        for (const auto& binding :
             node.result_bindings) {
            if (binding.source_node_id.empty() ||
                binding.source_field_path.empty() ||
                binding.target_param.empty() ||
                binding.transform_id != "IDENTITY" ||
                (binding.on_missing != "FAIL_NODE" &&
                 binding.on_missing != "SKIP_PATH" &&
                 binding.on_missing != "USE_DEFAULT") ||
                binding.source_schema_version == 0 ||
                binding.target_schema_version == 0 ||
                (binding.on_missing == "USE_DEFAULT" &&
                 binding.default_value.is_discarded())) {
                result.reject_code =
                    "ORCHESTRATOR_RESULT_BINDING_INVALID";
                result.details.push_back(node.node_id);
                return result;
            }
        }
        if (admission.allowed_capabilities.count(node.action) == 0) {
            result.reject_code = "ORCHESTRATOR_CAPABILITY_NOT_ALLOWED";
            result.details.push_back(node.action);
            return result;
        }
        if (node.max_attempts > 1) {
            const auto retry =
                admission.retry_policies.find(node.action);
            if (retry == admission.retry_policies.end()) {
                result.reject_code =
                    "ORCHESTRATOR_RETRY_POLICY_REQUIRED";
                result.details.push_back(node.node_id);
                return result;
            }
            if (node.executor == "agent_dispatch") {
                // Agent manifests currently do not expose a signed retry
                // contract. Fail closed instead of trusting DAG claims.
                result.reject_code =
                    "ORCHESTRATOR_AGENT_RETRY_POLICY_UNAVAILABLE";
                result.details.push_back(node.node_id);
                return result;
            }
        }
        if (node.base_priority == TaskPriority::P0 &&
            (!admission.p0_authorization ||
             admission.authorization_ref.rfind(
                 "trusted-safety:", 0) != 0 ||
             admission.p0_allowed_capabilities.empty() ||
             admission.p0_allowed_capabilities.count(node.action) == 0)) {
            result.reject_code = "ORCHESTRATOR_P0_AUTHORIZATION_REQUIRED";
            result.details.push_back(node.node_id);
            return result;
        }
    }
    std::set<std::string> edge_ids;
    std::map<std::string, std::size_t> incoming_required;
    for (const auto& edge : dag.edges) {
        const bool known_edge_type =
            edge.edge_type == "SUCCESS" ||
            edge.edge_type == "FAILURE" ||
            edge.edge_type == "DATA" ||
            edge.edge_type == "RESULT_TRUE" ||
            edge.edge_type == "RESULT_FALSE";
        const bool predicate_required =
            edge.edge_type == "RESULT_TRUE" ||
            edge.edge_type == "RESULT_FALSE";
        if (edge.edge_id.empty() ||
            !edge_ids.insert(edge.edge_id).second ||
            ids.count(edge.from_node_id) == 0 ||
            ids.count(edge.to_node_id) == 0 ||
            edge.from_node_id == edge.to_node_id ||
            !known_edge_type || !edge.predicate.is_object() ||
            (predicate_required &&
             edge.predicate.empty())) {
            result.reject_code = "ORCHESTRATOR_EDGE_INVALID";
            result.details.push_back(edge.edge_id);
            return result;
        }
        if (edge.required) {
            ++incoming_required[edge.to_node_id];
        }
    }
    for (const auto& node : normalized.nodes) {
        for (const auto& dependency : node.dependencies) {
            if (dependency == node.node_id || ids.count(dependency) == 0) {
                result.reject_code = "ORCHESTRATOR_DEPENDENCY_INVALID";
                return result;
            }
        }
        for (const auto& binding :
             node.result_bindings) {
            if (binding.source_node_id == node.node_id ||
                ids.count(binding.source_node_id) == 0) {
                result.reject_code =
                    "ORCHESTRATOR_RESULT_BINDING_INVALID";
                result.details.push_back(node.node_id);
                return result;
            }
        }
        const auto incoming =
            incoming_required[node.node_id];
        if (node.join_policy.kind == JoinKind::Any &&
            incoming == 0 &&
            node.dependencies.empty()) {
            result.reject_code =
                "ORCHESTRATOR_JOIN_POLICY_INVALID";
            result.details.push_back(node.node_id);
            return result;
        }
        if (node.join_policy.kind == JoinKind::Quorum &&
            (node.join_policy.quorum >
                 std::max(incoming,
                          node.dependencies.size()) ||
             node.join_policy.quorum == 0)) {
            result.reject_code =
                "ORCHESTRATOR_JOIN_POLICY_INVALID";
            result.details.push_back(node.node_id);
            return result;
        }
    }
    if (!acyclic(normalized)) {
        result.reject_code = "ORCHESTRATOR_DAG_CYCLE";
        return result;
    }
    result.valid = true;
    return result;
}

// Submission establishes durable plan ownership before returning acceptance.
// Idempotent replay returns the existing plan only when the full digest matches.

bool Orchestrator::acyclic(const IntentDAG& dag) {
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> children;
    for (const auto& node : dag.nodes) indegree[node.node_id] = 0;
    for (const auto& node : dag.nodes) {
        for (const auto& dependency : node.dependencies) {
            children[dependency].push_back(node.node_id);
            ++indegree[node.node_id];
        }
    }
    std::deque<std::string> ready;
    for (const auto& pair : indegree) {
        if (pair.second == 0) ready.push_back(pair.first);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto current = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto& child : children[current]) {
            if (--indegree[child] == 0) ready.push_back(child);
        }
    }
    return visited == dag.nodes.size();
}

}  // namespace master_agent::orchestrator
