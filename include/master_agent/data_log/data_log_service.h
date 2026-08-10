#pragma once

/**
 * @file data_log_service.h
 * @brief Defines structured event logging, durability, redaction, and trace-query contracts.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "master_agent/common/types.h"

namespace master_agent::data_log {

using ObservationContext =
    ::master_agent::ObservationContext;

enum class EventSeverity : std::uint8_t {
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

enum class DurabilityClass : std::uint8_t {
    D0Volatile,
    D1Buffered,
    D2Journaled,
    D3Fsynced,
    D4TamperEvident
};

enum class AppendDisposition : std::uint8_t {
    Accepted,
    Duplicate,
    PartialAccepted,
    Backpressured,
    Rejected
};

struct LogEvent {
    std::string event_id;
    std::uint32_t schema_version = 1;
    std::string event_type;
    std::string module;
    std::string interface_name;
    std::string operation;
    ObservationContext context;
    std::optional<std::string> old_state;
    std::optional<std::string> new_state;
    std::string outcome;
    std::optional<std::string> error_ref;
    std::int64_t occurred_at_utc_ms = 0;
    std::int64_t occurred_at_mono_ns = 0;
    EventSeverity severity = EventSeverity::Info;
    DurabilityClass requested_durability = DurabilityClass::D1Buffered;
    std::vector<std::string> privacy_labels;
    std::string payload_summary_json = "{}";
    std::string redaction_policy_version = "redaction-v1";
};

struct LogEventBatch {
    std::string batch_id;
    std::string producer_endpoint_id;
    std::uint64_t producer_epoch = 1;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::string checksum;
    std::string redaction_proof = "producer-redacted";
    std::vector<LogEvent> records;
};

struct AuditRecord {
    std::string audit_id;
    std::uint32_t schema_version = 1;
    std::string audit_type;
    ObservationContext context;
    std::string actor_id_hash;
    std::string actor_role;
    std::optional<std::string> delegated_by_hash;
    std::string subject_id_hash;
    std::string action;
    std::string interface_name;
    std::optional<std::string> capability_id;
    std::vector<std::string> object_refs;
    std::vector<std::string> object_versions;
    std::string decision;
    std::string policy_id;
    std::string policy_version;
    std::vector<std::string> evidence_hashes;
    std::string before_fact_summary;
    std::string after_fact_summary;
    SideEffectState side_effect_state =
        SideEffectState::NotApplicable;
    std::vector<std::string> privacy_labels = {"AUDIT_METADATA"};
    std::string redaction_policy_version = "redaction-v1";
    std::string retention_class = "AUDIT_STANDARD";
    std::optional<std::string> legal_hold_id;
    std::int64_t occurred_at_utc_ms = 0;
    std::int64_t occurred_at_mono_ns = 0;
    DurabilityClass requested_durability = DurabilityClass::D3Fsynced;
};

struct AuditBatch {
    std::string batch_id;
    std::string producer_endpoint_id;
    std::uint64_t producer_epoch = 1;
    std::uint64_t first_sequence = 0;
    std::uint64_t last_sequence = 0;
    std::string checksum;
    std::string redaction_proof = "producer-redacted";
    std::vector<AuditRecord> records;
};

struct LogAppendResult {
    AppendDisposition disposition = AppendDisposition::Rejected;
    std::string batch_id;
    std::uint64_t accepted_first_sequence = 0;
    std::uint64_t accepted_last_sequence = 0;
    std::uint32_t accepted_count = 0;
    std::uint32_t rejected_count = 0;
    DurabilityClass achieved_durability = DurabilityClass::D0Volatile;
    std::optional<std::string> durability_ack_id;
    std::uint64_t retry_after_ms = 0;
    std::string policy_version = "log-policy-v1";
};

struct AuditAppendResult {
    AppendDisposition disposition = AppendDisposition::Rejected;
    std::string batch_id;
    std::uint32_t accepted_count = 0;
    DurabilityClass achieved_durability = DurabilityClass::D3Fsynced;
    std::string durability_ack_id;
    std::string hash_chain_head;
    std::string key_generation;
};

struct TamperKeyMaterial {
    std::string key_generation;
    std::string key_material;
};

/// Monotonic trusted witness for the latest D4 commit.
struct AuditAnchorSnapshot {
    bool present = false;
    std::uint64_t generation = 0;
    std::string hash_chain_head;
    std::string key_generation;
    std::string authentication_code;
};

/// Platform seam for TPM/HSM/remote witness integration. The reference runtime uses an
/// in-memory fault-injection implementation; DataLog never invents a D4 key.
class ITamperEvidenceProvider {
public:
    virtual ~ITamperEvidenceProvider() = default;
    virtual Result<TamperKeyMaterial> currentKey() = 0;
    virtual Result<std::vector<TamperKeyMaterial>>
    verificationKeys() = 0;
    virtual Result<AuditAnchorSnapshot> loadAnchor() = 0;
    virtual Status commitAnchor(
        const AuditAnchorSnapshot& anchor) = 0;
};

struct TraceQuery {
    std::optional<std::string> trace_id;
    std::optional<std::string> request_id;
    std::optional<std::string> plan_id;
    std::optional<std::string> execution_id;
    std::size_t max_records = 100;
};

struct TracePage {
    std::vector<LogEvent> events;
    bool complete_for_requested_range = true;
};

struct LogHealth {
    bool ready = false;
    std::size_t buffered_events = 0;
    std::size_t persisted_events = 0;
    std::size_t audit_records = 0;
    std::size_t emergency_ring_records = 0;
    std::string hash_chain_head;
    bool d4_ready = false;
    bool audit_integrity_degraded = false;
    std::string active_key_generation;
    std::uint64_t audit_anchor_generation = 0;
};

/**
 * @brief Persists redacted events and audit records with explicit durability.
 *
 * Producers must maintain a strictly increasing sequence within each endpoint
 * epoch. Append results report achieved durability and must not be inferred from
 * the absence of an error alone.
 */
class IDataLogService {
public:
    virtual ~IDataLogService() = default;

    /**
     * Appends a producer-ordered, idempotent event batch. The result reports the
     * durability actually achieved; callers must not infer a stronger level.
     */
    virtual Result<LogAppendResult> appendEvents(
        const LogEventBatch& batch, const CallContext& call) = 0;

    /**
     * Appends audit records to the authenticated hash chain. A D4 result is
     * returned only after the external tamper-evidence anchor is committed.
     */
    virtual Result<AuditAppendResult> appendAudit(
        const AuditBatch& batch, const CallContext& call) = 0;

    /// Queries a bounded trace projection; at least one stable selector is required.
    virtual Result<TracePage> queryTrace(
        const TraceQuery& query, const CallContext& call) const = 0;

    /**
     * Flushes locally buffered records. Success does not imply D4 anchoring unless
     * an append result explicitly reported that durability class.
     */
    virtual Status flush(const CallContext& call) = 0;

    virtual LogHealth getHealth(const CallContext& call) const = 0;
};

class DataLogService final : public IDataLogService {
public:
    using DurabilitySync =
        std::function<Status(const std::filesystem::path&)>;

    DataLogService(std::filesystem::path storage_directory,
                      std::shared_ptr<IRuntimeClock> clock,
                      std::shared_ptr<IdGenerator> ids,
                      DurabilitySync durability_sync = {},
                      std::shared_ptr<ITamperEvidenceProvider>
                          tamper_evidence = {});
    ~DataLogService() override;

    Status initialize();

    Result<LogAppendResult> appendEvents(
        const LogEventBatch& batch, const CallContext& call) override;

    Result<AuditAppendResult> appendAudit(
        const AuditBatch& batch, const CallContext& call) override;

    Result<TracePage> queryTrace(
        const TraceQuery& query, const CallContext& call) const override;

    Status flush(const CallContext& call) override;

    LogHealth getHealth(const CallContext& call) const override;

    void appendEmergencySummary(const std::string& summary);

private:
    /// Caller must hold mutex_.
    void appendEmergencySummaryUnlocked(const std::string& summary);

    static bool containsForbiddenPayload(
        const std::string& event_type,
        const std::string& payload);

    /// Rebuilds trace, RecordID/BatchID/producer-sequence indexes and the
    /// Audit chain head before Ready. Caller holds both writer lanes and the
    /// state mutex; no public traffic is admitted during recovery.
    Status recoverJournalsUnlocked();

    /// Holds an OS-enforced single-writer lease for the storage directory.
    Status acquireWriterLeaseUnlocked();
    void releaseWriterLease() noexcept;

    std::filesystem::path storage_directory_;
    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    DurabilitySync durability_sync_;
    std::shared_ptr<ITamperEvidenceProvider> tamper_evidence_;
    std::string tamper_key_generation_;
    std::string tamper_key_material_;
    std::map<std::string, std::string>
        tamper_verification_keys_;
    std::uint64_t audit_anchor_generation_ = 0;
    std::string audit_anchor_head_ = "GENESIS";
    std::uint64_t recovered_d4_anchor_generation_ = 0;
    std::string recovered_d4_anchor_head_ = "GENESIS";
    mutable std::mutex mutex_;
    // Event and Audit use independent single-writer lanes. The registry lock
    // is released before write/flush/fsync, so queries and health never wait
    // on storage latency.
    mutable std::mutex event_writer_mutex_;
    mutable std::mutex audit_writer_mutex_;
    bool ready_ = false;
    std::vector<LogEvent> events_;
    std::vector<AuditRecord> audits_;
    std::set<std::string> accepted_batches_;
    std::map<std::string, std::string> event_batch_digests_;
    std::map<std::string, std::string> audit_batch_digests_;
    std::map<std::string, LogAppendResult> event_batch_results_;
    std::map<std::string, AuditAppendResult> audit_batch_results_;
    // A write/fsync failure can be ambiguous: bytes may already be visible
    // even though no durability acknowledgement was returned. Retaining the
    // failure by stable ID prevents an in-process retry from appending again.
    std::map<std::string, std::string> event_failure_digests_;
    std::map<std::string, std::string> audit_failure_digests_;
    std::map<std::string, Status> event_batch_failures_;
    std::map<std::string, Status> audit_batch_failures_;
    std::map<std::string, std::string> event_record_digests_;
    std::map<std::string, std::string> audit_record_digests_;
    std::set<std::string> ambiguous_event_ids_;
    std::set<std::string> ambiguous_audit_ids_;
    std::map<std::string, std::string> event_sequence_owners_;
    std::map<std::string, std::string> audit_sequence_owners_;
    std::map<std::string, std::uint64_t> event_producer_watermarks_;
    std::map<std::string, std::uint64_t> audit_producer_watermarks_;
    // Physical Event journal order can lag the logical watermark while D0
    // records are memory-only.  Backfill must never move this order backward.
    std::map<std::string, std::uint64_t> event_journal_watermarks_;
    // D1 frames written to the process stream but not yet flushed are
    // reported separately from D2/D3/recovered durable records.
    std::set<std::string> buffered_event_ids_;
    std::set<std::string> persisted_event_ids_;
    std::vector<std::string> emergency_ring_;
    std::string hash_chain_head_ = "GENESIS";
    bool audit_integrity_degraded_ = false;
    std::intptr_t writer_lease_handle_ = -1;
    std::ofstream event_stream_;
    std::ofstream audit_stream_;
};

}  // namespace master_agent::data_log
