/**
 * @file exception_reporter.cpp
 * @brief Validates, groups, persists, and projects exception reports.
 */

#include "include/exception_validation.h"
#include "include/exception_identity.h"
#include "include/exception_durability.h"
#include "include/exception_journal_codec.h"
#include "include/exception_journal_codec.h"
#include "include/exception_runtime_policy.h"

namespace master_agent::exception {

Result<ExceptionReportResult> ExceptionManager::report(
    const ExceptionReportRequest& request, const CallContext& call) {

    const auto caller = validateExceptionReportCaller(call);
    if (!caller.ok) {
        return Result<ExceptionReportResult>::Failure(caller);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_) {
        return Result<ExceptionReportResult>::Failure(
            initialization_status_);
    }
    if (call.deadline_mono_ns <= 0 ||
        deadlineExpired(call.deadline_mono_ns, *clock_)) {
        return Result<ExceptionReportResult>::Failure(Status::Error(
            "exception", "EXM_CALL_EXPIRED",
            "a live monotonic deadline is required"));
    }
    if (request.report_id.empty() || request.occurrences.empty() ||
        !isD2OrD3(request.requested_durability) ||
        !safeExceptionReference(
            request.source_redaction_proof, 256)) {
        return Result<ExceptionReportResult>::Failure(Status::Error(
            "exception", "EXM_PRIVACY_CONTRACT_INVALID",
            "report, D2/D3 durability and source redaction proof are required"));
    }
    constexpr std::size_t kMaxOccurrences = 128;
    constexpr std::size_t kMaxReportBytes = 1024 * 1024;
    constexpr std::size_t kMaxFieldBytes = 4096;
    if (request.batch_checksum.empty()) {
        return Result<ExceptionReportResult>::Failure(Status::Error(
            "exception", "EXM_BATCH_CHECKSUM_INVALID",
            "batch checksum is required"));
    }
    if (request.report_id.size() > 256 ||
        request.batch_checksum.size() > 256 ||
        request.occurrences.size() > kMaxOccurrences) {
        return Result<ExceptionReportResult>::Failure(Status::Error(
            "exception", "EXM_REPORT_TOO_LARGE",
            "exception report exceeds a configured batch limit"));
    }
    std::size_t estimated_bytes = 0;
    for (const auto& occurrence : request.occurrences) {
        const std::vector<const std::string*> fields{
            &occurrence.occurrence_id,
            &occurrence.producer_endpoint_id,
            &occurrence.domain,
            &occurrence.code,
            &occurrence.source_module,
            &occurrence.source_interface,
            &occurrence.operation,
            &occurrence.retry_scope_hint,
            &occurrence.bounded_detail_code,
            &occurrence.bounded_detail_summary,
            &occurrence.redaction_policy_version,
            &occurrence.normalizer_version};
        bool record_too_large =
            occurrence.evidence_event_ids.size() > 32 ||
            occurrence.evidence_audit_ids.size() > 32 ||
            occurrence.privacy_labels.size() > 8;
        for (const auto* field : fields) {
            record_too_large =
                record_too_large ||
                field->size() > kMaxFieldBytes;
            if (estimated_bytes >
                kMaxReportBytes -
                    std::min(field->size(), kMaxReportBytes)) {
                record_too_large = true;
            } else {
                estimated_bytes += field->size();
            }
        }
        const auto bounded_evidence =
            [](const std::vector<std::string>& refs) {
                return std::all_of(
                    refs.begin(), refs.end(),
                    [](const std::string& ref) {
                        return !ref.empty() &&
                               ref.size() <= 512;
                    });
            };
        if (record_too_large ||
            !bounded_evidence(occurrence.evidence_event_ids) ||
            !bounded_evidence(occurrence.evidence_audit_ids) ||
            estimated_bytes > kMaxReportBytes) {
            return Result<ExceptionReportResult>::Failure(Status::Error(
                "exception", "EXM_REPORT_TOO_LARGE",
                "exception report exceeds a configured byte limit"));
        }
    }

    auto normalized_occurrences = request.occurrences;
    for (auto& occurrence : normalized_occurrences) {
        if (occurrence.schema_version != 1 ||
            !validSeverity(occurrence.reported_severity) ||
            !validImpact(occurrence.impact) ||
            (occurrence.side_effect_state &&
             !validSideEffect(*occurrence.side_effect_state)) ||
            !validPriority(occurrence.context.task_priority)) {
            return Result<ExceptionReportResult>::Failure(
                Status::Error(
                    "exception",
                    "EXM_OCCURRENCE_SCHEMA_INVALID",
                    "occurrence requires schema v1 and closed "
                    "enum values"));
        }
        if (!validExceptionPrivacyContract(occurrence)) {
            return Result<ExceptionReportResult>::Failure(
                Status::Error(
                    "exception",
                    "EXM_PRIVACY_CONTRACT_INVALID",
                    "privacy labels or redacted summary is invalid"));
        }
        if (occurrence.producer_endpoint_id.empty()) {
            occurrence.producer_endpoint_id =
                call.caller_endpoint_id.empty()
                    ? "agent-service"
                    : call.caller_endpoint_id;
        }
        if (occurrence.producer_epoch == 0) {
            occurrence.producer_epoch =
                call.caller_process_epoch == 0
                    ? 1
                    : call.caller_process_epoch;
        }
        if ((!call.caller_endpoint_id.empty() &&
             occurrence.producer_endpoint_id !=
                 call.caller_endpoint_id) ||
            (call.caller_process_epoch != 0 &&
             occurrence.producer_epoch !=
                 call.caller_process_epoch)) {
            return Result<ExceptionReportResult>::Failure(
                Status::Error(
                    "exception",
                    "EXM_PRODUCER_IDENTITY_MISMATCH",
                    "occurrence producer must match the authenticated "
                    "AgentService endpoint and process epoch"));
        }
        const bool context_producer_unspecified =
            occurrence.context.producer_endpoint_id.empty();
        if (context_producer_unspecified) {
            occurrence.context.producer_endpoint_id =
                occurrence.producer_endpoint_id;
            occurrence.context.producer_epoch =
                occurrence.producer_epoch;
            occurrence.context.producer_sequence =
                occurrence.producer_sequence;
        }
        if (occurrence.context.producer_endpoint_id !=
                occurrence.producer_endpoint_id ||
            occurrence.context.producer_epoch !=
                occurrence.producer_epoch ||
            (occurrence.producer_sequence != 0 &&
             occurrence.context.producer_sequence !=
                 occurrence.producer_sequence)) {
            return Result<ExceptionReportResult>::Failure(
                Status::Error(
                    "exception",
                    "EXM_PRODUCER_IDENTITY_MISMATCH",
                    "occurrence context producer identity conflicts "
                    "with its idempotency envelope"));
        }
    }
    auto normalized_request = request;
    normalized_request.occurrences = normalized_occurrences;
    if (normalized_request.batch_checksum !=
        exceptionBatchChecksum(normalized_request)) {
        return Result<ExceptionReportResult>::Failure(
            Status::Error(
                "exception", "EXM_BATCH_CHECKSUM_INVALID",
                "batch checksum does not bind the normalized report"));
    }
    const auto request_digest = reportDigest(normalized_request);
    const auto tx_id = transactionId(
        "report", request.report_id, request_digest);

    const auto replay = report_results_.find(request.report_id);
    if (replay != report_results_.end()) {
        if (report_digests_.at(request.report_id) != request_digest) {
            return Result<ExceptionReportResult>::Failure(Status::Error(
                "exception", "EXM_REPORT_IDEMPOTENCY_CONFLICT",
                "report_id was reused with different content"));
        }
        return Result<ExceptionReportResult>::Success(replay->second);
    }
    const auto fenced =
        fenced_transaction_failures_.find(tx_id);
    if (fenced != fenced_transaction_failures_.end()) {
        if (fenced_transaction_digests_.at(tx_id) != request_digest) {
            return Result<ExceptionReportResult>::Failure(Status::Error(
                "exception", "EXM_REPORT_IDEMPOTENCY_CONFLICT",
                "fenced report was reused with different content"));
        }
        return Result<ExceptionReportResult>::Failure(fenced->second);
    }
    if (journal_fenced_) {
        return Result<ExceptionReportResult>::Failure(durabilityUnknown(
            "exception journal is fenced pending startup recovery"));
    }
    if (journal_commit_inflight_) {
        return Result<ExceptionReportResult>::Failure(Status::Error(
            "exception", "EXM_WRITER_BUSY",
            "another exception journal transaction is in progress", true));
    }

    auto target_durability = request.requested_durability;
    for (const auto& occurrence : normalized_occurrences) {
        if (occurrence.reported_severity ==
                ExceptionSeverity::Critical ||
            occurrence.impact == ExceptionImpact::SafetyAffected ||
            occurrence.side_effect_state == SideEffectState::Unknown) {
            target_durability =
                data_log::DurabilityClass::D3Fsynced;
        }
    }
    const auto durability_ack_id = tx_id + ":d" +
        std::to_string(static_cast<std::uint8_t>(target_durability));

    auto next_groups = groups_;
    auto next_fingerprints = fingerprint_to_exception_;
    auto next_occurrence_results = occurrence_results_;
    auto next_occurrence_digests = occurrence_digests_;
    auto next_watermarks = producer_sequence_watermarks_;
    auto next_sequence_owners = producer_sequence_owners_;
    std::set<std::string> touched_groups;
    Json occurrence_entries = Json::array();
    ExceptionReportResult report_result;
    report_result.report_id = request.report_id;
    std::vector<data_log::LogEvent> observation_events;
    auto next_observation_sequence = observation_sequence_;

    for (auto occurrence : normalized_occurrences) {
        ExceptionAccepted accepted;
        accepted.occurrence_id = occurrence.occurrence_id;
        if (occurrence.occurrence_id.empty() ||
            occurrence.domain.empty() || occurrence.code.empty() ||
            occurrence.source_module.empty() ||
            occurrence.source_interface.empty() ||
            occurrence.bounded_detail_summary.size() > 512) {
            accepted.disposition = ExceptionDisposition::Rejected;
            report_result.results.push_back(std::move(accepted));
            ++report_result.rejected_count;
            continue;
        }

        const auto key = occurrenceKey(
            occurrence.producer_endpoint_id,
            occurrence.producer_epoch, occurrence.occurrence_id);
        const auto digest = occurrenceDigest(occurrence);
        const auto occurrence_replay =
            next_occurrence_results.find(key);
        if (occurrence_replay != next_occurrence_results.end()) {
            if (next_occurrence_digests.at(key) != digest) {
                return Result<ExceptionReportResult>::Failure(
                    Status::Error(
                        "exception", "EXM_OCCURRENCE_CONFLICT",
                        "occurrence idempotency key has different content"));
            }
            accepted = occurrence_replay->second;
            accepted.disposition =
                ExceptionDisposition::DuplicateOccurrence;
            const auto group =
                next_groups.find(accepted.exception_id);
            if (group != next_groups.end()) {
                ++group->second.duplicate_replay_count;
                touched_groups.insert(group->first);
                // A transport replay is operational metadata, not a
                // lifecycle mutation. Persist the replay counter without
                // advancing the optimistic-concurrency group version, and
                // return the exact first acceptance version/durability ack.
            }
            report_result.results.push_back(std::move(accepted));
            ++report_result.accepted_count;
            continue;
        }

        const auto producer = producerKey(
            occurrence.producer_endpoint_id,
            occurrence.producer_epoch);
        const auto watermark = next_watermarks[producer];
        const auto effective_sequence =
            occurrence.producer_sequence == 0
                ? watermark + 1
                : occurrence.producer_sequence;
        const auto sequence_key = sequenceKey(
            occurrence.producer_endpoint_id,
            occurrence.producer_epoch, effective_sequence);
        const auto existing_owner =
            next_sequence_owners.find(sequence_key);
        if (existing_owner != next_sequence_owners.end() &&
            existing_owner->second != key) {
            return Result<ExceptionReportResult>::Failure(Status::Error(
                "exception", "EXM_SEQUENCE_CONFLICT",
                "producer sequence is owned by another occurrence"));
        }
        if (occurrence.producer_sequence != 0 &&
            effective_sequence <= watermark) {
            return Result<ExceptionReportResult>::Failure(Status::Error(
                "exception", "EXM_SEQUENCE_CONFLICT",
                "producer sequence rolled back without a new epoch"));
        }
        next_watermarks[producer] = effective_sequence;
        next_sequence_owners[sequence_key] = key;

        if (occurrence.received_at_utc_ms == 0) {
            occurrence.received_at_utc_ms = clock_->utcNowMs();
        }
        const auto fingerprint = fingerprintOf(occurrence);

        accepted.fingerprint = fingerprint;
        const auto mapping = next_fingerprints.find(fingerprint);
        if (mapping == next_fingerprints.end()) {
            ExceptionGroup group;
            group.exception_id = ids_->next("exception");
            group.fingerprint = fingerprint;
            group.version = 1;
            group.domain = occurrence.domain;
            group.code = occurrence.code;
            group.current_severity =
                occurrence.reported_severity;
            group.aggregate_impact = occurrence.impact;
            group.source_module = occurrence.source_module;
            group.source_interface =
                occurrence.source_interface;
            group.first_seen_at_utc_ms =
                occurrence.occurred_at_utc_ms;
            group.last_seen_at_utc_ms =
                occurrence.occurred_at_utc_ms;
            group.occurrence_count = 1;
            group.current_escalation =
                escalationFor(occurrence, 1);
            group.bounded_occurrence_ids.push_back(
                occurrence.occurrence_id);
            accepted.exception_id = group.exception_id;
            accepted.disposition =
                ExceptionDisposition::NewGroup;
            next_fingerprints[fingerprint] =
                group.exception_id;
            next_groups[group.exception_id] = std::move(group);
        } else {
            auto& group = next_groups.at(mapping->second);
            accepted.exception_id = group.exception_id;
            const bool reopened =
                group.lifecycle == ExceptionLifecycle::Resolved;
            accepted.disposition =
                reopened ? ExceptionDisposition::Reopened
                         : ExceptionDisposition::Aggregated;
            if (reopened) {
                group.lifecycle = ExceptionLifecycle::Reopened;
            }
            ++group.version;
            ++group.occurrence_count;
            group.last_seen_at_utc_ms =
                occurrence.occurred_at_utc_ms;
            group.current_severity = maxSeverity(
                group.current_severity,
                occurrence.reported_severity);
            group.aggregate_impact = maxImpact(
                group.aggregate_impact, occurrence.impact);
            group.current_escalation = maxEscalation(
                group.current_escalation,
                escalationFor(occurrence,
                              group.occurrence_count));
            if (group.bounded_occurrence_ids.size() < 16) {
                group.bounded_occurrence_ids.push_back(
                    occurrence.occurrence_id);
            }
        }
        auto& group = next_groups.at(accepted.exception_id);
        touched_groups.insert(group.exception_id);
        accepted.group_version = group.version;
        accepted.applied_severity = group.current_severity;
        accepted.total_count = group.occurrence_count;
        accepted.lifecycle = group.lifecycle;
        accepted.escalation = group.current_escalation;
        accepted.achieved_durability = target_durability;
        accepted.durability_ack_id = durability_ack_id;
        next_occurrence_results[key] = accepted;
        next_occurrence_digests[key] = digest;
        occurrence_entries.push_back(
            Json{{"occurrence_key", key},
                 {"digest", digest},
                 {"producer_endpoint_id",
                  occurrence.producer_endpoint_id},
                 {"producer_epoch", occurrence.producer_epoch},
                 {"effective_sequence", effective_sequence},
                 {"accepted", acceptedToJson(accepted)}});

        data_log::LogEvent event;
        event.event_id =
            "exception-event-" +
            secureDigest(tx_id + "|" + key).substr(0, 32);
        event.event_type = "EXCEPTION_OCCURRENCE_ACCEPTED";
        event.module = "ExceptionManager";
        event.interface_name = "IExceptionManager.report";
        event.operation = occurrence.operation;
        event.context = occurrence.context;
        event.context.request_id =
            event.context.request_id.empty() ? call.request_id
                                             : event.context.request_id;
        event.context.trace_id =
            event.context.trace_id.empty() ? call.trace_id
                                           : event.context.trace_id;
        event.context.producer_endpoint_id = "ExceptionManager";
        event.context.producer_epoch = writer_epoch_;
        event.context.producer_sequence =
            ++next_observation_sequence;
        event.context.task_priority = call.priority;
        event.context.deadline_mono_ns = call.deadline_mono_ns;
        event.outcome = "accepted";
        event.occurred_at_utc_ms = clock_->utcNowMs();
        event.occurred_at_mono_ns = clock_->monotonicNowNs();
        event.severity =
            occurrence.reported_severity ==
                    ExceptionSeverity::Critical
                ? data_log::EventSeverity::Critical
                : data_log::EventSeverity::Error;
        event.requested_durability =
            target_durability;
        event.payload_summary_json =
            Json{{"domain", occurrence.domain},
                 {"code", occurrence.code},
                 {"exception_id", accepted.exception_id},
                 {"transaction_id", tx_id}}
                .dump();
        observation_events.push_back(std::move(event));
        report_result.results.push_back(accepted);
        ++report_result.accepted_count;
    }
    report_result.partial =
        report_result.accepted_count > 0 &&
        report_result.rejected_count > 0;

    data_log::LogEventBatch observation;
    Json encoded_observation = nullptr;
    if (!observation_events.empty()) {
        observation.batch_id = "exception-observation:" + tx_id;
        observation.producer_endpoint_id = "ExceptionManager";
        observation.producer_epoch = writer_epoch_;
        observation.first_sequence =
            observation_events.front().context.producer_sequence;
        observation.last_sequence =
            observation_events.back().context.producer_sequence;
        observation.checksum =
            secureDigest(tx_id + "|" + request_digest);
        observation.records = std::move(observation_events);
        encoded_observation = batchToJson(observation);
    }

    Json groups_after = Json::array();
    for (const auto& id : touched_groups) {
        groups_after.push_back(groupToJson(next_groups.at(id)));
    }
    const Json transaction{
        {"kind", "report"},
        {"transaction_id", tx_id},
        {"idempotency_key", request.report_id},
        {"request_digest", request_digest},
        {"journal_sequence", journal_sequence_ + 1},
        {"writer_epoch", writer_epoch_},
        {"durability",
         static_cast<std::uint8_t>(target_durability)},
        {"committed_at_utc_ms", clock_->utcNowMs()},
        {"groups_after", std::move(groups_after)},
        {"occurrence_entries", std::move(occurrence_entries)},
        {"result", reportResultToJson(report_result)},
        {"observation", std::move(encoded_observation)}};
    const auto transaction_payload = transaction.dump();
    journal_commit_inflight_ = true;
    lock.unlock();
    const auto persisted = persistTransactionUnlocked(
        transaction_payload, target_durability);
    lock.lock();
    journal_commit_inflight_ = false;
    if (!persisted.ok) {
        if (persisted.error.side_effect_state ==
            SideEffectState::Unknown) {
            fenced_transaction_digests_[tx_id] = request_digest;
            fenced_transaction_failures_[tx_id] = persisted;
            journal_fenced_ = true;
        }
        return Result<ExceptionReportResult>::Failure(persisted);
    }

    groups_ = std::move(next_groups);
    fingerprint_to_exception_ = std::move(next_fingerprints);
    occurrence_results_ = std::move(next_occurrence_results);
    occurrence_digests_ = std::move(next_occurrence_digests);
    producer_sequence_watermarks_ = std::move(next_watermarks);
    producer_sequence_owners_ = std::move(next_sequence_owners);
    report_digests_[request.report_id] = request_digest;
    report_results_[request.report_id] = report_result;
    ++journal_sequence_;
    observation_sequence_ = next_observation_sequence;
    if (!observation.records.empty()) {
        PendingObservation pending;
        pending.batch_digest =
            secureDigest(batchToJson(observation).dump());
        pending.source_transaction_id = tx_id;
        pending.durability = target_durability;
        pending.batch = std::move(observation);
        pending_observations_.push_back(std::move(pending));
    }
    lock.unlock();
    drainPendingObservations(call);
    return Result<ExceptionReportResult>::Success(
        std::move(report_result));
}


}  // namespace master_agent::exception
