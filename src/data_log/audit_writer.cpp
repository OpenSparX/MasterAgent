/**
 * @file audit_writer.cpp
 * @brief Validates and durably appends tamper-evident audit batches.
 */

#include "include/batch_validation.h"
#include "include/journal_integrity.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_io.h"
#include "include/record_serialization.h"
#include "include/trace_filter.h"

namespace master_agent::data_log {

Result<AuditAppendResult> DataLogService::appendAudit(
    const AuditBatch& batch, const CallContext& call) {

    std::lock_guard<std::mutex> writer_lock(audit_writer_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "LOG_NOT_READY", "log service is not ready", true));
    }
    const bool exception_lifecycle_projection =
        call.caller == CallerModuleId::ExceptionManager;
    if (call.caller != CallerModuleId::AgentService &&
        !exception_lifecycle_projection) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "AUDIT_CALLER_NOT_ALLOWED",
            "audit writes require AgentService or the scoped "
            "ExceptionManager lifecycle projection"));
    }
    if (call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "AUDIT_CALL_EXPIRED",
            "audit call deadline expired"));
    }
    if (!producerAuthenticated(
            call, batch.producer_endpoint_id,
            batch.producer_epoch)) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "AUDIT_PRODUCER_IDENTITY_MISMATCH",
            "Audit producer is not bound to the authenticated caller"));
    }
    const auto bounds = validateAuditBatchBounds(batch);
    if (!bounds.ok) {
        return Result<AuditAppendResult>::Failure(bounds);
    }
    const auto batch_digest = auditBatchDigest(batch);
    const auto replay = audit_batch_results_.find(batch.batch_id);
    if (replay != audit_batch_results_.end()) {
        if (audit_batch_digests_.at(batch.batch_id) !=
            batch_digest) {
            return Result<AuditAppendResult>::Failure(Status::Error(
                "data_log", "AUDIT_BATCH_IDEMPOTENCY_CONFLICT",
                "audit batch_id was reused with different content"));
        }
        auto duplicate = replay->second;
        duplicate.disposition = AppendDisposition::Duplicate;
        return Result<AuditAppendResult>::Success(std::move(duplicate));
    }
    const auto failed_replay =
        audit_batch_failures_.find(batch.batch_id);
    if (failed_replay != audit_batch_failures_.end()) {
        if (audit_failure_digests_.at(batch.batch_id) != batch_digest) {
            return Result<AuditAppendResult>::Failure(Status::Error(
                "data_log", "AUDIT_BATCH_IDEMPOTENCY_CONFLICT",
                "ambiguous audit batch_id was reused with different "
                "content"));
        }
        return Result<AuditAppendResult>::Failure(failed_replay->second);
    }
    if (audit_integrity_degraded_) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "LOG_AUDIT_INTEGRITY_DEGRADED",
            "audit writes are fenced after an ambiguous durable commit",
            false, SideEffectState::Unknown));
    }
    if (!validAuditBatchIdentity(batch)) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "AUDIT_BATCH_INVALID", "audit batch is invalid"));
    }
    if (exception_lifecycle_projection &&
        (batch.producer_endpoint_id != "ExceptionManager" ||
         call.caller_endpoint_id != "ExceptionManager" ||
         std::any_of(
             batch.records.begin(), batch.records.end(),
             [](const AuditRecord& record) {
                 return record.audit_type !=
                            "ExceptionLifecycleMutation" ||
                        record.context.producer_endpoint_id !=
                            "ExceptionManager";
             }))) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "AUDIT_CALLER_SCOPE_VIOLATION",
            "ExceptionManager may append only its own lifecycle audit"));
    }

    std::vector<bool> duplicate(batch.records.size(), false);
    std::vector<std::string> record_digests;
    record_digests.reserve(batch.records.size());
    std::map<std::string, std::string> batch_record_digests;
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        const auto& audit = batch.records[i];
        if (audit.schema_version != 1 || audit.audit_id.empty() ||
            !validAuditPrivacyContract(audit) ||
            !validAuditDurability(audit.requested_durability) ||
            !validSideEffectState(audit.side_effect_state) ||
            !validTaskPriority(audit.context.task_priority)) {
            return Result<AuditAppendResult>::Failure(Status::Error(
                "data_log", "AUDIT_RECORD_INVALID",
                "audit requires an id and D3/D4 durability"));
        }
        const auto record_digest = auditRecordDigest(audit);
        record_digests.push_back(record_digest);
        const auto local_id =
            batch_record_digests.emplace(audit.audit_id, record_digest);
        if (!local_id.second) {
            return Result<AuditAppendResult>::Failure(Status::Error(
                "data_log", "AUDIT_IDEMPOTENCY_CONFLICT",
                "audit_id occurs more than once in the same batch"));
        }
        const auto existing =
            audit_record_digests_.find(audit.audit_id);
        if (existing != audit_record_digests_.end()) {
            if (existing->second != record_digest) {
                return Result<AuditAppendResult>::Failure(Status::Error(
                    "data_log", "AUDIT_IDEMPOTENCY_CONFLICT",
                    "audit_id was reused with different content"));
            }
            if (ambiguous_audit_ids_.count(audit.audit_id) != 0) {
                return Result<AuditAppendResult>::Failure(Status::Error(
                    "data_log", "AUDIT_COMMIT_UNKNOWN",
                    "the original audit write has an ambiguous commit",
                    false, SideEffectState::Unknown));
            }
            duplicate[i] = true;
        }
        const auto sequence_key = sequenceKey(
            batch.producer_endpoint_id, batch.producer_epoch,
            audit.context.producer_sequence);
        const auto sequence = audit_sequence_owners_.find(sequence_key);
        if (sequence != audit_sequence_owners_.end() &&
            sequence->second != audit.audit_id) {
            return Result<AuditAppendResult>::Failure(Status::Error(
                "data_log", "AUDIT_PRODUCER_SEQUENCE_CONFLICT",
                "producer sequence is already owned by another audit"));
        }
    }
    const auto producer =
        producerKey(batch.producer_endpoint_id, batch.producer_epoch);
    auto next_producer_watermark =
        audit_producer_watermarks_[producer];
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        if (duplicate[i]) continue;
        const auto sequence =
            batch.records[i].context.producer_sequence;
        if (sequence <= next_producer_watermark) {
            return Result<AuditAppendResult>::Failure(Status::Error(
                "data_log", "AUDIT_PRODUCER_SEQUENCE_ROLLBACK",
                "new Audit sequence must advance within a producer epoch"));
        }
        next_producer_watermark = sequence;
    }

    std::string staged_head = hash_chain_head_;
    nlohmann::json persisted_records =
        nlohmann::json::array();
    bool all_duplicates = true;
    std::vector<std::size_t> new_indexes;
    const auto achieved_durability =
        std::any_of(batch.records.begin(), batch.records.end(),
                    [](const AuditRecord& record) {
                        return record.requested_durability ==
                               DurabilityClass::D4TamperEvident;
                    })
            ? DurabilityClass::D4TamperEvident
            : DurabilityClass::D3Fsynced;
    const bool d4 =
        achieved_durability ==
        DurabilityClass::D4TamperEvident;
    if (d4 &&
        (!tamper_evidence_ ||
         tamper_key_generation_.empty() ||
         tamper_key_material_.empty())) {
        return Result<AuditAppendResult>::Failure(Status::Error(
            "data_log", "AUDIT_D4_PROVIDER_UNAVAILABLE",
            "D4 requires an injected key and trusted anchor provider"));
    }
    const auto key_generation =
        d4 ? tamper_key_generation_ : std::string{};
    const auto anchor_generation =
        d4 ? audit_anchor_generation_ + 1 : 0;
    const auto durability_ack_id =
        "audit-ack:" + secureDigest(batch_digest);
    const auto batch_frame_count =
        static_cast<std::uint32_t>(std::count(
            duplicate.begin(), duplicate.end(), false));
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        if (duplicate[i]) continue;
        all_duplicates = false;
        new_indexes.push_back(i);
        auto encoded = auditJson(batch.records[i], staged_head);
        addBatchMetadata(
            encoded, "audit", batch.batch_id, batch_digest,
            batch.producer_endpoint_id, batch.producer_epoch,
            batch.first_sequence, batch.last_sequence, batch.checksum,
            batch.redaction_proof, AppendDisposition::Accepted,
            static_cast<std::uint32_t>(batch.records.size()), 0,
            achieved_durability, durability_ack_id,
            batch_frame_count, key_generation,
            anchor_generation);
        const auto next_hash =
            d4
                ? hmacSha256(
                      tamper_key_material_,
                      staged_head + "|" + encoded.dump())
                : secureDigest(
                      staged_head + "|" + encoded.dump());
        encoded["hash"] = next_hash;
        persisted_records.push_back(std::move(encoded));
        staged_head = next_hash;
    }

    if (all_duplicates) {
        AuditAppendResult result;
        result.disposition = AppendDisposition::Duplicate;
        result.batch_id = batch.batch_id;
        result.accepted_count =
            static_cast<std::uint32_t>(batch.records.size());
        result.achieved_durability = achieved_durability;
        result.durability_ack_id =
            "audit-record-replay:" + hash_chain_head_;
        result.hash_chain_head = hash_chain_head_;
        result.key_generation = key_generation;
        audit_batch_digests_[batch.batch_id] = batch_digest;
        audit_batch_results_[batch.batch_id] = result;
        return Result<AuditAppendResult>::Success(std::move(result));
    }

    nlohmann::json encoded_batch;
    addBatchMetadata(
        encoded_batch, "audit_batch", batch.batch_id,
        batch_digest, batch.producer_endpoint_id,
        batch.producer_epoch, batch.first_sequence,
        batch.last_sequence, batch.checksum,
        batch.redaction_proof, AppendDisposition::Accepted,
        static_cast<std::uint32_t>(batch.records.size()), 0,
        achieved_durability, durability_ack_id,
        batch_frame_count, key_generation,
        anchor_generation);
    encoded_batch["records"] = std::move(persisted_records);
    std::string journal_frame =
        finalizeJournalFrame(std::move(encoded_batch)).dump();
    journal_frame.push_back('\n');

    const auto remember_ambiguous_failure =
        [&](Status failure) -> Result<AuditAppendResult> {
        auto ambiguous = Status::Error(
            "data_log", failure.error.code, failure.error.message, false,
            SideEffectState::Unknown);
        audit_failure_digests_[batch.batch_id] = batch_digest;
        audit_batch_failures_[batch.batch_id] = ambiguous;
        for (const auto index : new_indexes) {
            const auto& audit = batch.records[index];
            audit_record_digests_[audit.audit_id] =
                record_digests[index];
            ambiguous_audit_ids_.insert(audit.audit_id);
            audit_sequence_owners_[sequenceKey(
                batch.producer_endpoint_id, batch.producer_epoch,
                audit.context.producer_sequence)] = audit.audit_id;
        }
        audit_producer_watermarks_[producer] =
            next_producer_watermark;
        audit_integrity_degraded_ = true;
        appendEmergencySummaryUnlocked(
            batch.batch_id + ":audit_commit_unknown");
        return Result<AuditAppendResult>::Failure(std::move(ambiguous));
    };

    Status io_status = Status::Ok();
    lock.unlock();
    audit_stream_ << journal_frame;
    if (!audit_stream_) {
        io_status = Status::Error(
            "data_log", "LOG_AUDIT_PERSIST_FAILED",
            "audit journal batch write failed", false,
            SideEffectState::Unknown);
    }
    if (io_status.ok) {
        audit_stream_.flush();
        if (!audit_stream_) {
            io_status = Status::Error(
                "data_log", "LOG_AUDIT_FLUSH_FAILED",
                "audit journal flush failed", false,
                SideEffectState::Unknown);
        }
    }
    if (io_status.ok) {
        io_status = invokeDurabilitySync(
            durability_sync_,
            storage_directory_ / "audit.jsonl");
    }
    AuditAnchorSnapshot committed_anchor;
    if (io_status.ok && d4) {
        committed_anchor.present = true;
        committed_anchor.generation =
            anchor_generation;
        committed_anchor.hash_chain_head =
            staged_head;
        committed_anchor.key_generation =
            key_generation;
        committed_anchor.authentication_code =
            anchorAuthenticationCode(
                tamper_key_material_, committed_anchor);
        try {
            io_status =
                tamper_evidence_->commitAnchor(
                    committed_anchor);
        } catch (...) {
            io_status = Status::Error(
                "data_log", "AUDIT_D4_ANCHOR_COMMIT_FAILED",
                "trusted anchor provider threw after journal fsync",
                false, SideEffectState::Unknown);
        }
        if (!io_status.ok) {
            io_status = Status::Error(
                "data_log",
                io_status.error.code.empty()
                    ? "AUDIT_D4_ANCHOR_COMMIT_FAILED"
                    : io_status.error.code,
                io_status.error.message.empty()
                    ? "trusted anchor commit failed after journal fsync"
                    : io_status.error.message,
                false, SideEffectState::Unknown);
        }
        if (io_status.ok) {
            try {
                const auto observed =
                    tamper_evidence_->loadAnchor();
                const bool exact_commit =
                    observed.status.ok && observed.value &&
                    observed.value->present &&
                    observed.value->generation ==
                        committed_anchor.generation &&
                    observed.value->hash_chain_head ==
                        committed_anchor.hash_chain_head &&
                    observed.value->key_generation ==
                        committed_anchor.key_generation &&
                    observed.value->authentication_code ==
                        committed_anchor.authentication_code;
                if (!exact_commit) {
                    io_status = Status::Error(
                        "data_log",
                        "AUDIT_D4_ANCHOR_READBACK_MISMATCH",
                        "trusted anchor did not confirm the exact commit",
                        false, SideEffectState::Unknown);
                }
            } catch (...) {
                io_status = Status::Error(
                    "data_log",
                    "AUDIT_D4_ANCHOR_READBACK_FAILED",
                    "trusted anchor readback failed after commit",
                    false, SideEffectState::Unknown);
            }
        }
    }
    lock.lock();
    if (!io_status.ok) {
        return remember_ambiguous_failure(io_status);
    }

    for (const auto index : new_indexes) {
        const auto& audit = batch.records[index];
        audits_.push_back(audit);
        audit_record_digests_[audit.audit_id] =
            record_digests[index];
        audit_sequence_owners_[sequenceKey(
            batch.producer_endpoint_id, batch.producer_epoch,
            audit.context.producer_sequence)] = audit.audit_id;
    }
    hash_chain_head_ = staged_head;
    if (d4) {
        audit_anchor_generation_ =
            committed_anchor.generation;
        audit_anchor_head_ =
            committed_anchor.hash_chain_head;
    }
    audit_producer_watermarks_[producer] =
        next_producer_watermark;
    AuditAppendResult result;
    result.disposition = all_duplicates
                             ? AppendDisposition::Duplicate
                             : AppendDisposition::Accepted;
    result.batch_id = batch.batch_id;
    result.accepted_count =
        static_cast<std::uint32_t>(batch.records.size());
    result.achieved_durability = achieved_durability;
    result.durability_ack_id = durability_ack_id;
    result.hash_chain_head = hash_chain_head_;
    result.key_generation = key_generation;
    audit_batch_digests_[batch.batch_id] = batch_digest;
    audit_batch_results_[batch.batch_id] = result;
    return Result<AuditAppendResult>::Success(std::move(result));
}


}  // namespace master_agent::data_log
