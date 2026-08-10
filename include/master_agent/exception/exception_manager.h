#pragma once

/**
 * @file exception_manager.h
 * @brief Defines exception normalization, reporting, deduplication, and recovery contracts.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "master_agent/common/types.h"
#include "master_agent/data_log/data_log_service.h"

namespace master_agent::exception {

enum class ExceptionSeverity : std::uint8_t {
    Info,
    Warning,
    Error,
    Critical
};

enum class ExceptionImpact : std::uint8_t {
    None,
    Degraded,
    RequestFailed,
    SafetyAffected
};

enum class ExceptionLifecycle : std::uint8_t {
    Open,
    Acknowledged,
    Mitigating,
    Resolved,
    // Appended to preserve the persisted numeric values of states
    // Open..Resolved. A new occurrence after resolution remains explicitly
    // Reopened until an authorized lifecycle mutation advances it.
    Reopened
};

enum class ExceptionDisposition : std::uint8_t {
    NewGroup,
    Aggregated,
    DuplicateOccurrence,
    Reopened,
    Rejected
};

enum class EscalationKind : std::uint8_t {
    None,
    Ops,
    SafetyCandidate
};

struct ExceptionOccurrence {
    std::string occurrence_id;
    std::uint32_t schema_version = 1;
    // Empty means "bind from the authenticated CallContext at ingress".
    // A caller-supplied non-empty identity must match that context exactly.
    std::string producer_endpoint_id;
    std::uint64_t producer_epoch = 0;
    std::uint64_t producer_sequence = 0;
    std::string domain;
    std::string code;
    ExceptionSeverity reported_severity = ExceptionSeverity::Error;
    ExceptionImpact impact = ExceptionImpact::RequestFailed;
    std::string source_module;
    std::string source_interface;
    std::string operation;
    data_log::ObservationContext context;
    std::optional<std::string> object_ref;
    std::optional<std::string> capability_id;
    std::optional<SideEffectState> side_effect_state;
    bool recoverable_hint = false;
    bool retryable_hint = false;
    std::string retry_scope_hint;
    std::string bounded_detail_code;
    std::string bounded_detail_summary;
    std::vector<std::string> evidence_event_ids;
    std::vector<std::string> evidence_audit_ids;
    std::vector<std::string> privacy_labels;
    std::int64_t occurred_at_utc_ms = 0;
    std::int64_t occurred_at_mono_ns = 0;
    std::int64_t received_at_utc_ms = 0;
    std::string redaction_policy_version = "redaction-v1";
    std::string normalizer_version = "exception-normalizer-v1";
};

struct ExceptionReportRequest {
    std::string report_id;
    std::vector<ExceptionOccurrence> occurrences;
    std::string batch_checksum;
    std::string source_redaction_proof;
    data_log::DurabilityClass requested_durability =
        data_log::DurabilityClass::D2Journaled;
};

/// Canonical checksum over the report identity, normalized occurrence
/// digests, requested durability and producer redaction proof.
std::string exceptionBatchChecksum(
    const ExceptionReportRequest& request);

struct ExceptionAccepted {
    std::string occurrence_id;
    std::string exception_id;
    std::string fingerprint;
    ExceptionDisposition disposition = ExceptionDisposition::Rejected;
    std::uint64_t group_version = 0;
    ExceptionSeverity applied_severity = ExceptionSeverity::Error;
    std::uint64_t total_count = 0;
    ExceptionLifecycle lifecycle = ExceptionLifecycle::Open;
    EscalationKind escalation = EscalationKind::None;
    data_log::DurabilityClass achieved_durability =
        data_log::DurabilityClass::D2Journaled;
    std::string durability_ack_id;
    std::string fingerprint_policy_version = "fingerprint-v1";
};

struct ExceptionReportResult {
    std::string report_id;
    std::vector<ExceptionAccepted> results;
    std::uint32_t accepted_count = 0;
    std::uint32_t rejected_count = 0;
    bool partial = false;
};

struct ExceptionGroup {
    std::string exception_id;
    std::string fingerprint;
    std::uint64_t version = 0;
    std::string domain;
    std::string code;
    ExceptionSeverity current_severity = ExceptionSeverity::Error;
    ExceptionImpact aggregate_impact = ExceptionImpact::RequestFailed;
    ExceptionLifecycle lifecycle = ExceptionLifecycle::Open;
    std::string source_module;
    std::string source_interface;
    std::int64_t first_seen_at_utc_ms = 0;
    std::int64_t last_seen_at_utc_ms = 0;
    std::uint64_t occurrence_count = 0;
    std::uint64_t duplicate_replay_count = 0;
    EscalationKind current_escalation = EscalationKind::None;
    std::vector<std::string> bounded_occurrence_ids;
};

struct ExceptionMutationRequest {
    std::string mutation_id;
    std::string exception_id;
    std::uint64_t expected_group_version = 0;
    std::string actor_id_hash;
    std::string actor_role;
    std::string reason_code;
    // RESOLVED requires either bounded verification evidence or an explicit
    // policy waiver. ACKNOWLEDGED never implies resolution.
    std::vector<std::string> verification_evidence_refs;
    std::string resolution_waiver_id;
};

struct ExceptionMutationResult {
    bool changed = false;
    ExceptionGroup group;
};

/**
 * @brief Normalizes, deduplicates, and governs exception lifecycle records.
 *
 * report is idempotent by report and occurrence identity. Lifecycle mutations
 * use optimistic group versions and require authorized evidence for resolution.
 */
class IExceptionManager {
public:
    virtual ~IExceptionManager() = default;

    /**
     * Normalizes and durably records a batch before acknowledging it. Reusing an
     * identity with a different digest is a conflict, not a duplicate success.
     */
    virtual Result<ExceptionReportResult> report(
        const ExceptionReportRequest& request,
        const CallContext& call) = 0;

    /// Returns the authoritative aggregate and its optimistic concurrency version.
    virtual Result<ExceptionGroup> getException(
        const std::string& exception_id, const CallContext& call) const = 0;

    /// Moves an open group to Acknowledged when expected_version still matches.
    virtual Result<ExceptionMutationResult> acknowledge(
        const ExceptionMutationRequest& request,
        const CallContext& call) = 0;

    /// Records active mitigation under optimistic version control.
    virtual Result<ExceptionMutationResult> markMitigating(
        const ExceptionMutationRequest& request,
        const CallContext& call) = 0;

    /// Resolves a group only when the request includes authorized closure evidence.
    virtual Result<ExceptionMutationResult> resolve(
        const ExceptionMutationRequest& request,
        const CallContext& call) = 0;
};

class ExceptionManager final : public IExceptionManager {
public:
    using DurabilitySync =
        std::function<Status(const std::filesystem::path&)>;

    ExceptionManager(std::shared_ptr<IRuntimeClock> clock,
                        std::shared_ptr<IdGenerator> ids,
                        std::shared_ptr<data_log::IDataLogService> log);

    /**
     * Persistent constructor. storage_directory is owned exclusively by the
     * exception module; it must not be the DataLog directory.
     */
    ExceptionManager(
        std::filesystem::path storage_directory,
        std::shared_ptr<IRuntimeClock> clock,
        std::shared_ptr<IdGenerator> ids,
        std::shared_ptr<data_log::IDataLogService> log,
        DurabilitySync durability_sync = {},
        bool auto_initialize = true);
    ~ExceptionManager() override;

    /**
     * Opens the active journal and rebuilds the occurrence/group/mutation
     * ledgers before the ready gate.  Idempotent after a successful call.
     */
    Status initialize();

    Result<ExceptionReportResult> report(
        const ExceptionReportRequest& request,
        const CallContext& call) override;

    Result<ExceptionGroup> getException(
        const std::string& exception_id,
        const CallContext& call) const override;

    Result<ExceptionMutationResult> acknowledge(
        const ExceptionMutationRequest& request,
        const CallContext& call) override;

    Result<ExceptionMutationResult> markMitigating(
        const ExceptionMutationRequest& request,
        const CallContext& call) override;

    Result<ExceptionMutationResult> resolve(
        const ExceptionMutationRequest& request,
        const CallContext& call) override;

private:
    struct PendingObservation {
        data_log::LogEventBatch batch;
        data_log::DurabilityClass durability =
            data_log::DurabilityClass::D2Journaled;
        std::string source_transaction_id;
        std::string batch_digest;
    };

    static std::string fingerprintOf(const ExceptionOccurrence& occurrence);

    static EscalationKind escalationFor(const ExceptionOccurrence& occurrence,
                                        std::uint64_t count);

    Result<ExceptionMutationResult> mutate(
        const ExceptionMutationRequest& request,
        ExceptionLifecycle target, const CallContext& call);

    /// Caller must hold mutex_.
    Status recoverJournalUnlocked();

    Status acquireWriterLeaseUnlocked();
    void releaseWriterLease() noexcept;

    /// Must be called without mutex_.  journal_writer_mutex_ serializes the
    /// physical writer and permits durability callbacks to re-enter read APIs.
    /// The caller owns journal_commit_inflight_ and publishes/fences the
    /// prepared transaction after reacquiring mutex_.
    Status persistTransactionUnlocked(
        const std::string& transaction_payload,
        data_log::DurabilityClass durability);

    /// DataLog is a post-commit projection.  This method never changes the
    /// authoritative exception transaction result.
    void drainPendingObservations(const CallContext& originating_call);

    std::filesystem::path storage_directory_;
    std::filesystem::path active_journal_path_;
    std::shared_ptr<IRuntimeClock> clock_;
    std::shared_ptr<IdGenerator> ids_;
    std::shared_ptr<data_log::IDataLogService> log_;
    DurabilitySync durability_sync_;
    mutable std::mutex mutex_;
    // Physical journal I/O has a lock independent from exception state.
    // Never acquire mutex_ while holding this lock.
    mutable std::mutex journal_writer_mutex_;
    bool ready_ = false;
    bool journal_fenced_ = false;
    Status initialization_status_ =
        Status::Error("exception", "EXM_NOT_INITIALIZED",
                      "exception journal is not initialized", true);
    std::ofstream journal_stream_;
    std::map<std::string, ExceptionGroup> groups_;
    std::map<std::string, std::string> fingerprint_to_exception_;
    // Keyed by producer_endpoint_id + producer_epoch + occurrence_id.
    std::map<std::string, ExceptionAccepted> occurrence_results_;
    std::map<std::string, ExceptionReportResult> report_results_;
    std::map<std::string, ExceptionMutationResult> mutation_results_;
    std::map<std::string, std::string> occurrence_digests_;
    std::map<std::string, std::string> report_digests_;
    std::map<std::string, std::string> mutation_digests_;
    // Producer sequence ownership is independent from occurrence idempotency.
    std::map<std::string, std::uint64_t> producer_sequence_watermarks_;
    std::map<std::string, std::string> producer_sequence_owners_;
    // Stable transaction failures are retained so retry cannot append again
    // after a write/fsync result became ambiguous.
    std::map<std::string, std::string> fenced_transaction_digests_;
    std::map<std::string, Status> fenced_transaction_failures_;
    std::vector<PendingObservation> pending_observations_;
    std::uint64_t journal_sequence_ = 0;
    std::uint64_t observation_sequence_ = 0;
    std::uint64_t writer_epoch_ = 0;
    // Single-writer reservation remains set while durable I/O runs without
    // mutex_.  It prevents a second writer from preparing against stale
    // state without holding the state lock across the DataLog boundary.
    bool journal_commit_inflight_ = false;
    std::intptr_t writer_lease_handle_ = -1;
};

}  // namespace master_agent::exception
