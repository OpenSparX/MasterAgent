/**
 * @file service_lifecycle.cpp
 * @brief Owns the data-log writer lease, recovery, and initialization lifecycle.
 */

#include "include/batch_validation.h"
#include "include/journal_integrity.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_io.h"
#include "include/record_serialization.h"
#include "include/trace_filter.h"

namespace master_agent::data_log {

DataLogService::DataLogService(
    std::filesystem::path storage_directory,
    std::shared_ptr<IRuntimeClock> clock, std::shared_ptr<IdGenerator> ids,
    DurabilitySync durability_sync,
    std::shared_ptr<ITamperEvidenceProvider> tamper_evidence)
    : storage_directory_(std::move(storage_directory)),
      clock_(std::move(clock)),
      ids_(std::move(ids)),
      durability_sync_(
          durability_sync ? std::move(durability_sync) : syncFilePath),
      tamper_evidence_(std::move(tamper_evidence)) {}

DataLogService::~DataLogService() {
    if (event_stream_.is_open()) event_stream_.close();
    if (audit_stream_.is_open()) audit_stream_.close();
    releaseWriterLease();
}

Status DataLogService::acquireWriterLeaseUnlocked() {
    if (writer_lease_handle_ != -1) return Status::Ok();
    const auto path =
        storage_directory_ / "writer.lock";
#ifdef _WIN32
    const HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Status::Error(
            "data_log", "LOG_WRITER_LEASE_HELD",
            "another process owns the DataLog storage directory", true);
    }
    writer_lease_handle_ =
        reinterpret_cast<std::intptr_t>(handle);
#else
    const int descriptor =
        ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (descriptor < 0 ||
        ::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        if (descriptor >= 0) ::close(descriptor);
        return Status::Error(
            "data_log", "LOG_WRITER_LEASE_HELD",
            "another process owns the DataLog storage directory", true);
    }
    writer_lease_handle_ =
        static_cast<std::intptr_t>(descriptor);
#endif
    return Status::Ok();
}

void DataLogService::releaseWriterLease() noexcept {
    if (writer_lease_handle_ == -1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(
        writer_lease_handle_));
#else
    const int descriptor =
        static_cast<int>(writer_lease_handle_);
    (void)::flock(descriptor, LOCK_UN);
    (void)::close(descriptor);
#endif
    writer_lease_handle_ = -1;
}

Status DataLogService::recoverJournalsUnlocked() {

    events_.clear();
    audits_.clear();
    accepted_batches_.clear();
    event_batch_digests_.clear();
    audit_batch_digests_.clear();
    event_batch_results_.clear();
    audit_batch_results_.clear();
    event_failure_digests_.clear();
    audit_failure_digests_.clear();
    event_batch_failures_.clear();
    audit_batch_failures_.clear();
    event_record_digests_.clear();
    audit_record_digests_.clear();
    ambiguous_event_ids_.clear();
    ambiguous_audit_ids_.clear();
    event_sequence_owners_.clear();
    audit_sequence_owners_.clear();
    event_producer_watermarks_.clear();
    audit_producer_watermarks_.clear();
    event_journal_watermarks_.clear();
    buffered_event_ids_.clear();
    persisted_event_ids_.clear();
    hash_chain_head_ = "GENESIS";
    recovered_d4_anchor_generation_ = 0;
    recovered_d4_anchor_head_ = "GENESIS";
    audit_integrity_degraded_ = false;

    std::vector<std::string> event_lines;
    auto status = readCommittedLines(
        storage_directory_ / "events.jsonl", event_lines);
    if (!status.ok) return status;
    try {
        std::vector<nlohmann::json> logical_event_records;
        for (const auto& line : event_lines) {
            const auto batch_frame =
                verifyJournalFrame(nlohmann::json::parse(line));
            if (batch_frame.at("_journal_kind").get<std::string>() !=
                    "event_batch" ||
                !batch_frame.contains("records") ||
                !batch_frame.at("records").is_array() ||
                batch_frame.at("records").empty() ||
                batch_frame.at("records").size() !=
                    batch_frame.at("_batch_frame_count")
                        .get<std::uint32_t>()) {
                throw std::runtime_error(
                    "event transaction frame");
            }
            for (const auto& record :
                 batch_frame.at("records")) {
                if (!record.is_object()) {
                    throw std::runtime_error(
                        "event record frame");
                }
                logical_event_records.push_back(
                    bindEventRecordToBatch(record, batch_frame));
            }
        }
        std::map<std::string, std::uint32_t> recovered_frame_counts;
        std::map<std::string, std::uint32_t> expected_frame_counts;
        std::map<std::string, std::string> batch_envelopes;
        std::set<std::string> closed_batches;
        std::string active_batch;
        for (const auto& encoded : logical_event_records) {
            if (encoded.at("_journal_kind").get<std::string>() !=
                "event") {
                throw std::runtime_error("event journal kind");
            }
            const auto event = eventFromJson(encoded);
            if (event.schema_version != 1 ||
                event.event_id.empty() || event.event_type.empty() ||
                !validEventSeverity(event.severity) ||
                !validEventDurability(
                    event.requested_durability) ||
                !validTaskPriority(
                    event.context.task_priority)) {
                throw std::runtime_error("event journal schema");
            }
            const auto batch_id =
                encoded.at("_batch_id").get<std::string>();
            const auto batch_digest =
                encoded.at("_batch_digest").get<std::string>();
            const auto batch_endpoint =
                encoded.at("_batch_producer_endpoint_id")
                    .get<std::string>();
            const auto batch_epoch =
                encoded.at("_batch_producer_epoch")
                    .get<std::uint64_t>();
            const auto first_sequence =
                encoded.at("_batch_first_sequence")
                    .get<std::uint64_t>();
            const auto last_sequence =
                encoded.at("_batch_last_sequence")
                    .get<std::uint64_t>();
            const auto frame_count =
                encoded.at("_batch_frame_count")
                    .get<std::uint32_t>();
            if (batch_id.empty() || batch_digest.empty() ||
                batch_endpoint !=
                    event.context.producer_endpoint_id ||
                batch_epoch != event.context.producer_epoch ||
                last_sequence < first_sequence ||
                event.context.producer_sequence < first_sequence ||
                event.context.producer_sequence > last_sequence) {
                throw std::runtime_error("event batch binding");
            }
            if (active_batch != batch_id) {
                if (!active_batch.empty()) {
                    if (recovered_frame_counts.at(active_batch) !=
                        expected_frame_counts.at(active_batch)) {
                        throw std::runtime_error(
                            "partial Event batch before next batch");
                    }
                    closed_batches.insert(active_batch);
                }
                if (closed_batches.count(batch_id) != 0) {
                    throw std::runtime_error(
                        "non-contiguous Event batch frames");
                }
                active_batch = batch_id;
            }
            ++recovered_frame_counts[batch_id];
            const auto expected =
                expected_frame_counts.emplace(batch_id, frame_count);
            if ((!expected.second &&
                 expected.first->second != frame_count) ||
                frame_count == 0) {
                throw std::runtime_error("event batch frame count");
            }
            const auto envelope = recoveredBatchEnvelopeDigest(encoded);
            const auto envelope_entry =
                batch_envelopes.emplace(batch_id, envelope);
            if (!envelope_entry.second &&
                envelope_entry.first->second != envelope) {
                throw std::runtime_error(
                    "event batch envelope conflict");
            }
            const auto record_digest = eventRecordDigest(event);
            if (!event_record_digests_
                     .emplace(event.event_id, record_digest)
                     .second) {
                throw std::runtime_error(
                    "duplicate physical event id");
            }
            const auto sequence = sequenceKey(
                batch_endpoint, batch_epoch,
                event.context.producer_sequence);
            if (!event_sequence_owners_
                     .emplace(sequence, event.event_id)
                     .second) {
                throw std::runtime_error(
                    "duplicate physical event sequence");
            }
            auto& recovered_watermark =
                event_producer_watermarks_[
                    producerKey(batch_endpoint, batch_epoch)];
            if (event.context.producer_sequence <=
                recovered_watermark) {
                throw std::runtime_error(
                    "event producer sequence reordered");
            }
            recovered_watermark =
                event.context.producer_sequence;
            event_journal_watermarks_[
                producerKey(batch_endpoint, batch_epoch)] =
                recovered_watermark;

            LogAppendResult result;
            result.disposition =
                static_cast<AppendDisposition>(
                    encoded.at("_batch_disposition")
                        .get<std::uint8_t>());
            result.batch_id = batch_id;
            result.accepted_first_sequence = first_sequence;
            result.accepted_last_sequence = last_sequence;
            result.accepted_count =
                encoded.at("_batch_accepted_count")
                    .get<std::uint32_t>();
            result.rejected_count =
                encoded.at("_batch_rejected_count")
                    .get<std::uint32_t>();
            result.achieved_durability =
                static_cast<DurabilityClass>(
                    encoded.at("_batch_achieved_durability")
                        .get<std::uint8_t>());
            const auto durability_ack_id =
                encoded.at("_durability_ack_id")
                    .get<std::string>();
            if (!durability_ack_id.empty()) {
                result.durability_ack_id = durability_ack_id;
            }
            if (static_cast<std::uint8_t>(result.disposition) >
                    static_cast<std::uint8_t>(
                        AppendDisposition::Rejected) ||
                (result.disposition != AppendDisposition::Accepted &&
                 result.disposition !=
                     AppendDisposition::PartialAccepted) ||
                (result.achieved_durability !=
                     DurabilityClass::D1Buffered &&
                 result.achieved_durability !=
                     DurabilityClass::D2Journaled &&
                 result.achieved_durability !=
                     DurabilityClass::D3Fsynced) ||
                (result.achieved_durability ==
                         DurabilityClass::D1Buffered
                     ? result.durability_ack_id.has_value()
                     : (!result.durability_ack_id ||
                        result.durability_ack_id->empty()))) {
                throw std::runtime_error("event batch result");
            }
            const auto batch_record_count =
                last_sequence - first_sequence + 1;
            if (batch_record_count !=
                    static_cast<std::uint64_t>(
                        result.accepted_count) +
                        result.rejected_count ||
                frame_count > result.accepted_count) {
                throw std::runtime_error(
                    "event batch cardinality");
            }
            const auto existing =
                event_batch_results_.find(batch_id);
            if (existing == event_batch_results_.end()) {
                event_batch_digests_[batch_id] = batch_digest;
                event_batch_results_[batch_id] = result;
                accepted_batches_.insert(batch_id);
            } else if (event_batch_digests_.at(batch_id) !=
                           batch_digest ||
                       existing->second.accepted_count !=
                           result.accepted_count ||
                       existing->second.rejected_count !=
                           result.rejected_count ||
                       existing->second.achieved_durability !=
                           result.achieved_durability ||
                       existing->second.durability_ack_id !=
                           result.durability_ack_id) {
                throw std::runtime_error(
                    "event batch metadata conflict");
            }
            events_.push_back(event);
            persisted_event_ids_.insert(event.event_id);
        }
        for (const auto& expected : expected_frame_counts) {
            if (recovered_frame_counts.at(expected.first) !=
                expected.second) {
                throw std::runtime_error(
                    "partial committed Event batch");
            }
        }
    } catch (...) {
        return Status::Error(
            "data_log", "LOG_EVENT_JOURNAL_INTEGRITY",
            "committed Event journal frame is corrupt");
    }

    std::vector<std::string> audit_lines;
    status = readCommittedLines(
        storage_directory_ / "audit.jsonl", audit_lines);
    if (!status.ok) return status;
    try {
        std::vector<nlohmann::json> logical_audit_records;
        for (const auto& line : audit_lines) {
            const auto batch_frame =
                verifyJournalFrame(nlohmann::json::parse(line));
            if (batch_frame.at("_journal_kind").get<std::string>() !=
                    "audit_batch" ||
                !batch_frame.contains("records") ||
                !batch_frame.at("records").is_array() ||
                batch_frame.at("records").empty() ||
                batch_frame.at("records").size() !=
                    batch_frame.at("_batch_frame_count")
                        .get<std::uint32_t>()) {
                throw std::runtime_error(
                    "audit transaction frame");
            }
            for (const auto& record :
                 batch_frame.at("records")) {
                if (!record.is_object() ||
                    !auditRecordMatchesBatch(record,
                                             batch_frame)) {
                    throw std::runtime_error(
                        "audit record batch binding");
                }
                logical_audit_records.push_back(record);
            }
        }
        std::map<std::string, std::uint32_t> recovered_frame_counts;
        std::map<std::string, std::uint32_t> expected_frame_counts;
        std::map<std::string, std::string> batch_envelopes;
        std::set<std::string> closed_batches;
        std::string active_batch;
        for (const auto& encoded : logical_audit_records) {
            if (encoded.at("_journal_kind").get<std::string>() !=
                "audit") {
                throw std::runtime_error("audit journal kind");
            }
            const auto achieved_durability =
                static_cast<DurabilityClass>(
                    encoded.at("_batch_achieved_durability")
                        .get<std::uint8_t>());
            const auto key_generation =
                encoded.at("_key_generation")
                    .get<std::string>();
            const auto anchor_generation =
                encoded.at("_anchor_generation")
                    .get<std::uint64_t>();
            const bool d4 =
                achieved_durability ==
                DurabilityClass::D4TamperEvident;
            const auto verification_key =
                tamper_verification_keys_.find(
                    key_generation);
            if (d4 &&
                (verification_key ==
                     tamper_verification_keys_.end() ||
                 key_generation.empty() ||
                 anchor_generation == 0)) {
                throw std::runtime_error(
                    "D4 key or anchor generation unavailable");
            }
            if (!d4 &&
                (!key_generation.empty() ||
                 anchor_generation != 0)) {
                throw std::runtime_error(
                    "D3 frame carries forged D4 metadata");
            }
            const auto previous_hash =
                encoded.at("previous_hash").get<std::string>();
            const auto stored_hash =
                encoded.at("hash").get<std::string>();
            auto hash_material = encoded;
            hash_material.erase("hash");
            const auto expected_hash =
                d4
                    ? hmacSha256(
                          verification_key->second,
                          hash_chain_head_ + "|" +
                              hash_material.dump())
                    : secureDigest(
                          hash_chain_head_ + "|" +
                          hash_material.dump());
            if (previous_hash != hash_chain_head_ ||
                stored_hash != expected_hash) {
                throw std::runtime_error("audit hash chain");
            }
            const auto audit = auditFromJson(encoded);
            if (audit.schema_version != 1 || audit.audit_id.empty() ||
                !validAuditPrivacyContract(audit) ||
                !safeAuditToken(
                    encoded.at("_batch_redaction_proof")
                        .get<std::string>(),
                    256) ||
                !validAuditDurability(
                    audit.requested_durability) ||
                !validSideEffectState(
                    audit.side_effect_state) ||
                !validTaskPriority(
                    audit.context.task_priority)) {
                throw std::runtime_error("audit journal schema");
            }
            const auto batch_id =
                encoded.at("_batch_id").get<std::string>();
            const auto batch_digest =
                encoded.at("_batch_digest").get<std::string>();
            const auto batch_endpoint =
                encoded.at("_batch_producer_endpoint_id")
                    .get<std::string>();
            const auto batch_epoch =
                encoded.at("_batch_producer_epoch")
                    .get<std::uint64_t>();
            const auto first_sequence =
                encoded.at("_batch_first_sequence")
                    .get<std::uint64_t>();
            const auto last_sequence =
                encoded.at("_batch_last_sequence")
                    .get<std::uint64_t>();
            const auto frame_count =
                encoded.at("_batch_frame_count")
                    .get<std::uint32_t>();
            if (batch_id.empty() || batch_digest.empty() ||
                batch_endpoint !=
                    audit.context.producer_endpoint_id ||
                batch_epoch != audit.context.producer_epoch ||
                last_sequence < first_sequence ||
                audit.context.producer_sequence < first_sequence ||
                audit.context.producer_sequence > last_sequence) {
                throw std::runtime_error("audit batch binding");
            }
            if (active_batch != batch_id) {
                if (!active_batch.empty()) {
                    if (recovered_frame_counts.at(active_batch) !=
                        expected_frame_counts.at(active_batch)) {
                        throw std::runtime_error(
                            "partial Audit batch before next batch");
                    }
                    closed_batches.insert(active_batch);
                }
                if (closed_batches.count(batch_id) != 0) {
                    throw std::runtime_error(
                        "non-contiguous Audit batch frames");
                }
                active_batch = batch_id;
            }
            ++recovered_frame_counts[batch_id];
            const auto expected =
                expected_frame_counts.emplace(batch_id, frame_count);
            if ((!expected.second &&
                 expected.first->second != frame_count) ||
                frame_count == 0) {
                throw std::runtime_error("audit batch frame count");
            }
            const auto envelope = recoveredBatchEnvelopeDigest(encoded);
            const auto envelope_entry =
                batch_envelopes.emplace(batch_id, envelope);
            if (!envelope_entry.second &&
                envelope_entry.first->second != envelope) {
                throw std::runtime_error(
                    "audit batch envelope conflict");
            }
            const auto record_digest = auditRecordDigest(audit);
            if (!audit_record_digests_
                     .emplace(audit.audit_id, record_digest)
                     .second) {
                throw std::runtime_error(
                    "duplicate physical audit id");
            }
            const auto sequence = sequenceKey(
                batch_endpoint, batch_epoch,
                audit.context.producer_sequence);
            if (!audit_sequence_owners_
                     .emplace(sequence, audit.audit_id)
                     .second) {
                throw std::runtime_error(
                    "duplicate physical audit sequence");
            }
            auto& recovered_watermark =
                audit_producer_watermarks_[
                    producerKey(batch_endpoint, batch_epoch)];
            if (audit.context.producer_sequence <=
                recovered_watermark) {
                throw std::runtime_error(
                    "audit producer sequence reordered");
            }
            recovered_watermark =
                audit.context.producer_sequence;

            AuditAppendResult result;
            result.disposition =
                static_cast<AppendDisposition>(
                    encoded.at("_batch_disposition")
                        .get<std::uint8_t>());
            result.batch_id = batch_id;
            result.accepted_count =
                encoded.at("_batch_accepted_count")
                    .get<std::uint32_t>();
            result.achieved_durability =
                achieved_durability;
            result.durability_ack_id =
                encoded.at("_durability_ack_id")
                    .get<std::string>();
            result.hash_chain_head = stored_hash;
            result.key_generation = key_generation;
            if (result.disposition != AppendDisposition::Accepted ||
                (result.achieved_durability !=
                     DurabilityClass::D3Fsynced &&
                 result.achieved_durability !=
                     DurabilityClass::D4TamperEvident) ||
                result.durability_ack_id.empty()) {
                throw std::runtime_error("audit batch result");
            }
            const auto batch_record_count =
                last_sequence - first_sequence + 1;
            if (batch_record_count != result.accepted_count ||
                frame_count > result.accepted_count) {
                throw std::runtime_error(
                    "audit batch cardinality");
            }
            const auto existing =
                audit_batch_results_.find(batch_id);
            if (existing == audit_batch_results_.end()) {
                audit_batch_digests_[batch_id] = batch_digest;
                audit_batch_results_[batch_id] = result;
            } else {
                if (audit_batch_digests_.at(batch_id) !=
                        batch_digest ||
                    existing->second.accepted_count !=
                        result.accepted_count ||
                    existing->second.achieved_durability !=
                        result.achieved_durability ||
                    existing->second.durability_ack_id !=
                        result.durability_ack_id ||
                    existing->second.key_generation !=
                        result.key_generation) {
                    throw std::runtime_error(
                        "audit batch metadata conflict");
                }
                existing->second.hash_chain_head = stored_hash;
            }
            audits_.push_back(audit);
            hash_chain_head_ = stored_hash;
            if (d4 &&
                recovered_frame_counts.at(batch_id) ==
                    frame_count) {
                if (anchor_generation <=
                    recovered_d4_anchor_generation_) {
                    throw std::runtime_error(
                        "D4 anchor generation rollback");
                }
                recovered_d4_anchor_generation_ =
                    anchor_generation;
                recovered_d4_anchor_head_ = stored_hash;
            }
        }
        for (const auto& expected : expected_frame_counts) {
            if (recovered_frame_counts.at(expected.first) !=
                expected.second) {
                throw std::runtime_error(
                    "partial committed Audit batch");
            }
        }
    } catch (...) {
        audit_integrity_degraded_ = true;
        return Status::Error(
            "data_log", "LOG_AUDIT_JOURNAL_INTEGRITY",
            "committed Audit journal or hash chain is corrupt", false,
            SideEffectState::Unknown);
    }
    return Status::Ok();
}

Status DataLogService::initialize() {

    TamperKeyMaterial configured_key;
    AuditAnchorSnapshot configured_anchor;
    std::map<std::string, std::string>
        configured_verification_keys;
    if (tamper_evidence_) {
        try {
            const auto key = tamper_evidence_->currentKey();
            const auto verification =
                tamper_evidence_->verificationKeys();
            const auto anchor = tamper_evidence_->loadAnchor();
            if (!key.status.ok || !key.value ||
                key.value->key_generation.empty() ||
                key.value->key_material.empty() ||
                !verification.status.ok ||
                !verification.value ||
                verification.value->empty() ||
                !anchor.status.ok || !anchor.value) {
                return Status::Error(
                    "data_log", "LOG_D4_PROVIDER_UNAVAILABLE",
                    "tamper-evidence key or anchor provider is unavailable",
                    true);
            }
            configured_key = *key.value;
            for (const auto& material : *verification.value) {
                if (material.key_generation.empty() ||
                    material.key_material.empty() ||
                    !configured_verification_keys
                         .emplace(material.key_generation,
                                  material.key_material)
                         .second) {
                    return Status::Error(
                        "data_log", "LOG_D4_PROVIDER_UNAVAILABLE",
                        "tamper-evidence verification key ring is invalid",
                        true);
                }
            }
            const auto current_verification =
                configured_verification_keys.find(
                    configured_key.key_generation);
            if (current_verification ==
                    configured_verification_keys.end() ||
                current_verification->second !=
                    configured_key.key_material) {
                return Status::Error(
                    "data_log", "LOG_D4_PROVIDER_UNAVAILABLE",
                    "current D4 key does not match its verification key ring",
                    true);
            }
            configured_anchor = *anchor.value;
        } catch (...) {
            return Status::Error(
                "data_log", "LOG_D4_PROVIDER_UNAVAILABLE",
                "tamper-evidence provider threw during initialization",
                true);
        }
    }
    std::scoped_lock writer_locks(event_writer_mutex_,
                                  audit_writer_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (ready_) return Status::Ok();
    if (!clock_ || !ids_ || storage_directory_.empty()) {
        return Status::Error("data_log", "LOG_CONFIGURATION_INVALID",
                             "storage, clock and ids are required");
    }
    ready_ = false;
    tamper_key_generation_ =
        configured_key.key_generation;
    tamper_key_material_ =
        configured_key.key_material;
    tamper_verification_keys_ =
        std::move(configured_verification_keys);
    audit_anchor_generation_ =
        configured_anchor.present
            ? configured_anchor.generation
            : 0;
    audit_anchor_head_ =
        configured_anchor.present
            ? configured_anchor.hash_chain_head
            : "GENESIS";
    try {
        if (event_stream_.is_open()) event_stream_.close();
        if (audit_stream_.is_open()) audit_stream_.close();
        event_stream_.clear();
        audit_stream_.clear();
        std::error_code error;
        std::filesystem::create_directories(storage_directory_, error);
        if (error) {
            return Status::Error("data_log", "LOG_STORAGE_UNAVAILABLE",
                                 "failed to create log storage", true);
        }
        const auto writer_lease =
            acquireWriterLeaseUnlocked();
        if (!writer_lease.ok) return writer_lease;
        const auto recovered = recoverJournalsUnlocked();
        if (!recovered.ok) return recovered;
        if (configured_anchor.present) {
            const bool anchor_valid =
                configured_anchor.generation != 0 &&
                tamper_verification_keys_.count(
                    configured_anchor.key_generation) != 0 &&
                !configured_anchor.hash_chain_head.empty() &&
                configured_anchor.authentication_code ==
                    anchorAuthenticationCode(
                        tamper_verification_keys_.at(
                            configured_anchor.key_generation),
                        configured_anchor);
            if (!anchor_valid) {
                audit_integrity_degraded_ = true;
                return Status::Error(
                    "data_log", "LOG_AUDIT_ANCHOR_INTEGRITY",
                    "trusted D4 anchor authentication failed", false,
                    SideEffectState::Unknown);
            }
            if (configured_anchor.generation !=
                    recovered_d4_anchor_generation_ ||
                configured_anchor.hash_chain_head !=
                    recovered_d4_anchor_head_) {
                audit_integrity_degraded_ = true;
                return Status::Error(
                    "data_log", "LOG_AUDIT_ROLLBACK_DETECTED",
                    "D4 journal and trusted anchor are not at the same "
                    "monotonic generation",
                    false, SideEffectState::Unknown);
            }
        } else if (recovered_d4_anchor_generation_ != 0) {
            audit_integrity_degraded_ = true;
            return Status::Error(
                "data_log", "LOG_AUDIT_ANCHOR_MISSING",
                "D4 journal exists without its trusted anchor", false,
                SideEffectState::Unknown);
        }
        event_stream_.open(storage_directory_ / "events.jsonl",
                           std::ios::app | std::ios::binary);
        audit_stream_.open(storage_directory_ / "audit.jsonl",
                           std::ios::app | std::ios::binary);
        if (!event_stream_ || !audit_stream_) {
            return Status::Error("data_log", "LOG_STORAGE_UNAVAILABLE",
                                 "failed to open log journals", true);
        }
    } catch (...) {
        return Status::Error(
            "data_log", "LOG_STORAGE_UNAVAILABLE",
            "unexpected failure during journal recovery", true);
    }
    const auto event_path = storage_directory_ / "events.jsonl";
    const auto audit_path = storage_directory_ / "audit.jsonl";
    std::error_code event_size_error;
    std::error_code audit_size_error;
    const auto event_size =
        std::filesystem::file_size(event_path, event_size_error);
    const auto audit_size =
        std::filesystem::file_size(audit_path, audit_size_error);
    if (event_size_error || audit_size_error) {
        return Status::Error(
            "data_log", "LOG_STORAGE_UNAVAILABLE",
            "cannot inspect recovered journal durability", true);
    }

    // A complete frame may have survived a process crash after an ambiguous
    // fsync result.  Re-sync recovered bytes before restoring a D3/D4 ACK.
    // The state lock is released so health probes from a platform durability
    // adapter cannot deadlock the Ready gate.
    lock.unlock();
    Status recovered_sync = Status::Ok();
    if (event_size != 0) {
        recovered_sync =
            invokeDurabilitySync(durability_sync_, event_path);
    }
    if (recovered_sync.ok && audit_size != 0) {
        recovered_sync =
            invokeDurabilitySync(durability_sync_, audit_path);
    }
    lock.lock();
    if (!recovered_sync.ok) {
        return recovered_sync;
    }
    ready_ = true;
    return Status::Ok();
}

// The writer lane serializes physical event order. State is prepared under
// mutex_, durable I/O runs without that lock, and stable IDs fence ambiguous
// failures from being appended again.

}  // namespace master_agent::data_log
