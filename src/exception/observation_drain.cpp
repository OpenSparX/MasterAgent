/**
 * @file observation_drain.cpp
 * @brief Drains deferred log and audit observations after durable state commits.
 */

#include "include/exception_validation.h"
#include "include/exception_identity.h"
#include "include/exception_durability.h"
#include "include/exception_journal_codec.h"
#include "include/exception_journal_codec.h"
#include "include/exception_runtime_policy.h"

namespace master_agent::exception {

void ExceptionManager::drainPendingObservations(
    const CallContext& originating_call) {
    if (!log_ || !clock_) return;
    std::vector<PendingObservation> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = pending_observations_;
    }
    for (const auto& item : pending) {
        // This is an independent post-commit outbox delivery, not a child
        // operation whose deadline may be extended.  It receives a new
        // bounded internal deadline while preserving causal request/trace.
        CallContext log_call{
            CallerModuleId::ExceptionManager,
            originating_call.request_id.empty()
                ? "exception-observation"
                : originating_call.request_id,
            originating_call.trace_id.empty()
                ? "exception-observation"
                : originating_call.trace_id,
            "exception-manager", originating_call.priority,
            clock_->monotonicNowNs() + 1'000'000'000LL,
            item.batch.producer_endpoint_id,
            item.batch.producer_epoch,
            "internal-observability"};
        bool projection_committed = false;
        try {
            const bool lifecycle_audit =
                item.batch.records.size() == 1 &&
                item.batch.records.front().event_type ==
                    "EXCEPTION_LIFECYCLE_MUTATED";
            if (lifecycle_audit) {
                const auto& event = item.batch.records.front();
                Json detail = Json::object();
                try {
                    detail = Json::parse(
                        event.payload_summary_json);
                } catch (...) {
                    detail = Json::object();
                }
                data_log::AuditRecord audit;
                audit.audit_id =
                    "exception-mutation-audit-" +
                    secureDigest(item.source_transaction_id)
                        .substr(0, 32);
                audit.audit_type =
                    "ExceptionLifecycleMutation";
                audit.context = event.context;
                audit.actor_id_hash =
                    detail.value("actor_id_hash", "");
                audit.actor_role =
                    detail.value("actor_role", "");
                const auto exception_id =
                    detail.value("exception_id", "");
                audit.subject_id_hash =
                    exception_id.empty()
                        ? std::string{}
                        : secureDigest(exception_id);
                audit.action = event.operation;
                audit.interface_name = event.interface_name;
                audit.decision = event.outcome;
                audit.policy_id = "exception-lifecycle";
                audit.policy_version = "contract-v2";
                audit.side_effect_state =
                    SideEffectState::NotApplicable;
                audit.occurred_at_utc_ms =
                    event.occurred_at_utc_ms;
                audit.occurred_at_mono_ns =
                    event.occurred_at_mono_ns;
                audit.requested_durability =
                    data_log::DurabilityClass::D3Fsynced;
                data_log::AuditBatch audit_batch;
                audit_batch.batch_id = item.batch.batch_id;
                audit_batch.producer_endpoint_id =
                    item.batch.producer_endpoint_id;
                audit_batch.producer_epoch =
                    item.batch.producer_epoch;
                audit_batch.first_sequence =
                    item.batch.first_sequence;
                audit_batch.last_sequence =
                    item.batch.last_sequence;
                audit_batch.checksum = item.batch.checksum;
                audit_batch.redaction_proof =
                    item.batch.redaction_proof;
                audit_batch.records = {std::move(audit)};
                const auto appended =
                    log_->appendAudit(audit_batch, log_call);
                const bool disposition_ok =
                    appended.status.ok && appended.value &&
                    (appended.value->disposition ==
                         data_log::AppendDisposition::Accepted ||
                     appended.value->disposition ==
                         data_log::AppendDisposition::Duplicate);
                const bool durability_ok =
                    appended.value &&
                    validAuditDurability(
                        appended.value->achieved_durability) &&
                    static_cast<std::uint8_t>(
                        appended.value->achieved_durability) >=
                        static_cast<std::uint8_t>(
                            item.durability);
                projection_committed =
                    disposition_ok &&
                    appended.value->batch_id ==
                        audit_batch.batch_id &&
                    appended.value->accepted_count ==
                        audit_batch.records.size() &&
                    durability_ok &&
                    !appended.value->durability_ack_id.empty() &&
                    !appended.value->hash_chain_head.empty();
            } else {
                const auto appended =
                    log_->appendEvents(item.batch, log_call);
                const bool disposition_ok =
                    appended.status.ok && appended.value &&
                    (appended.value->disposition ==
                         data_log::AppendDisposition::Accepted ||
                     appended.value->disposition ==
                         data_log::AppendDisposition::Duplicate);
                const bool durability_ok =
                    appended.value &&
                    validEventDurability(
                        appended.value->achieved_durability) &&
                    static_cast<std::uint8_t>(
                        appended.value->achieved_durability) >=
                        static_cast<std::uint8_t>(
                            item.durability);
                projection_committed =
                    disposition_ok &&
                    appended.value->batch_id ==
                        item.batch.batch_id &&
                    appended.value->accepted_count ==
                        item.batch.records.size() &&
                    appended.value->rejected_count == 0 &&
                    appended.value->accepted_first_sequence ==
                        item.batch.first_sequence &&
                    appended.value->accepted_last_sequence ==
                        item.batch.last_sequence &&
                    durability_ok &&
                    appended.value->durability_ack_id &&
                    !appended.value->durability_ack_id->empty();
            }
        } catch (...) {
            continue;
        }
        if (!projection_committed) {
            continue;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        auto current = std::find_if(
            pending_observations_.begin(),
            pending_observations_.end(),
            [&](const PendingObservation& candidate) {
                return candidate.source_transaction_id ==
                           item.source_transaction_id &&
                       candidate.batch.batch_id ==
                           item.batch.batch_id;
            });
        if (current == pending_observations_.end()) {
            continue;
        }
        if (journal_fenced_ || journal_commit_inflight_) {
            break;
        }
        const auto ack_transaction_id =
            item.source_transaction_id + ":observation-ack";
        const Json ack{
            {"kind", "observation_ack"},
            {"transaction_id", ack_transaction_id},
            {"idempotency_key", item.batch.batch_id},
            {"request_digest", item.batch_digest},
            {"journal_sequence", journal_sequence_ + 1},
            {"writer_epoch", writer_epoch_},
            {"durability",
             static_cast<std::uint8_t>(item.durability)},
            {"committed_at_utc_ms", clock_->utcNowMs()},
            {"source_transaction_id",
             item.source_transaction_id},
            {"batch_id", item.batch.batch_id},
            {"batch_digest", item.batch_digest}};
        const auto ack_payload = ack.dump();
        journal_commit_inflight_ = true;
        lock.unlock();
        const auto acknowledged = persistTransactionUnlocked(
            ack_payload, item.durability);
        lock.lock();
        journal_commit_inflight_ = false;
        if (!acknowledged.ok) {
            // The business transaction is already committed.  Keep the
            // observation pending and fence new writers until recovery
            // decides whether this ACK frame exists.
            if (acknowledged.error.side_effect_state ==
                SideEffectState::Unknown) {
                fenced_transaction_digests_[ack_transaction_id] =
                    item.batch_digest;
                fenced_transaction_failures_[ack_transaction_id] =
                    acknowledged;
                journal_fenced_ = true;
            }
            break;
        }
        ++journal_sequence_;
        current = std::find_if(
            pending_observations_.begin(),
            pending_observations_.end(),
            [&](const PendingObservation& candidate) {
                return candidate.source_transaction_id ==
                           item.source_transaction_id &&
                       candidate.batch.batch_id ==
                           item.batch.batch_id;
            });
        if (current == pending_observations_.end()) {
            // Reservation prevents a second writer from removing this item.
            // Treat a violated invariant as a fence instead of silently
            // publishing an inconsistent journal sequence.
            journal_fenced_ = true;
            break;
        }
        pending_observations_.erase(current);
    }
}

}  // namespace master_agent::exception
