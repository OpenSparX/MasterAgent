/**
 * @file persistence_store.cpp
 * @brief Persists Agent registry epochs, dispatch ownership, and reliable event cursors.
 *
 * Design trace: Sub-Agent Dispatch Engine Detailed Design, sections 3.2,
 * 4.2, 4.4, 9.3, and 11. Non-terminal work recovered without a live
 * provider is conservatively marked UNKNOWN until exact-epoch reconciliation.
 */

#include "include/dispatch_task_identity.h"

#include <fstream>
#include <utility>

namespace master_agent::agent_dispatch {
namespace {

nlohmann::json manifestJson(
    const sub_agents::AgentManifest& manifest) {
    return {
        {"agent_id", manifest.agent_id},
        {"agent_epoch", manifest.agent_epoch},
        {"manifest_digest", manifest.manifest_digest},
        {"capability_version", manifest.capability_version},
        {"capabilities", manifest.capabilities},
        {"required_permissions", manifest.required_permissions},
        {"required_atomic_tools", manifest.required_atomic_tools},
        {"max_concurrency", manifest.max_concurrency},
        {"reserved_p0_slots", manifest.reserved_p0_slots},
        {"supports_safe_point_preemption",
         manifest.supports_safe_point_preemption},
        {"prompt_profile_id", manifest.prompt_profile_id},
        {"model_profile_id", manifest.model_profile_id}};
}

sub_agents::AgentManifest manifestFromJson(
    const nlohmann::json& encoded) {
    sub_agents::AgentManifest manifest;
    manifest.agent_id = encoded.at("agent_id").get<std::string>();
    manifest.agent_epoch =
        encoded.at("agent_epoch").get<std::uint64_t>();
    manifest.manifest_digest =
        encoded.at("manifest_digest").get<std::string>();
    manifest.capability_version =
        encoded.at("capability_version").get<std::string>();
    manifest.capabilities =
        encoded.at("capabilities").get<std::vector<std::string>>();
    manifest.required_permissions =
        encoded.at("required_permissions")
            .get<std::map<std::string, std::vector<std::string>>>();
    manifest.required_atomic_tools =
        encoded.at("required_atomic_tools")
            .get<std::map<std::string, std::vector<std::string>>>();
    manifest.max_concurrency =
        encoded.at("max_concurrency").get<std::uint32_t>();
    manifest.reserved_p0_slots =
        encoded.at("reserved_p0_slots").get<std::uint32_t>();
    manifest.supports_safe_point_preemption =
        encoded.at("supports_safe_point_preemption").get<bool>();
    manifest.prompt_profile_id =
        encoded.at("prompt_profile_id").get<std::string>();
    manifest.model_profile_id =
        encoded.at("model_profile_id").get<std::string>();
    if (manifest.agent_id.empty() || manifest.agent_epoch == 0 ||
        manifest.manifest_digest.empty() ||
        manifest.capabilities.empty() ||
        manifest.max_concurrency == 0 ||
        manifest.reserved_p0_slots > manifest.max_concurrency) {
        throw std::runtime_error("invalid durable Agent manifest");
    }
    return manifest;
}

nlohmann::json taskJson(const DispatchTask& task) {
    return {
        {"caller_module_id", static_cast<std::uint8_t>(task.caller_module_id)},
        {"request_id", task.request_id}, {"plan_id", task.plan_id},
        {"pid", task.pid}, {"activation_id", task.activation_id},
        {"execution_id", task.execution_id}, {"attempt_no", task.attempt_no},
        {"operation_id", task.operation_id}, {"task_id", task.task_id},
        {"action", task.action}, {"target_agent", task.target_agent},
        {"allow_agent_fallback", task.allow_agent_fallback},
        {"params", task.params},
        {"input_schema_version", task.input_schema_version},
        {"expected_output_schema_version",
         task.expected_output_schema_version},
        {"priority", static_cast<std::uint8_t>(task.priority)},
        {"deadline_mono_ns", task.deadline_mono_ns},
        {"idempotency_key", task.idempotency_key},
        {"fencing_token", task.fencing_token},
        {"capability_digest", task.capability_digest},
        {"capacity_epoch", task.capacity_epoch},
        {"resource_lease_refs", task.resource_lease_refs},
        {"granted_permissions", task.granted_permissions},
        {"allowed_child_capabilities", task.allowed_child_capabilities},
        {"child_authorization_digest", task.child_authorization_digest},
        {"principal_id_hash", task.principal_id_hash},
        {"authorization_ref", task.authorization_ref},
        {"trace_id", task.trace_id}};
}

DispatchTask taskFromJson(const nlohmann::json& encoded) {
    DispatchTask task;
    task.caller_module_id = static_cast<CallerModuleId>(
        encoded.at("caller_module_id").get<std::uint8_t>());
    task.request_id = encoded.at("request_id").get<std::string>();
    task.plan_id = encoded.at("plan_id").get<std::string>();
    task.pid = encoded.at("pid").get<std::string>();
    task.activation_id = encoded.at("activation_id").get<std::string>();
    task.execution_id = encoded.at("execution_id").get<std::string>();
    task.attempt_no = encoded.at("attempt_no").get<std::uint32_t>();
    task.operation_id = encoded.at("operation_id").get<std::string>();
    task.task_id = encoded.at("task_id").get<std::string>();
    task.action = encoded.at("action").get<std::string>();
    task.target_agent = encoded.at("target_agent").get<std::string>();
    task.allow_agent_fallback =
        encoded.at("allow_agent_fallback").get<bool>();
    task.params = encoded.at("params");
    task.input_schema_version =
        encoded.at("input_schema_version").get<std::uint32_t>();
    task.expected_output_schema_version =
        encoded.at("expected_output_schema_version").get<std::uint32_t>();
    task.priority = static_cast<TaskPriority>(
        encoded.at("priority").get<std::uint8_t>());
    task.deadline_mono_ns =
        encoded.at("deadline_mono_ns").get<std::int64_t>();
    task.idempotency_key =
        encoded.at("idempotency_key").get<std::string>();
    task.fencing_token =
        encoded.at("fencing_token").get<std::uint64_t>();
    task.capability_digest =
        encoded.at("capability_digest").get<std::string>();
    task.capacity_epoch =
        encoded.at("capacity_epoch").get<std::uint64_t>();
    task.resource_lease_refs =
        encoded.at("resource_lease_refs").get<std::vector<std::string>>();
    task.granted_permissions =
        encoded.at("granted_permissions").get<std::vector<std::string>>();
    task.allowed_child_capabilities = encoded.at("allowed_child_capabilities")
                                          .get<std::vector<std::string>>();
    task.child_authorization_digest =
        encoded.at("child_authorization_digest").get<std::string>();
    task.principal_id_hash =
        encoded.at("principal_id_hash").get<std::string>();
    task.authorization_ref =
        encoded.at("authorization_ref").get<std::string>();
    task.trace_id = encoded.at("trace_id").get<std::string>();
    return task;
}

nlohmann::json routeJson(const AgentRouteDecision& route) {
    return {{"routed", route.routed}, {"agent_id", route.agent_id},
            {"agent_epoch", route.agent_epoch},
            {"manifest_digest", route.manifest_digest},
            {"capability_version", route.capability_version},
            {"lease_id", route.lease_id},
            {"reason_code", route.reason_code},
            {"route_decided_at_utc_ms", route.route_decided_at_utc_ms}};
}

AgentRouteDecision routeFromJson(const nlohmann::json& encoded) {
    AgentRouteDecision route;
    route.routed = encoded.at("routed").get<bool>();
    route.agent_id = encoded.at("agent_id").get<std::string>();
    route.agent_epoch = encoded.at("agent_epoch").get<std::uint64_t>();
    route.manifest_digest =
        encoded.at("manifest_digest").get<std::string>();
    route.capability_version =
        encoded.at("capability_version").get<std::string>();
    route.lease_id = encoded.at("lease_id").get<std::string>();
    route.reason_code = encoded.at("reason_code").get<std::string>();
    route.route_decided_at_utc_ms =
        encoded.at("route_decided_at_utc_ms").get<std::int64_t>();
    return route;
}

nlohmann::json snapshotJson(const DispatchSnapshot& snapshot) {
    return {{"dispatch_id", snapshot.dispatch_id},
            {"task", taskJson(snapshot.task)},
            {"route", routeJson(snapshot.route)},
            {"state", static_cast<std::uint8_t>(snapshot.state)},
            {"result", snapshot.result},
            {"error_code", snapshot.error_code},
            {"side_effect_state",
             static_cast<std::uint8_t>(snapshot.side_effect_state)},
            {"checkpoint_ref", snapshot.checkpoint_ref},
            {"control_epoch", snapshot.control_epoch},
            {"enqueue_sequence", snapshot.enqueue_sequence},
            {"retryable_hint", snapshot.retryable_hint}};
}

DispatchSnapshot snapshotFromJson(const nlohmann::json& encoded) {
    DispatchSnapshot snapshot;
    snapshot.dispatch_id = encoded.at("dispatch_id").get<std::string>();
    snapshot.task = taskFromJson(encoded.at("task"));
    snapshot.route = routeFromJson(encoded.at("route"));
    snapshot.state = static_cast<DispatchState>(
        encoded.at("state").get<std::uint8_t>());
    snapshot.result = encoded.at("result");
    snapshot.error_code = encoded.at("error_code").get<std::string>();
    snapshot.side_effect_state = static_cast<SideEffectState>(
        encoded.at("side_effect_state").get<std::uint8_t>());
    snapshot.checkpoint_ref =
        encoded.at("checkpoint_ref").get<std::string>();
    snapshot.control_epoch =
        encoded.at("control_epoch").get<std::uint64_t>();
    snapshot.enqueue_sequence =
        encoded.at("enqueue_sequence").get<std::uint64_t>();
    snapshot.retryable_hint = encoded.at("retryable_hint").get<bool>();
    return snapshot;
}

nlohmann::json eventJson(const DispatchEvent& event) {
    return {{"event_id", event.event_id}, {"event_type", event.event_type},
            {"dispatch_id", event.dispatch_id},
            {"request_id", event.request_id}, {"plan_id", event.plan_id},
            {"pid", event.pid}, {"activation_id", event.activation_id},
            {"execution_id", event.execution_id},
            {"attempt_no", event.attempt_no},
            {"operation_id", event.operation_id},
            {"agent_id", event.agent_id}, {"agent_epoch", event.agent_epoch},
            {"lease_id", event.lease_id},
            {"state", static_cast<std::uint8_t>(event.state)},
            {"fencing_token", event.fencing_token},
            {"side_effect_state",
             static_cast<std::uint8_t>(event.side_effect_state)},
            {"result", event.result},
            {"output_schema_version", event.output_schema_version},
            {"error_code", event.error_code}, {"safe_point", event.safe_point},
            {"checkpoint_ref", event.checkpoint_ref},
            {"resource_released", event.resource_released},
            {"occurred_at_utc_ms", event.occurred_at_utc_ms},
            {"trace_id", event.trace_id}};
}

DispatchEvent eventFromJson(const nlohmann::json& encoded) {
    DispatchEvent event;
    event.event_id = encoded.at("event_id").get<std::string>();
    event.event_type = encoded.at("event_type").get<std::string>();
    event.dispatch_id = encoded.at("dispatch_id").get<std::string>();
    event.request_id = encoded.at("request_id").get<std::string>();
    event.plan_id = encoded.at("plan_id").get<std::string>();
    event.pid = encoded.at("pid").get<std::string>();
    event.activation_id = encoded.at("activation_id").get<std::string>();
    event.execution_id = encoded.at("execution_id").get<std::string>();
    event.attempt_no = encoded.at("attempt_no").get<std::uint32_t>();
    event.operation_id = encoded.at("operation_id").get<std::string>();
    event.agent_id = encoded.at("agent_id").get<std::string>();
    event.agent_epoch = encoded.at("agent_epoch").get<std::uint64_t>();
    event.lease_id = encoded.at("lease_id").get<std::string>();
    event.state = static_cast<DispatchState>(
        encoded.at("state").get<std::uint8_t>());
    event.fencing_token =
        encoded.at("fencing_token").get<std::uint64_t>();
    event.side_effect_state = static_cast<SideEffectState>(
        encoded.at("side_effect_state").get<std::uint8_t>());
    event.result = encoded.at("result");
    event.output_schema_version =
        encoded.at("output_schema_version").get<std::uint32_t>();
    event.error_code = encoded.at("error_code").get<std::string>();
    event.safe_point = encoded.at("safe_point").get<bool>();
    event.checkpoint_ref = encoded.at("checkpoint_ref").get<std::string>();
    event.resource_released =
        encoded.at("resource_released").get<bool>();
    event.occurred_at_utc_ms =
        encoded.at("occurred_at_utc_ms").get<std::int64_t>();
    event.trace_id = encoded.at("trace_id").get<std::string>();
    return event;
}

}  // namespace

Status AgentDispatch::persistStateUnlocked() const {
    if (storage_directory_.empty()) return Status::Ok();
    nlohmann::json state;
    state["schema_version"] = 1;
    state["capacity_epoch"] = capacity_epoch_;
    state["enqueue_sequence"] = enqueue_sequence_;
    state["control_sequence"] = control_sequence_;
    state["accepting"] = accepting_;
    state["registry_epochs"] = recovered_agent_epochs_;
    for (const auto& [agent_id, record] : agents_) {
        state["registry_epochs"][agent_id] =
            record.manifest.agent_epoch;
    }
    state["registry_manifests"] = nlohmann::json::array();
    std::map<std::string, sub_agents::AgentManifest> manifests =
        recovered_manifests_;
    for (const auto& [agent_id, record] : agents_) {
        manifests[agent_id] = record.manifest;
    }
    for (const auto& [agent_id, manifest] : manifests) {
        (void)agent_id;
        state["registry_manifests"].push_back(
            manifestJson(manifest));
    }
    state["dispatches"] = nlohmann::json::array();
    for (const auto& [dispatch_id, snapshot] : dispatches_) {
        (void)dispatch_id;
        state["dispatches"].push_back(snapshotJson(snapshot));
    }
    state["events"] = nlohmann::json::array();
    for (const auto& event : events_) {
        state["events"].push_back(eventJson(event));
    }
    state["event_acks"] = nlohmann::json::object();
    for (const auto& [consumer, ack] : event_acks_) {
        state["event_acks"][consumer] = {ack.first, ack.second};
    }
    state["checksum"] = secureDigest(state.dump());
    const auto path = storage_directory_ / "dispatch_state.json";
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(
            temporary, std::ios::binary | std::ios::trunc);
        output << state.dump();
        output.flush();
        if (!output) {
            return Status::Error(
                "agent_dispatch", "DISPATCH_DURABILITY_FAILED",
                "dispatch state could not be sealed", true,
                SideEffectState::Unknown);
        }
    }
    std::error_code error;
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    return error ? Status::Error(
                       "agent_dispatch", "DISPATCH_DURABILITY_FAILED",
                       "dispatch state could not be published", true,
                       SideEffectState::Unknown)
                 : Status::Ok();
}

Status AgentDispatch::recoverStateUnlocked() {
    const auto path = storage_directory_ / "dispatch_state.json";
    if (!std::filesystem::exists(path)) return persistStateUnlocked();
    try {
        std::ifstream input(path, std::ios::binary);
        nlohmann::json state;
        input >> state;
        const auto checksum = state.at("checksum").get<std::string>();
        state.erase("checksum");
        if (state.at("schema_version").get<int>() != 1 ||
            checksum != secureDigest(state.dump())) {
            throw std::runtime_error("dispatch checksum");
        }
        capacity_epoch_ =
            state.at("capacity_epoch").get<std::uint64_t>();
        enqueue_sequence_ =
            state.at("enqueue_sequence").get<std::uint64_t>();
        control_sequence_ =
            state.at("control_sequence").get<std::uint64_t>();
        accepting_ = state.at("accepting").get<bool>();
        recovered_agent_epochs_ =
            state.at("registry_epochs")
                .get<std::map<std::string, std::uint64_t>>();
        recovered_manifests_.clear();
        if (const auto manifests = state.find("registry_manifests");
            manifests != state.end()) {
            if (!manifests->is_array()) {
                throw std::runtime_error("dispatch manifest registry");
            }
            for (const auto& encoded : *manifests) {
                auto manifest = manifestFromJson(encoded);
                const auto epoch = recovered_agent_epochs_.find(
                    manifest.agent_id);
                if (epoch == recovered_agent_epochs_.end() ||
                    epoch->second != manifest.agent_epoch ||
                    !recovered_manifests_
                         .emplace(manifest.agent_id, manifest)
                         .second) {
                    throw std::runtime_error(
                        "dispatch manifest epoch binding");
                }
            }
        }
        dispatches_.clear();
        operation_to_dispatch_.clear();
        execution_to_dispatch_.clear();
        idempotency_to_dispatch_.clear();
        idempotency_digest_.clear();
        for (const auto& encoded : state.at("dispatches")) {
            auto snapshot = snapshotFromJson(encoded);
            if (snapshot.state != DispatchState::Succeeded &&
                snapshot.state != DispatchState::Failed &&
                snapshot.state != DispatchState::Cancelled) {
                snapshot.state = DispatchState::Unknown;
                snapshot.side_effect_state = SideEffectState::Unknown;
                snapshot.error_code = "DISPATCH_RECOVERY_RECONCILE_REQUIRED";
            }
            const auto id = snapshot.dispatch_id;
            operation_to_dispatch_[snapshot.task.operation_id] = id;
            execution_to_dispatch_[snapshot.task.execution_id] = id;
            const auto ledger_key = scopedIdempotencyLedgerKey(
                snapshot.task.principal_id_hash,
                snapshot.task.idempotency_key);
            idempotency_to_dispatch_[ledger_key] = id;
            idempotency_digest_[ledger_key] =
                taskDigest(snapshot.task);
            dispatches_[id] = std::move(snapshot);
        }
        events_.clear();
        for (const auto& encoded : state.at("events")) {
            events_.push_back(eventFromJson(encoded));
        }
        event_acks_.clear();
        for (auto item = state.at("event_acks").begin();
             item != state.at("event_acks").end(); ++item) {
            event_acks_[item.key()] = {
                item.value().at(0).get<std::uint64_t>(),
                item.value().at(1).get<std::uint64_t>()};
        }
        recordCapacityUnlocked();
    } catch (...) {
        return Status::Error(
            "agent_dispatch", "DISPATCH_RECOVERY_INTEGRITY_FAILED",
            "persisted dispatch state failed integrity validation", false,
            SideEffectState::Unknown);
    }
    return Status::Ok();
}

}  // namespace master_agent::agent_dispatch
