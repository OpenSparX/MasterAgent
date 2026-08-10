/**
 * @file persistence_store.cpp
 * @brief Implements durable atomic-call persistence and recovery.
 */

#include "include/atomic_access_policy.h"
#include "include/mcp_schema_validation.h"
#include "include/atomic_wal_codec.h"
#include "include/atomic_durability.h"
#include "include/atomic_state_rules.h"

namespace master_agent::atomic_service {

AtomicServiceManager::AtomicServiceManager(
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    std::size_t max_inflight,
    std::shared_ptr<IAtomicParentLineageValidator>
        lineage_validator,
    std::filesystem::path storage_directory)
    : clock_(std::move(clock)),
      ids_(std::move(ids)),
      lineage_validator_(std::move(lineage_validator)),
      max_inflight_(std::max<std::size_t>(1, max_inflight)),
      storage_directory_(std::move(storage_directory)) {
    durability_status_ = Status::Ok();
    if (!storage_directory_.empty()) {
        durable_wal_path_ =
            storage_directory_ / "atomic_execution.wal";
        durability_status_ = recoverDurableState();
    }
}

Status AtomicServiceManager::persistExecutionUnlocked(
    const AtomicExecutionSnapshot& snapshot,
    const std::string& ledger_key,
    const std::string& request_digest,
    std::uint64_t queue_sequence) {
    if (storage_directory_.empty()) return Status::Ok();
    if (!durability_status_.ok) return durability_status_;
    try {
        const nlohmann::json payload{
            {"schema_version", kAtomicWalSchemaVersion},
            {"ledger_key", ledger_key},
            {"request_digest", request_digest},
            {"queue_sequence", queue_sequence},
            {"snapshot", atomicSnapshotJson(snapshot)}};
        const auto next_sequence = durable_sequence_ + 1;
        const auto checksum = secureDigest(
            "atomic-wal-v1|" + durable_chain_head_ + "|" +
            std::to_string(next_sequence) + "|" + payload.dump());
        const nlohmann::json frame{
            {"kind", "atomic_execution"},
            {"schema_version", kAtomicWalSchemaVersion},
            {"sequence", next_sequence},
            {"previous_checksum", durable_chain_head_},
            {"payload", payload},
            {"checksum", checksum}};
        std::ofstream output(
            durable_wal_path_,
            std::ios::binary | std::ios::out | std::ios::app);
        if (!output.is_open()) {
            durability_status_ = Status::Error(
                "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
                "cannot open atomic execution WAL", true,
                SideEffectState::Unknown);
            return durability_status_;
        }
        output << frame.dump() << '\n';
        output.flush();
        if (!output.good()) {
            durability_status_ = Status::Error(
                "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
                "atomic execution WAL write is ambiguous", true,
                SideEffectState::Unknown);
            return durability_status_;
        }
        output.close();
        const auto synced =
            nativeAtomicDurabilitySync(durable_wal_path_);
        if (!synced.ok) {
            durability_status_ = synced;
            return durability_status_;
        }
        durable_sequence_ = next_sequence;
        durable_chain_head_ = checksum;
        return Status::Ok();
    } catch (...) {
        durability_status_ = Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
            "atomic execution WAL encoding failed", true,
            SideEffectState::Unknown);
        return durability_status_;
    }
}

Status AtomicServiceManager::persistCurrentExecutionUnlocked(
    const std::string& execution_id) {
    const auto found = executions_.find(execution_id);
    const auto sequence = queue_sequence_.find(execution_id);
    if (found == executions_.end() ||
        sequence == queue_sequence_.end()) {
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_STATE_INVALID",
            "atomic execution indexes are incomplete", false,
            SideEffectState::Unknown);
    }
    const auto& runtime = found->second.envelope.runtime;
    std::string digest;
    try {
        digest = callDigest(found->second.envelope);
    } catch (...) {
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_STATE_INVALID",
            "atomic execution cannot be canonically encoded", false,
            SideEffectState::Unknown);
    }
    const auto ledger_key = scopedIdempotencyLedgerKey(
        runtime.principal_id_hash, runtime.idempotency_key);
    return persistExecutionUnlocked(
        found->second, ledger_key, digest, sequence->second);
}

Status AtomicServiceManager::recoverDurableState() {

    recovered_execution_records_.clear();
    durable_sequence_ = 0;
    durable_chain_head_ = kAtomicWalGenesis;
    try {
        std::filesystem::create_directories(storage_directory_);
        if (!std::filesystem::exists(durable_wal_path_)) {
            std::ofstream create(
                durable_wal_path_,
                std::ios::binary | std::ios::out);
            if (!create.is_open()) {
                return Status::Error(
                    "atomic_service",
                    "ATOMIC_DURABILITY_UNAVAILABLE",
                    "cannot create atomic execution WAL", true,
                    SideEffectState::Unknown);
            }
            create.close();
            return nativeAtomicDurabilitySync(durable_wal_path_);
        }
        const auto bytes_on_disk =
            std::filesystem::file_size(durable_wal_path_);
        if (bytes_on_disk > 256ULL * 1024ULL * 1024ULL) {
            return Status::Error(
                "atomic_service", "ATOMIC_WAL_CORRUPT",
                "atomic execution WAL exceeds recovery bound", false,
                SideEffectState::Unknown);
        }
        std::ifstream input(durable_wal_path_, std::ios::binary);
        if (!input.is_open()) {
            return Status::Error(
                "atomic_service",
                "ATOMIC_DURABILITY_UNAVAILABLE",
                "cannot read atomic execution WAL", true,
                SideEffectState::Unknown);
        }
        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        std::size_t cursor = 0;
        std::uint64_t expected_sequence = 1;
        std::string expected_previous = kAtomicWalGenesis;
        while (cursor < bytes.size()) {
            const auto newline = bytes.find('\n', cursor);
            if (newline == std::string::npos) {
                std::filesystem::resize_file(
                    durable_wal_path_, cursor);
                const auto synced =
                    nativeAtomicDurabilitySync(durable_wal_path_);
                if (!synced.ok) return synced;
                break;
            }
            const auto line = bytes.substr(cursor, newline - cursor);
            if (line.empty() || line.size() > 16U * 1024U * 1024U) {
                return Status::Error(
                    "atomic_service", "ATOMIC_WAL_CORRUPT",
                    "atomic execution WAL frame is invalid", false,
                    SideEffectState::Unknown);
            }
            try {
                const auto frame = nlohmann::json::parse(line);
                if (frame.at("kind").get<std::string>() !=
                        "atomic_execution" ||
                    frame.at("schema_version").get<std::uint32_t>() !=
                        kAtomicWalSchemaVersion ||
                    frame.at("sequence").get<std::uint64_t>() !=
                        expected_sequence ||
                    frame.at("previous_checksum").get<std::string>() !=
                        expected_previous) {
                    throw std::runtime_error("atomic WAL identity");
                }
                const auto& payload = frame.at("payload");
                if (payload.at("schema_version")
                        .get<std::uint32_t>() !=
                    kAtomicWalSchemaVersion) {
                    throw std::runtime_error("atomic WAL schema");
                }
                const auto checksum =
                    frame.at("checksum").get<std::string>();
                const auto expected_checksum = secureDigest(
                    "atomic-wal-v1|" + expected_previous + "|" +
                    std::to_string(expected_sequence) + "|" +
                    payload.dump());
                if (checksum != expected_checksum) {
                    throw std::runtime_error("atomic WAL checksum");
                }
                DurableExecutionRecord record;
                record.snapshot =
                    atomicSnapshotFromJson(payload.at("snapshot"));
                record.ledger_key =
                    payload.at("ledger_key").get<std::string>();
                record.request_digest =
                    payload.at("request_digest").get<std::string>();
                record.queue_sequence =
                    payload.at("queue_sequence")
                        .get<std::uint64_t>();
                const auto& runtime =
                    record.snapshot.envelope.runtime;
                if (record.ledger_key.empty() ||
                    record.request_digest.empty() ||
                    record.queue_sequence == 0 ||
                    record.request_digest !=
                        callDigest(record.snapshot.envelope) ||
                    record.ledger_key !=
                        scopedIdempotencyLedgerKey(
                            runtime.principal_id_hash,
                            runtime.idempotency_key) ||
                    (record.snapshot.provider_invocation &&
                     record.snapshot.provider_invocation
                             ->request_digest !=
                         record.request_digest)) {
                    throw std::runtime_error(
                        "atomic WAL execution binding");
                }
                recovered_execution_records_[
                    runtime.execution_id] = std::move(record);
                durable_sequence_ = expected_sequence;
                durable_chain_head_ = checksum;
                expected_previous = checksum;
                ++expected_sequence;
            } catch (...) {
                // A newline-terminated invalid frame is tampering or
                // mid-log corruption, never an ignorable torn tail.
                return Status::Error(
                    "atomic_service", "ATOMIC_WAL_CORRUPT",
                    "atomic execution WAL integrity check failed",
                    false, SideEffectState::Unknown);
            }
            cursor = newline + 1;
        }
        return Status::Ok();
    } catch (...) {
        return Status::Error(
            "atomic_service", "ATOMIC_DURABILITY_UNAVAILABLE",
            "atomic execution WAL recovery failed", true,
            SideEffectState::Unknown);
    }
}

Status
AtomicServiceManager::activateRecoveredExecutionsUnlocked(
    const std::map<std::string, ToolRecord>& registered_tools) {
    if (recovered_execution_records_.empty()) return Status::Ok();

    std::map<std::string, AtomicExecutionSnapshot> executions;
    std::map<std::string, ToolRecord> execution_tools;
    std::map<std::string, std::string> operations;
    std::map<std::string, std::string> idempotency;
    std::map<std::string, std::string> digests;
    std::map<std::string, ResourceFenceOwnership> fences;
    std::map<std::string, std::uint64_t> queue_sequences;
    std::uint64_t max_queue_sequence = 0;
    std::vector<std::string> normalized;

    for (const auto& [execution_id, durable] :
         recovered_execution_records_) {
        auto snapshot = durable.snapshot;
        const auto& runtime = snapshot.envelope.runtime;
        const auto tool = registered_tools.find(
            snapshot.envelope.mcp_request.name);
        if (execution_id != runtime.execution_id ||
            tool == registered_tools.end() ||
            runtime.tool_digest != tool->second.policy.tool_digest ||
            runtime.policy_digest != tool->second.policy.policy_digest ||
            snapshot.resource_key !=
                resourceKey(tool->second.policy,
                            snapshot.envelope.mcp_request.arguments) ||
            operations.count(runtime.operation_id) != 0 ||
            idempotency.count(durable.ledger_key) != 0) {
            return Status::Error(
                "atomic_service",
                "ATOMIC_RECOVERY_CATALOG_MISMATCH",
                "recovered execution does not match the registered Tool "
                "contract or unique indexes",
                false, SideEffectState::Unknown);
        }
        const bool settled =
            snapshot.state == AtomicExecutionState::Succeeded ||
            snapshot.state == AtomicExecutionState::Failed ||
            snapshot.state == AtomicExecutionState::Cancelled;
        if (!settled) {
            snapshot.result.reset();
            snapshot.completion_evidence = CompletionEvidence::None;
            snapshot.retryable_hint = false;
            if (snapshot.provider_invocation) {
                snapshot.state = AtomicExecutionState::Unknown;
                snapshot.side_effect_state = SideEffectState::Unknown;
                snapshot.error_code =
                    "ATOMIC_RECOVERED_INFLIGHT_REQUIRES_RECONCILE";
            } else {
                snapshot.state = AtomicExecutionState::Failed;
                snapshot.side_effect_state =
                    SideEffectState::ConfirmedNotExecuted;
                snapshot.error_code =
                    "ATOMIC_RECOVERED_BEFORE_PROVIDER_NOT_EXECUTED";
            }
            normalized.push_back(execution_id);
        }
        executions.emplace(execution_id, snapshot);
        execution_tools.emplace(execution_id, tool->second);
        operations.emplace(runtime.operation_id, execution_id);
        idempotency.emplace(durable.ledger_key, execution_id);
        digests.emplace(
            durable.ledger_key, durable.request_digest);
        queue_sequences.emplace(
            execution_id, durable.queue_sequence);
        max_queue_sequence =
            std::max(max_queue_sequence, durable.queue_sequence);

        ResourceFenceOwnership owner{
            runtime.fencing_token,
            runtime.caller_module_id == CallerModuleId::SubAgent
                ? runtime.parent_dispatch_id
                : std::string{},
            runtime.caller_module_id == CallerModuleId::SubAgent
                ? runtime.parent_lease_id
                : std::string{}};
        const auto current = fences.find(snapshot.resource_key);
        if (current == fences.end() ||
            owner.fencing_token >
                current->second.fencing_token) {
            fences[snapshot.resource_key] = std::move(owner);
        }
    }

    executions_ = std::move(executions);
    execution_tools_ = std::move(execution_tools);
    operation_to_execution_ = std::move(operations);
    idempotency_to_execution_ = std::move(idempotency);
    idempotency_digest_ = std::move(digests);
    highest_fencing_by_resource_ = std::move(fences);
    queue_sequence_ = std::move(queue_sequences);
    enqueue_sequence_ =
        std::max(enqueue_sequence_, max_queue_sequence);
    for (const auto& execution_id : normalized) {
        const auto persisted =
            persistCurrentExecutionUnlocked(execution_id);
        if (!persisted.ok) return persisted;
    }
    recovered_execution_records_.clear();
    return Status::Ok();
}


}  // namespace master_agent::atomic_service
