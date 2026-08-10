#pragma once

/**
 * @file orchestrator_wal_codec.h
 * @brief Private orchestrator WAL constants and snapshot serialization helpers.
 *
 * This header is private to Task Orchestration and is not part of the installed API.
 */

#include "master_agent/orchestrator/orchestrator.h"

#include <algorithm>
#include <cerrno>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace master_agent::orchestrator {
namespace {

constexpr std::uint32_t kOrchestratorWalSchemaVersion = 1;
constexpr const char* kOrchestratorWalGenesis = "GENESIS";

nlohmann::json resultBindingJson(const ResultBinding& binding) {
    return nlohmann::json{
        {"source_node_id", binding.source_node_id},
        {"source_field_path", binding.source_field_path},
        {"target_param", binding.target_param},
        {"transform_id", binding.transform_id},
        {"on_missing", binding.on_missing},
        {"default_value", binding.default_value},
        {"source_schema_version", binding.source_schema_version},
        {"target_schema_version", binding.target_schema_version}};
}

ResultBinding resultBindingFromJson(const nlohmann::json& encoded) {
    ResultBinding binding;
    binding.source_node_id =
        encoded.at("source_node_id").get<std::string>();
    binding.source_field_path =
        encoded.at("source_field_path").get<std::string>();
    binding.target_param =
        encoded.at("target_param").get<std::string>();
    binding.transform_id =
        encoded.value("transform_id", std::string{"IDENTITY"});
    binding.on_missing =
        encoded.value("on_missing", std::string{"FAIL_NODE"});
    binding.default_value =
        encoded.value("default_value", nlohmann::json{});
    binding.source_schema_version =
        encoded.value("source_schema_version", 1U);
    binding.target_schema_version =
        encoded.value("target_schema_version", 1U);
    return binding;
}

nlohmann::json dagNodeJson(const DAGNode& node) {
    nlohmann::json bindings = nlohmann::json::array();
    for (const auto& binding : node.result_bindings) {
        bindings.push_back(resultBindingJson(binding));
    }
    nlohmann::json compensation_bindings =
        nlohmann::json::array();
    for (const auto& binding :
         node.compensation.result_bindings) {
        compensation_bindings.push_back(
            resultBindingJson(binding));
    }
    return nlohmann::json{
        {"node_id", node.node_id},
        {"node_type", node.node_type},
        {"executor", node.executor},
        {"action", node.action},
        {"target_agent", node.target_agent},
        {"allow_agent_fallback", node.allow_agent_fallback},
        {"params", node.params},
        {"dependencies", node.dependencies},
        {"input_schema_version", node.input_schema_version},
        {"expected_output_schema_version",
         node.expected_output_schema_version},
        {"base_priority",
         static_cast<std::uint8_t>(node.base_priority)},
        {"deadline_mono_ns", node.deadline_mono_ns},
        {"max_attempts", node.max_attempts},
        {"resource_requirements", node.resource_requirements},
        {"trigger",
         {{"type",
           static_cast<std::uint8_t>(node.trigger.type)},
          {"source", node.trigger.source},
          {"expression", node.trigger.expression},
          {"repeat_policy",
           static_cast<std::uint8_t>(
               node.trigger.repeat_policy)},
          {"next_fire_at_utc_ms",
           node.trigger.next_fire_at_utc_ms},
          {"missed_fire_policy",
           static_cast<std::uint8_t>(
               node.trigger.missed_fire_policy)},
          {"overlap_policy",
           static_cast<std::uint8_t>(
               node.trigger.overlap_policy)},
          {"catch_up_limit", node.trigger.catch_up_limit},
          {"max_parallel", node.trigger.max_parallel}}},
        {"join_policy",
         {{"kind",
           static_cast<std::uint8_t>(
               node.join_policy.kind)},
          {"quorum", node.join_policy.quorum}}},
        {"result_bindings", bindings},
        {"lifecycle",
         {{"max_activations",
           node.lifecycle.max_activations},
          {"repeat_interval_ms",
           node.lifecycle.repeat_interval_ms},
          {"end_at_utc_ms",
           node.lifecycle.end_at_utc_ms}}},
        {"compensation",
         {{"enabled", node.compensation.enabled},
          {"executor", node.compensation.executor},
          {"action", node.compensation.action},
          {"target_agent",
           node.compensation.target_agent},
          {"params", node.compensation.params},
          {"result_bindings", compensation_bindings}}}};
}

DAGNode dagNodeFromJson(const nlohmann::json& encoded) {
    DAGNode node;
    node.node_id = encoded.at("node_id").get<std::string>();
    node.node_type = encoded.at("node_type").get<std::string>();
    node.executor = encoded.at("executor").get<std::string>();
    node.action = encoded.at("action").get<std::string>();
    node.target_agent =
        encoded.at("target_agent").get<std::string>();
    node.allow_agent_fallback =
        encoded.at("allow_agent_fallback").get<bool>();
    node.params = encoded.at("params");
    node.dependencies =
        encoded.at("dependencies")
            .get<std::vector<std::string>>();
    node.input_schema_version =
        encoded.at("input_schema_version").get<std::uint32_t>();
    node.expected_output_schema_version =
        encoded.at("expected_output_schema_version")
            .get<std::uint32_t>();
    node.base_priority = static_cast<TaskPriority>(
        encoded.at("base_priority").get<std::uint8_t>());
    node.deadline_mono_ns =
        encoded.at("deadline_mono_ns").get<std::int64_t>();
    node.max_attempts =
        encoded.at("max_attempts").get<std::uint32_t>();
    node.resource_requirements =
        encoded.at("resource_requirements")
            .get<std::vector<std::string>>();
    if (const auto trigger = encoded.find("trigger");
        trigger != encoded.end()) {
        node.trigger.type = static_cast<TriggerType>(
            trigger->value("type", 0U));
        node.trigger.source =
            trigger->value("source", std::string{});
        node.trigger.expression =
            trigger->value(
                "expression", nlohmann::json::object());
        node.trigger.repeat_policy =
            static_cast<RepeatPolicy>(
                trigger->value("repeat_policy", 0U));
        node.trigger.next_fire_at_utc_ms =
            trigger->value("next_fire_at_utc_ms", 0LL);
        node.trigger.missed_fire_policy =
            static_cast<MissedFirePolicy>(
                trigger->value("missed_fire_policy", 0U));
        node.trigger.overlap_policy =
            static_cast<OverlapPolicy>(
                trigger->value("overlap_policy", 0U));
        node.trigger.catch_up_limit =
            trigger->value("catch_up_limit", 1U);
        node.trigger.max_parallel =
            trigger->value("max_parallel", 1U);
    }
    if (const auto join = encoded.find("join_policy");
        join != encoded.end()) {
        node.join_policy.kind = static_cast<JoinKind>(
            join->value("kind", 0U));
        node.join_policy.quorum =
            join->value("quorum", 0U);
    }
    if (const auto bindings =
            encoded.find("result_bindings");
        bindings != encoded.end()) {
        for (const auto& binding : *bindings) {
            node.result_bindings.push_back(
                resultBindingFromJson(binding));
        }
    }
    if (const auto lifecycle = encoded.find("lifecycle");
        lifecycle != encoded.end()) {
        node.lifecycle.max_activations =
            lifecycle->value("max_activations", 1U);
        node.lifecycle.repeat_interval_ms =
            lifecycle->value("repeat_interval_ms", 0LL);
        node.lifecycle.end_at_utc_ms =
            lifecycle->value("end_at_utc_ms", 0LL);
    }
    if (const auto compensation =
            encoded.find("compensation");
        compensation != encoded.end()) {
        node.compensation.enabled =
            compensation->value("enabled", false);
        node.compensation.executor =
            compensation->value(
                "executor",
                std::string{"atomic_service"});
        node.compensation.action =
            compensation->value("action", std::string{});
        node.compensation.target_agent =
            compensation->value(
                "target_agent", std::string{});
        node.compensation.params =
            compensation->value(
                "params", nlohmann::json::object());
        if (const auto bindings =
                compensation->find("result_bindings");
            bindings != compensation->end()) {
            for (const auto& binding : *bindings) {
                node.compensation.result_bindings.push_back(
                    resultBindingFromJson(binding));
            }
        }
    }
    if (node.node_id.empty() || node.node_type != "ATOMIC" ||
        (node.executor != "atomic_service" &&
         node.executor != "agent_dispatch") ||
        node.action.empty() || !node.params.is_object() ||
        node.input_schema_version == 0 ||
        node.expected_output_schema_version == 0 ||
        !isValidTaskPriority(node.base_priority) ||
        node.deadline_mono_ns <= 0 || node.max_attempts == 0) {
        throw std::runtime_error("invalid recovered DAG node");
    }
    return node;
}

nlohmann::json dagEdgeJson(const DAGEdge& edge) {
    return nlohmann::json{
        {"edge_id", edge.edge_id},
        {"from_node_id", edge.from_node_id},
        {"to_node_id", edge.to_node_id},
        {"edge_type", edge.edge_type},
        {"predicate", edge.predicate},
        {"required", edge.required}};
}

DAGEdge dagEdgeFromJson(const nlohmann::json& encoded) {
    DAGEdge edge;
    edge.edge_id = encoded.at("edge_id").get<std::string>();
    edge.from_node_id =
        encoded.at("from_node_id").get<std::string>();
    edge.to_node_id =
        encoded.at("to_node_id").get<std::string>();
    edge.edge_type =
        encoded.at("edge_type").get<std::string>();
    edge.predicate =
        encoded.value(
            "predicate", nlohmann::json::object());
    edge.required = encoded.at("required").get<bool>();
    return edge;
}

nlohmann::json taskEventJson(const TaskEvent& event) {
    return nlohmann::json{
        {"event_id", event.event_id},
        {"event_type", event.event_type},
        {"plan_id", event.plan_id},
        {"pid", event.pid},
        {"activation_id", event.activation_id},
        {"execution_id", event.execution_id},
        {"plan_version", event.plan_version},
        {"orchestrator_epoch", event.orchestrator_epoch},
        {"occurred_at_utc_ms", event.occurred_at_utc_ms},
        {"payload_digest", event.payload_digest},
        {"trace_id", event.trace_id}};
}

TaskEvent taskEventFromJson(const nlohmann::json& encoded) {
    TaskEvent event;
    event.event_id =
        encoded.at("event_id").get<std::string>();
    event.event_type =
        encoded.at("event_type").get<std::string>();
    event.plan_id =
        encoded.at("plan_id").get<std::string>();
    event.pid = encoded.at("pid").get<std::string>();
    event.activation_id =
        encoded.at("activation_id").get<std::string>();
    event.execution_id =
        encoded.at("execution_id").get<std::string>();
    event.plan_version =
        encoded.at("plan_version").get<std::uint64_t>();
    event.orchestrator_epoch =
        encoded.at("orchestrator_epoch")
            .get<std::uint64_t>();
    event.occurred_at_utc_ms =
        encoded.at("occurred_at_utc_ms")
            .get<std::int64_t>();
    event.payload_digest =
        encoded.at("payload_digest").get<std::string>();
    event.trace_id =
        encoded.at("trace_id").get<std::string>();
    if (event.event_id.empty() || event.event_type.empty() ||
        event.plan_id.empty() || event.plan_version == 0 ||
        event.orchestrator_epoch == 0 ||
        event.payload_digest.empty() || event.trace_id.empty()) {
        throw std::runtime_error(
            "invalid recovered TaskEvent");
    }
    return event;
}

nlohmann::json admissionJson(const AdmissionContext& admission) {
    nlohmann::json retry = nlohmann::json::object();
    for (const auto& [capability, policy] :
         admission.retry_policies) {
        retry[capability] =
            nlohmann::json{
                {"idempotency_policy",
                 policy.idempotency_policy},
                {"retryable_errors", policy.retryable_errors},
                {"base_backoff_ns", policy.base_backoff_ns},
                {"max_backoff_ns", policy.max_backoff_ns}};
    }
    return nlohmann::json{
        {"principal_id_hash", admission.principal_id_hash},
        {"source_type", admission.source_type},
        {"granted_priority",
         static_cast<std::uint8_t>(admission.granted_priority)},
        {"p0_authorization", admission.p0_authorization},
        {"p0_allowed_capabilities",
         admission.p0_allowed_capabilities},
        {"policy_snapshot_id", admission.policy_snapshot_id},
        {"policy_digest", admission.policy_digest},
        {"authorization_ref", admission.authorization_ref},
        {"max_nodes", admission.max_nodes},
        {"allowed_capabilities",
         admission.allowed_capabilities},
        {"granted_permissions",
         admission.granted_permissions},
        {"retry_policies", retry},
        {"retry_policy_digest",
         admission.retry_policy_digest},
        {"deadline_mono_ns", admission.deadline_mono_ns}};
}

AdmissionContext admissionFromJson(
    const nlohmann::json& encoded) {
    AdmissionContext admission;
    admission.principal_id_hash =
        encoded.at("principal_id_hash").get<std::string>();
    admission.source_type =
        encoded.at("source_type").get<std::string>();
    admission.granted_priority = static_cast<TaskPriority>(
        encoded.at("granted_priority").get<std::uint8_t>());
    admission.p0_authorization =
        encoded.at("p0_authorization").get<bool>();
    admission.p0_allowed_capabilities =
        encoded.at("p0_allowed_capabilities")
            .get<std::set<std::string>>();
    admission.policy_snapshot_id =
        encoded.at("policy_snapshot_id").get<std::string>();
    admission.policy_digest =
        encoded.at("policy_digest").get<std::string>();
    admission.authorization_ref =
        encoded.at("authorization_ref").get<std::string>();
    admission.max_nodes =
        encoded.at("max_nodes").get<std::size_t>();
    admission.allowed_capabilities =
        encoded.at("allowed_capabilities")
            .get<std::set<std::string>>();
    admission.granted_permissions =
        encoded.at("granted_permissions")
            .get<std::set<std::string>>();
    for (const auto& item :
         encoded.at("retry_policies").items()) {
        CapabilityRetryPolicy policy;
        policy.idempotency_policy =
            item.value()
                .at("idempotency_policy")
                .get<std::string>();
        policy.retryable_errors =
            item.value()
                .at("retryable_errors")
                .get<std::set<std::string>>();
        policy.base_backoff_ns =
            item.value()
                .at("base_backoff_ns")
                .get<std::int64_t>();
        policy.max_backoff_ns =
            item.value()
                .at("max_backoff_ns")
                .get<std::int64_t>();
        admission.retry_policies.emplace(
            item.key(), std::move(policy));
    }
    admission.retry_policy_digest =
        encoded.at("retry_policy_digest").get<std::string>();
    admission.deadline_mono_ns =
        encoded.at("deadline_mono_ns").get<std::int64_t>();
    if (admission.principal_id_hash.empty() ||
        admission.source_type.empty() ||
        !isValidTaskPriority(admission.granted_priority) ||
        admission.policy_snapshot_id.empty() ||
        admission.policy_digest.empty() ||
        admission.authorization_ref.empty() ||
        admission.max_nodes == 0 ||
        admission.allowed_capabilities.empty() ||
        admission.deadline_mono_ns <= 0 ||
        (!admission.retry_policies.empty() &&
         admission.retry_policy_digest !=
             retryPoliciesDigest(admission.retry_policies))) {
        throw std::runtime_error(
            "invalid recovered Admission");
    }
    return admission;
}

nlohmann::json nodeRuntimeJson(
    const NodeRuntimeSnapshot& node) {
    nlohmann::json activation_history = nlohmann::json::array();
    for (const auto& activation : node.activation_history) {
        activation_history.push_back({
            {"activation_id", activation.activation_id},
            {"trigger_instance_id", activation.trigger_instance_id},
            {"state", static_cast<std::uint8_t>(activation.state)},
            {"attempt_count", activation.attempt_count},
            {"execution_id", activation.execution_id},
            {"operation_id", activation.operation_id},
            {"result", activation.result},
            {"error_code", activation.error_code},
            {"side_effect_state",
             static_cast<std::uint8_t>(activation.side_effect_state)},
            {"terminal_at_utc_ms", activation.terminal_at_utc_ms}});
    }
    return nlohmann::json{
        {"definition", dagNodeJson(node.definition)},
        {"pid", node.pid},
        {"activation_id", node.activation_id},
        {"state", static_cast<std::uint8_t>(node.state)},
        {"effective_priority",
         static_cast<std::uint8_t>(node.effective_priority)},
        {"attempt_count", node.attempt_count},
        {"execution_id", node.execution_id},
        {"operation_id", node.operation_id},
        {"fencing_token", node.fencing_token},
        {"result", node.result},
        {"error_code", node.error_code},
        {"side_effect_state",
         static_cast<std::uint8_t>(node.side_effect_state)},
        {"retryable_hint", node.retryable_hint},
        {"retry_at_mono_ns", node.retry_at_mono_ns},
        {"trigger_satisfied", node.trigger_satisfied},
        {"trigger_instance_id", node.trigger_instance_id},
        {"blockers", node.blockers},
        {"bound_params", node.bound_params},
        {"activation_count", node.activation_count},
        {"next_fire_at_utc_ms", node.next_fire_at_utc_ms},
        {"pending_trigger_count", node.pending_trigger_count},
        {"pending_trigger_instance_id",
         node.pending_trigger_instance_id},
        {"activation_history", activation_history}};
}

NodeRuntimeSnapshot nodeRuntimeFromJson(
    const nlohmann::json& encoded) {
    NodeRuntimeSnapshot node;
    node.definition =
        dagNodeFromJson(encoded.at("definition"));
    node.pid = encoded.at("pid").get<std::string>();
    node.activation_id =
        encoded.at("activation_id").get<std::string>();
    const auto state =
        encoded.at("state").get<std::uint8_t>();
    if (state >
        static_cast<std::uint8_t>(ActivationState::Unknown)) {
        throw std::runtime_error(
            "invalid recovered Activation state");
    }
    node.state = static_cast<ActivationState>(state);
    node.effective_priority = static_cast<TaskPriority>(
        encoded.at("effective_priority").get<std::uint8_t>());
    node.attempt_count =
        encoded.at("attempt_count").get<std::uint32_t>();
    node.execution_id =
        encoded.at("execution_id").get<std::string>();
    node.operation_id =
        encoded.at("operation_id").get<std::string>();
    node.fencing_token =
        encoded.at("fencing_token").get<std::uint64_t>();
    node.result = encoded.at("result");
    node.error_code =
        encoded.at("error_code").get<std::string>();
    node.side_effect_state = static_cast<SideEffectState>(
        encoded.at("side_effect_state").get<std::uint8_t>());
    node.retryable_hint =
        encoded.at("retryable_hint").get<bool>();
    node.retry_at_mono_ns =
        encoded.at("retry_at_mono_ns").get<std::int64_t>();
    node.trigger_satisfied =
        encoded.value("trigger_satisfied", true);
    node.trigger_instance_id =
        encoded.value(
            "trigger_instance_id", std::string{});
    node.blockers =
        encoded.value(
            "blockers", std::vector<std::string>{});
    node.bound_params =
        encoded.value(
            "bound_params", nlohmann::json::object());
    node.activation_count = encoded.value(
        "activation_count", node.activation_id.empty() ? 0U : 1U);
    node.next_fire_at_utc_ms = encoded.value(
        "next_fire_at_utc_ms",
        node.definition.trigger.next_fire_at_utc_ms);
    node.pending_trigger_count = encoded.value(
        "pending_trigger_count", 0U);
    node.pending_trigger_instance_id = encoded.value(
        "pending_trigger_instance_id", std::string{});
    if (const auto history = encoded.find("activation_history");
        history != encoded.end()) {
        if (!history->is_array()) {
            throw std::runtime_error("invalid Activation history");
        }
        for (const auto& item : *history) {
            ActivationHistoryEntry activation;
            activation.activation_id =
                item.at("activation_id").get<std::string>();
            activation.trigger_instance_id =
                item.at("trigger_instance_id").get<std::string>();
            const auto history_state =
                item.at("state").get<std::uint8_t>();
            if (history_state > static_cast<std::uint8_t>(
                                    ActivationState::Unknown)) {
                throw std::runtime_error(
                    "invalid Activation history state");
            }
            activation.state =
                static_cast<ActivationState>(history_state);
            activation.attempt_count =
                item.at("attempt_count").get<std::uint32_t>();
            activation.execution_id =
                item.at("execution_id").get<std::string>();
            activation.operation_id =
                item.at("operation_id").get<std::string>();
            activation.result = item.at("result");
            activation.error_code =
                item.at("error_code").get<std::string>();
            activation.side_effect_state =
                static_cast<SideEffectState>(
                    item.at("side_effect_state")
                        .get<std::uint8_t>());
            activation.terminal_at_utc_ms =
                item.at("terminal_at_utc_ms")
                    .get<std::int64_t>();
            if (activation.activation_id.empty() ||
                !nodeTerminal(activation.state) ||
                !activation.result.is_object() ||
                !isValidSideEffectState(
                    activation.side_effect_state)) {
                throw std::runtime_error(
                    "invalid Activation history entry");
            }
            node.activation_history.push_back(
                std::move(activation));
        }
    }
    const bool waiting_for_trigger =
        node.state == ActivationState::Blocked &&
        !node.trigger_satisfied;
    if (node.pid.empty() ||
        (node.activation_id.empty() && !waiting_for_trigger) ||
        !isValidTaskPriority(node.effective_priority) ||
        !isValidSideEffectState(node.side_effect_state) ||
        !node.result.is_object()) {
        throw std::runtime_error(
            "invalid recovered Activation");
    }
    return node;
}

nlohmann::json planSnapshotJson(
    const TaskPlanSnapshot& snapshot) {
    nlohmann::json nodes = nlohmann::json::object();
    for (const auto& [node_id, node] : snapshot.nodes) {
        nodes[node_id] = nodeRuntimeJson(node);
    }
    return nlohmann::json{
        {"plan_id", snapshot.plan_id},
        {"request_id", snapshot.request_id},
        {"state", static_cast<std::uint8_t>(snapshot.state)},
        {"summary_priority",
         static_cast<std::uint8_t>(snapshot.summary_priority)},
        {"active_priority",
         static_cast<std::uint8_t>(snapshot.active_priority)},
        {"effective_deadline_mono_ns",
         snapshot.effective_deadline_mono_ns},
        {"capability_snapshot_id",
         snapshot.capability_snapshot_id},
        {"policy_snapshot_id", snapshot.policy_snapshot_id},
        {"control_epoch", snapshot.control_epoch},
        {"version", snapshot.version},
        {"nodes", nodes},
        {"trace_id", snapshot.trace_id},
        {"cancellation_requested",
         snapshot.cancellation_requested},
        {"parent_plan_id", snapshot.parent_plan_id}};
}

TaskPlanSnapshot planSnapshotFromJson(
    const nlohmann::json& encoded) {
    TaskPlanSnapshot snapshot;
    snapshot.plan_id =
        encoded.at("plan_id").get<std::string>();
    snapshot.request_id =
        encoded.at("request_id").get<std::string>();
    const auto state =
        encoded.at("state").get<std::uint8_t>();
    if (state >
        static_cast<std::uint8_t>(PlanState::Compensating)) {
        throw std::runtime_error("invalid recovered Plan state");
    }
    snapshot.state = static_cast<PlanState>(state);
    snapshot.summary_priority = static_cast<TaskPriority>(
        encoded.at("summary_priority").get<std::uint8_t>());
    snapshot.active_priority = static_cast<TaskPriority>(
        encoded.at("active_priority").get<std::uint8_t>());
    snapshot.effective_deadline_mono_ns =
        encoded.at("effective_deadline_mono_ns")
            .get<std::int64_t>();
    snapshot.capability_snapshot_id =
        encoded.at("capability_snapshot_id").get<std::string>();
    snapshot.policy_snapshot_id =
        encoded.at("policy_snapshot_id").get<std::string>();
    snapshot.control_epoch =
        encoded.at("control_epoch").get<std::uint64_t>();
    snapshot.version =
        encoded.at("version").get<std::uint64_t>();
    for (const auto& item : encoded.at("nodes").items()) {
        auto node = nodeRuntimeFromJson(item.value());
        if (item.key() != node.definition.node_id ||
            snapshot.nodes.count(item.key()) != 0) {
            throw std::runtime_error(
                "invalid recovered Plan node index");
        }
        snapshot.nodes.emplace(item.key(), std::move(node));
    }
    snapshot.trace_id =
        encoded.at("trace_id").get<std::string>();
    snapshot.cancellation_requested =
        encoded.value("cancellation_requested", false);
    snapshot.parent_plan_id =
        encoded.value("parent_plan_id", std::string{});
    if (snapshot.plan_id.empty() || snapshot.request_id.empty() ||
        !isValidTaskPriority(snapshot.summary_priority) ||
        !isValidTaskPriority(snapshot.active_priority) ||
        snapshot.effective_deadline_mono_ns <= 0 ||
        snapshot.capability_snapshot_id.empty() ||
        snapshot.policy_snapshot_id.empty() ||
        snapshot.version == 0 || snapshot.nodes.empty() ||
        snapshot.trace_id.empty()) {
        throw std::runtime_error("invalid recovered Plan");
    }
    return snapshot;
}

}  // namespace
}  // namespace master_agent::orchestrator

