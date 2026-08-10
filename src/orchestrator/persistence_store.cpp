/**
 * @file persistence_store.cpp
 * @brief Implements durable plan persistence and recovery.
 */

#include "include/orchestrator_access_identity.h"
#include "include/dag_runtime_rules.h"
#include "include/orchestrator_wal_codec.h"
#include "include/orchestrator_durability.h"
#include "include/plan_priority.h"

namespace master_agent::orchestrator {
namespace {

nlohmann::json catalogJson(
    const atomic_service::McpToolCatalogSnapshot& catalog) {
    return {
        {"snapshot_id", catalog.snapshot_id},
        {"mcp_protocol_version", catalog.mcp_protocol_version},
        {"catalog_generation", catalog.catalog_generation},
        {"catalog_digest", catalog.catalog_digest},
        {"created_at_utc_ms", catalog.created_at_utc_ms},
        {"tool_digests", catalog.tool_digests},
        {"policy_digests", catalog.policy_digests},
        {"idempotency_policies", catalog.idempotency_policies},
        {"retryable_errors", catalog.retryable_errors},
        {"provider_ids", catalog.provider_ids},
        {"provider_epochs", catalog.provider_epochs}};
}

atomic_service::McpToolCatalogSnapshot catalogFromJson(
    const nlohmann::json& encoded) {
    atomic_service::McpToolCatalogSnapshot catalog;
    catalog.snapshot_id = encoded.at("snapshot_id").get<std::string>();
    catalog.mcp_protocol_version =
        encoded.at("mcp_protocol_version").get<std::string>();
    catalog.catalog_generation =
        encoded.at("catalog_generation").get<std::uint64_t>();
    catalog.catalog_digest =
        encoded.at("catalog_digest").get<std::string>();
    catalog.created_at_utc_ms =
        encoded.at("created_at_utc_ms").get<std::int64_t>();
    catalog.tool_digests =
        encoded.at("tool_digests")
            .get<std::map<std::string, std::string>>();
    catalog.policy_digests =
        encoded.at("policy_digests")
            .get<std::map<std::string, std::string>>();
    catalog.idempotency_policies =
        encoded.at("idempotency_policies")
            .get<std::map<std::string, std::string>>();
    catalog.retryable_errors =
        encoded.at("retryable_errors")
            .get<std::map<std::string,
                          std::vector<std::string>>>();
    catalog.provider_ids =
        encoded.at("provider_ids")
            .get<std::map<std::string, std::string>>();
    catalog.provider_epochs =
        encoded.at("provider_epochs")
            .get<std::map<std::string, std::uint64_t>>();
    if (catalog.snapshot_id.empty() ||
        catalog.catalog_digest.empty() ||
        catalog.tool_digests.empty() ||
        catalog.policy_digests.empty()) {
        throw std::runtime_error("invalid recovered capability catalog");
    }
    return catalog;
}

}  // namespace

Orchestrator::Orchestrator(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::shared_ptr<atomic_service::IAtomicServiceManager> atomic,
    std::shared_ptr<agent_dispatch::IAgentDispatch> dispatch,
    std::filesystem::path storage_directory)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      atomic_(std::move(atomic)),
      dispatch_(std::move(dispatch)),
      storage_directory_(std::move(storage_directory)) {
    durability_status_ = Status::Ok();
    if (!storage_directory_.empty()) {
        durable_wal_path_ =
            storage_directory_ / "orchestrator_plans.wal";
        durability_status_ = recoverDurableState();
    }
}

Status Orchestrator::persistPlanUnlocked(
    const PlanRuntime& plan) {
    if (storage_directory_.empty()) return Status::Ok();
    if (!durability_status_.ok) return durability_status_;
    try {
        nlohmann::json edges = nlohmann::json::array();
        for (const auto& edge : plan.edges) {
            edges.push_back(dagEdgeJson(edge));
        }
        nlohmann::json event_history =
            nlohmann::json::array();
        for (const auto& event : plan.event_history) {
            event_history.push_back(taskEventJson(event));
        }
        const nlohmann::json payload{
            {"schema_version", kOrchestratorWalSchemaVersion},
            {"snapshot", planSnapshotJson(plan.snapshot)},
            {"admission", admissionJson(plan.admission)},
            {"capability_catalog", catalogJson(plan.catalog)},
            {"submit_digest", plan.submit_digest},
            {"ledger_key", plan.ledger_key},
            {"plan_ready_sequence", plan.ready_sequence},
            {"node_ready_sequence",
             plan.node_ready_sequence},
            {"edges", edges},
            {"event_history", event_history},
            {"seen_signal_events", seen_signal_events_},
            {"signal_cursors", signal_cursors_},
            {"event_acks", event_acks_},
            {"global_ready_sequence", ready_sequence_},
            {"global_fencing_sequence", fencing_sequence_}};
        const auto next_sequence = durable_sequence_ + 1;
        const auto checksum = secureDigest(
            "orchestrator-wal-v1|" + durable_chain_head_ + "|" +
            std::to_string(next_sequence) + "|" + payload.dump());
        const nlohmann::json frame{
            {"kind", "orchestrator_plan"},
            {"schema_version", kOrchestratorWalSchemaVersion},
            {"sequence", next_sequence},
            {"previous_checksum", durable_chain_head_},
            {"payload", payload},
            {"checksum", checksum}};
        std::ofstream output(
            durable_wal_path_,
            std::ios::binary | std::ios::out | std::ios::app);
        if (!output.is_open()) {
            durability_status_ = Status::Error(
                "orchestrator",
                "ORCHESTRATOR_DURABILITY_UNAVAILABLE",
                "cannot open Plan WAL", true,
                SideEffectState::Unknown);
            return durability_status_;
        }
        output << frame.dump() << '\n';
        output.flush();
        if (!output.good()) {
            durability_status_ = Status::Error(
                "orchestrator",
                "ORCHESTRATOR_DURABILITY_UNAVAILABLE",
                "Plan WAL write is ambiguous", true,
                SideEffectState::Unknown);
            return durability_status_;
        }
        output.close();
        const auto synced =
            nativeOrchestratorDurabilitySync(durable_wal_path_);
        if (!synced.ok) {
            durability_status_ = synced;
            return durability_status_;
        }
        durable_sequence_ = next_sequence;
        durable_chain_head_ = checksum;
        return Status::Ok();
    } catch (...) {
        durability_status_ = Status::Error(
            "orchestrator",
            "ORCHESTRATOR_DURABILITY_UNAVAILABLE",
            "Plan WAL encoding failed", true,
            SideEffectState::Unknown);
        return durability_status_;
    }
}

Status Orchestrator::recoverDurableState() {

    recovered_plan_records_.clear();
    durable_sequence_ = 0;
    durable_chain_head_ = kOrchestratorWalGenesis;
    try {
        std::filesystem::create_directories(storage_directory_);
        if (!std::filesystem::exists(durable_wal_path_)) {
            std::ofstream create(
                durable_wal_path_,
                std::ios::binary | std::ios::out);
            if (!create.is_open()) {
                return Status::Error(
                    "orchestrator",
                    "ORCHESTRATOR_DURABILITY_UNAVAILABLE",
                    "cannot create Plan WAL", true,
                    SideEffectState::Unknown);
            }
            create.close();
            return nativeOrchestratorDurabilitySync(
                durable_wal_path_);
        }
        if (std::filesystem::file_size(durable_wal_path_) >
            256ULL * 1024ULL * 1024ULL) {
            return Status::Error(
                "orchestrator", "ORCHESTRATOR_WAL_CORRUPT",
                "Plan WAL exceeds recovery bound", false,
                SideEffectState::Unknown);
        }
        std::ifstream input(durable_wal_path_, std::ios::binary);
        if (!input.is_open()) {
            return Status::Error(
                "orchestrator",
                "ORCHESTRATOR_DURABILITY_UNAVAILABLE",
                "cannot read Plan WAL", true,
                SideEffectState::Unknown);
        }
        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        std::size_t cursor = 0;
        std::uint64_t expected_sequence = 1;
        std::string expected_previous =
            kOrchestratorWalGenesis;
        while (cursor < bytes.size()) {
            const auto newline = bytes.find('\n', cursor);
            if (newline == std::string::npos) {
                std::filesystem::resize_file(
                    durable_wal_path_, cursor);
                const auto synced =
                    nativeOrchestratorDurabilitySync(
                        durable_wal_path_);
                if (!synced.ok) return synced;
                break;
            }
            const auto line =
                bytes.substr(cursor, newline - cursor);
            if (line.empty() ||
                line.size() > 16U * 1024U * 1024U) {
                return Status::Error(
                    "orchestrator",
                    "ORCHESTRATOR_WAL_CORRUPT",
                    "Plan WAL frame is invalid", false,
                    SideEffectState::Unknown);
            }
            try {
                const auto frame =
                    nlohmann::json::parse(line);
                if (frame.at("kind").get<std::string>() !=
                        "orchestrator_plan" ||
                    frame.at("schema_version")
                            .get<std::uint32_t>() !=
                        kOrchestratorWalSchemaVersion ||
                    frame.at("sequence")
                            .get<std::uint64_t>() !=
                        expected_sequence ||
                    frame.at("previous_checksum")
                            .get<std::string>() !=
                        expected_previous) {
                    throw std::runtime_error(
                        "Plan WAL identity");
                }
                const auto& payload = frame.at("payload");
                if (payload.at("schema_version")
                        .get<std::uint32_t>() !=
                    kOrchestratorWalSchemaVersion) {
                    throw std::runtime_error(
                        "Plan WAL schema");
                }
                const auto checksum =
                    frame.at("checksum").get<std::string>();
                if (checksum != secureDigest(
                        "orchestrator-wal-v1|" +
                        expected_previous + "|" +
                        std::to_string(expected_sequence) + "|" +
                        payload.dump())) {
                    throw std::runtime_error(
                        "Plan WAL checksum");
                }
                DurablePlanRecord record;
                record.runtime.snapshot =
                    planSnapshotFromJson(
                        payload.at("snapshot"));
                record.runtime.admission =
                    admissionFromJson(
                        payload.at("admission"));
                record.runtime.catalog =
                    catalogFromJson(
                        payload.at("capability_catalog"));
                record.runtime.submit_digest =
                    payload.at("submit_digest")
                        .get<std::string>();
                record.runtime.ledger_key =
                    payload.at("ledger_key")
                        .get<std::string>();
                record.runtime.ready_sequence =
                    payload.at("plan_ready_sequence")
                        .get<std::uint64_t>();
                record.runtime.node_ready_sequence =
                    payload.at("node_ready_sequence")
                        .get<std::map<std::string,
                                      std::uint64_t>>();
                if (const auto edges =
                        payload.find("edges");
                    edges != payload.end()) {
                    for (const auto& edge : *edges) {
                        record.runtime.edges.push_back(
                            dagEdgeFromJson(edge));
                    }
                }
                if (const auto history =
                        payload.find("event_history");
                    history != payload.end()) {
                    for (const auto& event : *history) {
                        record.runtime.event_history.push_back(
                            taskEventFromJson(event));
                    }
                }
                if (const auto seen =
                        payload.find("seen_signal_events");
                    seen != payload.end()) {
                    const auto recovered =
                        seen->get<std::set<std::string>>();
                    seen_signal_events_.insert(
                        recovered.begin(), recovered.end());
                }
                if (const auto cursors =
                        payload.find("signal_cursors");
                    cursors != payload.end()) {
                    for (const auto& item :
                         cursors->items()) {
                        const auto value =
                            item.value().get<
                                std::pair<std::uint64_t,
                                          std::uint64_t>>();
                        auto& existing =
                            signal_cursors_[item.key()];
                        if (value > existing) {
                            existing = value;
                        }
                    }
                }
                if (const auto acknowledgements =
                        payload.find("event_acks");
                    acknowledgements != payload.end()) {
                    for (const auto& item :
                         acknowledgements->items()) {
                        const auto value = item.value().get<
                            std::pair<std::uint64_t,
                                      std::uint64_t>>();
                        auto& existing = event_acks_[item.key()];
                        if (value > existing) {
                            existing = value;
                        }
                    }
                }
                record.ready_sequence =
                    payload.at("global_ready_sequence")
                        .get<std::uint64_t>();
                record.fencing_sequence =
                    payload.at("global_fencing_sequence")
                        .get<std::uint64_t>();
                if (record.runtime.submit_digest.empty() ||
                    record.runtime.ledger_key.empty() ||
                    record.fencing_sequence == 0) {
                    throw std::runtime_error(
                        "Plan WAL binding");
                }
                recovered_plan_records_[
                    record.runtime.snapshot.plan_id] =
                    std::move(record);
                durable_sequence_ = expected_sequence;
                durable_chain_head_ = checksum;
                expected_previous = checksum;
                ++expected_sequence;
            } catch (...) {
                return Status::Error(
                    "orchestrator",
                    "ORCHESTRATOR_WAL_CORRUPT",
                    "Plan WAL integrity check failed", false,
                    SideEffectState::Unknown);
            }
            cursor = newline + 1;
        }

        CallContext catalog_call{
            CallerModuleId::TaskOrchestrationEngine,
            "orchestrator-recovery", "orchestrator-recovery-trace",
            "system", TaskPriority::P1,
            clock_->monotonicNowNs() + 5'000'000'000LL};
        const auto current_catalog_result =
            atomic_->getToolCatalogSnapshot(catalog_call);
        if (!current_catalog_result.status.ok ||
            !current_catalog_result.value) {
            return Status::Error(
                "orchestrator",
                "ORCHESTRATOR_CAPABILITY_RECOVERY_UNAVAILABLE",
                "current capability catalog is unavailable during recovery",
                true);
        }
        const auto& current_catalog =
            *current_catalog_result.value;
        std::set<std::string> ledgers;
        std::vector<std::string> normalized;
        for (auto& [plan_id, durable] :
             recovered_plan_records_) {
            auto& runtime = durable.runtime;
            if (plan_id != runtime.snapshot.plan_id ||
                !ledgers.insert(runtime.ledger_key).second) {
                return Status::Error(
                    "orchestrator",
                    "ORCHESTRATOR_WAL_CORRUPT",
                    "Plan WAL contains conflicting indexes", false,
                    SideEffectState::Unknown);
            }
            if (!planTerminal(runtime.snapshot.state)) {
                bool capability_compatible = true;
                for (const auto& [node_id, node] :
                     runtime.snapshot.nodes) {
                    (void)node_id;
                    if (node.definition.executor != "atomic_service" ||
                        nodeTerminal(node.state)) {
                        continue;
                    }
                    const auto old_tool = runtime.catalog.tool_digests.find(
                        node.definition.action);
                    const auto new_tool = current_catalog.tool_digests.find(
                        node.definition.action);
                    const auto old_policy =
                        runtime.catalog.policy_digests.find(
                            node.definition.action);
                    const auto new_policy =
                        current_catalog.policy_digests.find(
                            node.definition.action);
                    if (old_tool == runtime.catalog.tool_digests.end() ||
                        new_tool == current_catalog.tool_digests.end() ||
                        old_policy == runtime.catalog.policy_digests.end() ||
                        new_policy == current_catalog.policy_digests.end() ||
                        old_tool->second != new_tool->second ||
                        old_policy->second != new_policy->second) {
                        capability_compatible = false;
                        break;
                    }
                }
                if (!capability_compatible) {
                    runtime.snapshot.state = PlanState::Suspended;
                    for (auto& [node_id, node] :
                         runtime.snapshot.nodes) {
                        (void)node_id;
                        if (!nodeTerminal(node.state)) {
                            node.blockers.push_back(
                                "CAPABILITY_VERSION_INCOMPATIBLE");
                            node.error_code =
                                "ORCHESTRATOR_CAPABILITY_VERSION_INCOMPATIBLE";
                        }
                    }
                    ++runtime.snapshot.version;
                    normalized.push_back(plan_id);
                } else {
                    // Unexecuted nodes were checked against the current Tool
                    // and policy digests. Bind their future Dispatch envelope
                    // to the new snapshot identity while preserving old
                    // activation history under the original snapshot.
                    runtime.catalog = current_catalog;
                    runtime.snapshot.capability_snapshot_id =
                        current_catalog.snapshot_id;
                }
                bool requires_reconciliation = false;
                for (auto& [node_id, node] :
                     runtime.snapshot.nodes) {
                    (void)node_id;
                    if (!nodeTerminal(node.state)) {
                        const bool dispatched =
                            !node.execution_id.empty() ||
                            !node.operation_id.empty() ||
                            node.fencing_token != 0;
                        if (dispatched ||
                            node.state == ActivationState::Queued ||
                            node.state == ActivationState::Running ||
                            node.state ==
                                ActivationState::Reconciling) {
                            node.state = ActivationState::Unknown;
                            node.result = nlohmann::json::object();
                            node.side_effect_state =
                                SideEffectState::Unknown;
                            node.error_code =
                                "ORCHESTRATOR_RECOVERED_ATTEMPT_REQUIRES_RECONCILIATION";
                            node.retryable_hint = false;
                            node.retry_at_mono_ns = 0;
                            requires_reconciliation = true;
                        }
                        // BLOCKED timer/signal activations and READY work have
                        // no accepted external side effect. Retaining them is
                        // required to restore timer registrations and rebuild
                        // the ReadyQueue instead of manufacturing UNKNOWN.
                    }
                }
                if (capability_compatible) {
                    runtime.snapshot.state =
                        requires_reconciliation ? PlanState::Unknown
                                                : PlanState::Running;
                }
                if (requires_reconciliation) {
                    ++runtime.snapshot.version;
                    normalized.push_back(plan_id);
                }
            }
            ready_sequence_ =
                std::max(ready_sequence_, durable.ready_sequence);
            for (const auto& [node_id, node] :
                 runtime.snapshot.nodes) {
                if (node.state == ActivationState::Ready &&
                    runtime.node_ready_sequence.count(node_id) == 0) {
                    runtime.node_ready_sequence[node_id] =
                        ++ready_sequence_;
                }
            }
            plans_[plan_id] = runtime;
            for (const auto& event :
                 runtime.event_history) {
                const auto duplicate =
                    std::find_if(
                        events_.begin(), events_.end(),
                        [&event](const TaskEvent& candidate) {
                            return candidate.event_id ==
                                   event.event_id;
                        });
                if (duplicate == events_.end()) {
                    events_.push_back(event);
                }
            }
            idempotency_to_plan_[runtime.ledger_key] = plan_id;
            idempotency_digest_[runtime.ledger_key] =
                runtime.submit_digest;
            fencing_sequence_ =
                std::max(fencing_sequence_,
                         durable.fencing_sequence);
        }
        for (const auto& plan_id : normalized) {
            const auto persisted =
                persistPlanUnlocked(plans_.at(plan_id));
            if (!persisted.ok) return persisted;
        }
        recovered_plan_records_.clear();
        return Status::Ok();
    } catch (...) {
        return Status::Error(
            "orchestrator",
            "ORCHESTRATOR_DURABILITY_UNAVAILABLE",
            "Plan WAL recovery failed", true,
            SideEffectState::Unknown);
    }
}


}  // namespace master_agent::orchestrator
