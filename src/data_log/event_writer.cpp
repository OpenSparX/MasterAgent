/**
 * @file event_writer.cpp
 * @brief Validates and durably appends structured event batches.
 */

#include "include/batch_validation.h"
#include "include/journal_integrity.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_io.h"
#include "include/record_serialization.h"
#include "include/trace_filter.h"

namespace master_agent::data_log {

Result<LogAppendResult> DataLogService::appendEvents(
    const LogEventBatch& batch, const CallContext& call) {

    std::lock_guard<std::mutex> writer_lock(event_writer_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_) {
        return Result<LogAppendResult>::Failure(Status::Error(
            "data_log", "LOG_NOT_READY", "log service is not ready", true));
    }
    if (call.caller == CallerModuleId::Invalid) {
        return Result<LogAppendResult>::Failure(Status::Error(
            "data_log", "LOG_CALLER_NOT_ALLOWED",
            "appendEvents requires a designed caller module"));
    }
    if (call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<LogAppendResult>::Failure(Status::Error(
            "data_log", "LOG_CALL_EXPIRED", "log call deadline expired"));
    }
    if (!producerAuthenticated(
            call, batch.producer_endpoint_id,
            batch.producer_epoch)) {
        return Result<LogAppendResult>::Failure(Status::Error(
            "data_log", "LOG_PRODUCER_IDENTITY_MISMATCH",
            "batch producer is not bound to the authenticated caller"));
    }
    const auto bounds = validateEventBatchBounds(batch);
    if (!bounds.ok) {
        return Result<LogAppendResult>::Failure(bounds);
    }
    const auto batch_digest = eventBatchDigest(batch);
    const auto replay = event_batch_results_.find(batch.batch_id);
    if (replay != event_batch_results_.end()) {
        if (event_batch_digests_.at(batch.batch_id) !=
            batch_digest) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_BATCH_IDEMPOTENCY_CONFLICT",
                "batch_id was reused with different content"));
        }
        auto duplicate = replay->second;
        if (duplicate.disposition != AppendDisposition::Rejected) {
            duplicate.disposition = AppendDisposition::Duplicate;
        }
        return Result<LogAppendResult>::Success(std::move(duplicate));
    }
    const auto failed_replay =
        event_batch_failures_.find(batch.batch_id);
    if (failed_replay != event_batch_failures_.end()) {
        if (event_failure_digests_.at(batch.batch_id) != batch_digest) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_BATCH_IDEMPOTENCY_CONFLICT",
                "ambiguous batch_id was reused with different content"));
        }
        return Result<LogAppendResult>::Failure(failed_replay->second);
    }
    if (!validBatchIdentity(batch)) {
        return Result<LogAppendResult>::Failure(Status::Error(
            "data_log", "LOG_BATCH_SEQUENCE_INVALID",
            "batch identity or producer sequence is invalid"));
    }

    LogAppendResult result;
    result.batch_id = batch.batch_id;
    result.accepted_first_sequence = batch.first_sequence;
    result.accepted_last_sequence = batch.last_sequence;
    result.disposition = AppendDisposition::Accepted;
    result.achieved_durability = DurabilityClass::D0Volatile;

    std::vector<bool> valid(batch.records.size(), true);
    std::vector<bool> duplicate(batch.records.size(), false);
    std::vector<std::string> record_digests;
    record_digests.reserve(batch.records.size());
    std::map<std::string, std::string> batch_record_digests;
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        const auto& event = batch.records[i];
        if (event.requested_durability ==
            DurabilityClass::D4TamperEvident) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_EVENT_D4_REQUIRES_AUDIT",
                "Event supports D0-D3; D4 facts must use AuditRecord"));
        }
        if (event.schema_version != 1 ||
            !validEventDurability(event.requested_durability) ||
            !validEventSeverity(event.severity) ||
            !validTaskPriority(event.context.task_priority)) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_EVENT_SCHEMA_INVALID",
                "Event requires schema v1 and closed enum values"));
        }
        const auto record_digest = eventRecordDigest(event);
        record_digests.push_back(record_digest);
        if (event.event_id.empty() || event.event_type.empty() ||
            containsForbiddenPayload(
                event.event_type,
                event.payload_summary_json)) {
            valid[i] = false;
            ++result.rejected_count;
            continue;
        }
        const auto local_id =
            batch_record_digests.emplace(event.event_id, record_digest);
        if (!local_id.second) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_EVENT_IDEMPOTENCY_CONFLICT",
                "event_id occurs more than once in the same batch"));
        }
        const auto existing =
            event_record_digests_.find(event.event_id);
        if (existing != event_record_digests_.end()) {
            if (existing->second != record_digest) {
                return Result<LogAppendResult>::Failure(Status::Error(
                    "data_log", "LOG_EVENT_IDEMPOTENCY_CONFLICT",
                    "event_id was reused with different content"));
            }
            if (ambiguous_event_ids_.count(event.event_id) != 0) {
                return Result<LogAppendResult>::Failure(Status::Error(
                    "data_log", "LOG_EVENT_COMMIT_UNKNOWN",
                    "the original event write has an ambiguous commit",
                    false, SideEffectState::Unknown));
            }
            duplicate[i] = true;
        }
        const auto sequence_key = sequenceKey(
            batch.producer_endpoint_id, batch.producer_epoch,
            event.context.producer_sequence);
        const auto sequence = event_sequence_owners_.find(sequence_key);
        if (sequence != event_sequence_owners_.end() &&
            sequence->second != event.event_id) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_PRODUCER_SEQUENCE_CONFLICT",
                "producer sequence is already owned by another event"));
        }
    }
    const auto producer =
        producerKey(batch.producer_endpoint_id, batch.producer_epoch);
    auto next_producer_watermark =
        event_producer_watermarks_[producer];
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        if (!valid[i] || duplicate[i]) continue;
        const auto sequence =
            batch.records[i].context.producer_sequence;
        if (sequence <= next_producer_watermark) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_PRODUCER_SEQUENCE_ROLLBACK",
                "new Event sequence must advance within a producer epoch"));
        }
        next_producer_watermark = sequence;
    }

    if (result.rejected_count != 0) {
        result.rejected_count =
            static_cast<std::uint32_t>(batch.records.size());
        result.accepted_count = 0;
        result.accepted_first_sequence = 0;
        result.accepted_last_sequence = 0;
        result.disposition = AppendDisposition::Rejected;
        event_batch_digests_[batch.batch_id] = batch_digest;
        event_batch_results_[batch.batch_id] = result;
        return Result<LogAppendResult>::Success(std::move(result));
    }

    std::vector<std::size_t> accepted_indexes;
    bool all_accepted_are_duplicates = true;
    DurabilityClass new_record_durability =
        DurabilityClass::D0Volatile;
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        if (!valid[i]) continue;
        const auto& event = batch.records[i];
        accepted_indexes.push_back(i);
        ++result.accepted_count;
        all_accepted_are_duplicates =
            all_accepted_are_duplicates && duplicate[i];
        if (!duplicate[i] &&
            static_cast<std::uint8_t>(event.requested_durability) >
                static_cast<std::uint8_t>(new_record_durability)) {
            new_record_durability = event.requested_durability;
        }
        if (static_cast<std::uint8_t>(event.requested_durability) >
            static_cast<std::uint8_t>(result.achieved_durability)) {
            result.achieved_durability = event.requested_durability;
        }
    }
    if (std::any_of(accepted_indexes.begin(), accepted_indexes.end(),
                    [&](std::size_t index) {
                        return !duplicate[index];
                    }) &&
        static_cast<std::uint8_t>(result.achieved_durability) >
            static_cast<std::uint8_t>(new_record_durability)) {

        new_record_durability = result.achieved_durability;
    }

    if (result.rejected_count > 0 && result.accepted_count > 0) {
        result.disposition = AppendDisposition::PartialAccepted;
    } else if (result.accepted_count == 0) {
        result.disposition = AppendDisposition::Rejected;
    } else if (all_accepted_are_duplicates) {
        result.disposition = AppendDisposition::Duplicate;
    }

    if (result.disposition == AppendDisposition::Duplicate) {
        if (static_cast<std::uint8_t>(result.achieved_durability) >=
            static_cast<std::uint8_t>(
                DurabilityClass::D2Journaled)) {
            result.durability_ack_id =
                "log-record-replay:" + secureDigest(batch_digest);
        }
        accepted_batches_.insert(batch.batch_id);
        event_batch_digests_[batch.batch_id] = batch_digest;
        event_batch_results_[batch.batch_id] = result;
        return Result<LogAppendResult>::Success(std::move(result));
    }

    std::vector<bool> included_in_journal(
        batch.records.size(), false);
    std::vector<std::size_t> journal_indexes;
    if (static_cast<std::uint8_t>(new_record_durability) >=
        static_cast<std::uint8_t>(
            DurabilityClass::D1Buffered)) {
        for (const auto index : accepted_indexes) {
            const auto& event = batch.records[index];
            const bool already_at_least_buffered =
                persisted_event_ids_.count(event.event_id) != 0 ||
                buffered_event_ids_.count(event.event_id) != 0;
            if (!duplicate[index] || !already_at_least_buffered) {
                included_in_journal[index] = true;
                journal_indexes.push_back(index);
            }
        }
    }
    auto next_journal_watermark =
        event_journal_watermarks_[producer];
    for (const auto index : journal_indexes) {
        const auto sequence =
            batch.records[index].context.producer_sequence;
        if (sequence <= next_journal_watermark) {
            return Result<LogAppendResult>::Failure(Status::Error(
                "data_log", "LOG_EVENT_BACKFILL_ORDER_INVALID",
                "a volatile Event cannot be backfilled behind a later "
                "physical producer sequence"));
        }
        next_journal_watermark = sequence;
    }

    std::string journal_frame;
    if (!journal_indexes.empty()) {
        if (static_cast<std::uint8_t>(new_record_durability) >=
            static_cast<std::uint8_t>(
                DurabilityClass::D2Journaled)) {
            result.durability_ack_id =
                "log-ack:" + secureDigest(batch_digest);
        }
        const auto batch_frame_count =
            static_cast<std::uint32_t>(
                journal_indexes.size());
        nlohmann::json persisted_records =
            nlohmann::json::array();
        for (const auto index : journal_indexes) {
            const auto& event = batch.records[index];
            persisted_records.push_back(eventJson(event));
        }
        nlohmann::json encoded_batch;
        addBatchMetadata(
            encoded_batch, "event_batch", batch.batch_id,
            batch_digest, batch.producer_endpoint_id,
            batch.producer_epoch, batch.first_sequence,
            batch.last_sequence, batch.checksum,
            batch.redaction_proof, result.disposition,
            result.accepted_count, result.rejected_count,
            result.achieved_durability,
            result.durability_ack_id.value_or(""),
            batch_frame_count);
        encoded_batch["records"] =
            std::move(persisted_records);
        journal_frame =
            finalizeJournalFrame(std::move(encoded_batch)).dump();
        journal_frame.push_back('\n');
    }

    const auto remember_ambiguous_failure =
        [&](Status failure) -> Result<LogAppendResult> {

        auto ambiguous = Status::Error(
            "data_log", failure.error.code, failure.error.message, false,
            SideEffectState::Unknown);
        event_failure_digests_[batch.batch_id] = batch_digest;
        event_batch_failures_[batch.batch_id] = ambiguous;
        for (const auto index : journal_indexes) {
            const auto& event = batch.records[index];
            event_record_digests_[event.event_id] =
                record_digests[index];
            ambiguous_event_ids_.insert(event.event_id);
            event_sequence_owners_[sequenceKey(
                batch.producer_endpoint_id, batch.producer_epoch,
                event.context.producer_sequence)] = event.event_id;
        }
        event_producer_watermarks_[producer] =
            next_producer_watermark;
        event_journal_watermarks_[producer] =
            next_journal_watermark;
        appendEmergencySummaryUnlocked(
            batch.batch_id + ":event_commit_unknown");
        return Result<LogAppendResult>::Failure(std::move(ambiguous));
    };

    Status io_status = Status::Ok();
    if (static_cast<std::uint8_t>(new_record_durability) >=
        static_cast<std::uint8_t>(DurabilityClass::D1Buffered)) {

        lock.unlock();
        if (!journal_frame.empty()) {
            event_stream_ << journal_frame;
            if (!event_stream_) {
                io_status = Status::Error(
                    "data_log", "LOG_EVENT_PERSIST_FAILED",
                    "event journal batch write failed", false,
                    SideEffectState::Unknown);
            }
        }
        if (io_status.ok &&
            static_cast<std::uint8_t>(new_record_durability) >=
                static_cast<std::uint8_t>(
                    DurabilityClass::D2Journaled)) {
            event_stream_.flush();
            if (!event_stream_) {
                io_status = Status::Error(
                    "data_log", "LOG_EVENT_FLUSH_FAILED",
                    "event journal flush failed", false,
                    SideEffectState::Unknown);
            }
        }
        if (io_status.ok &&
            new_record_durability == DurabilityClass::D3Fsynced) {
            io_status = invokeDurabilitySync(
                durability_sync_,
                storage_directory_ / "events.jsonl");
        }
        lock.lock();
        if (!io_status.ok) {
            return remember_ambiguous_failure(io_status);
        }
    } else if (static_cast<std::uint8_t>(result.achieved_durability) >=
               static_cast<std::uint8_t>(
                   DurabilityClass::D2Journaled)) {
        result.durability_ack_id =
            "log-record-replay:" + secureDigest(batch_digest);
    }

    for (const auto index : accepted_indexes) {
        if (duplicate[index]) continue;
        const auto& event = batch.records[index];
        events_.push_back(event);
        event_record_digests_[event.event_id] = record_digests[index];
        event_sequence_owners_[sequenceKey(
            batch.producer_endpoint_id, batch.producer_epoch,
            event.context.producer_sequence)] = event.event_id;
        if (included_in_journal[index]) {
            if (static_cast<std::uint8_t>(
                    new_record_durability) >=
                static_cast<std::uint8_t>(
                    DurabilityClass::D2Journaled)) {
                persisted_event_ids_.insert(event.event_id);
            } else {
                buffered_event_ids_.insert(event.event_id);
            }
        }
    }
    for (const auto index : journal_indexes) {
        if (!duplicate[index]) continue;
        const auto& event = batch.records[index];
        if (static_cast<std::uint8_t>(new_record_durability) >=
            static_cast<std::uint8_t>(
                DurabilityClass::D2Journaled)) {
            persisted_event_ids_.insert(event.event_id);
        } else {
            buffered_event_ids_.insert(event.event_id);
        }
    }
    if (static_cast<std::uint8_t>(new_record_durability) >=
        static_cast<std::uint8_t>(
            DurabilityClass::D2Journaled)) {
        persisted_event_ids_.insert(buffered_event_ids_.begin(),
                                    buffered_event_ids_.end());
        buffered_event_ids_.clear();
    }
    if (result.accepted_count != 0) {
        accepted_batches_.insert(batch.batch_id);
    }
    event_producer_watermarks_[producer] =
        next_producer_watermark;
    if (!journal_indexes.empty()) {
        event_journal_watermarks_[producer] =
            next_journal_watermark;
    }
    event_batch_digests_[batch.batch_id] = batch_digest;
    event_batch_results_[batch.batch_id] = result;
    return Result<LogAppendResult>::Success(std::move(result));
}

// Audit commits preserve the authenticated chain and external anchor order.
// D4 is acknowledged only after both journal durability and anchor commit.

}  // namespace master_agent::data_log
