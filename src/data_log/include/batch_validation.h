#pragma once

/**
 * @file batch_validation.h
 * @brief Private batch bounds, privacy, caller, and producer validation helpers.
 *
 * This header is private to Data Log and is not part of the installed API.
 */

#include "master_agent/data_log/data_log_service.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif


namespace master_agent::data_log {
namespace {

// Version 2 stores one complete logical batch in one newline-delimited
// physical frame.  A torn batch therefore has no newline and can be removed
// as an uncommitted tail; recovery never has to guess whether N individually
// committed record frames formed one transaction.
constexpr std::uint32_t kJournalFrameVersion = 3;
constexpr std::size_t kMaxBatchRecords = 256;
constexpr std::size_t kMaxBatchBytes = 1024 * 1024;
constexpr std::size_t kMaxRecordBytes = 64 * 1024;
constexpr std::size_t kMaxPayloadSummaryBytes = 16 * 1024;
constexpr std::size_t kMaxMetadataFieldBytes = 4096;
constexpr std::size_t kMaxPrivacyLabels = 32;

bool hasContiguousSequence(const LogEventBatch& batch) {
    if (batch.records.empty()) {
        return false;
    }
    if (batch.records.size() - 1 >
            std::numeric_limits<std::uint64_t>::max() -
                batch.first_sequence) {
        return false;
    }
    if (batch.first_sequence != batch.records.front().context.producer_sequence ||
        batch.last_sequence != batch.records.back().context.producer_sequence) {
        return false;
    }
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        if (batch.records[i].context.producer_sequence !=
            batch.first_sequence + i) {
            return false;
        }
    }
    return true;
}

bool validBatchIdentity(const LogEventBatch& batch) {
    if (batch.batch_id.empty() || batch.producer_endpoint_id.empty() ||
        batch.producer_epoch == 0 || !hasContiguousSequence(batch)) {
        return false;
    }
    return std::all_of(
        batch.records.begin(), batch.records.end(),
        [&batch](const LogEvent& event) {
            return event.context.producer_endpoint_id ==
                       batch.producer_endpoint_id &&
                   event.context.producer_epoch == batch.producer_epoch;
        });
}

nlohmann::json eventJson(const LogEvent& event);
nlohmann::json auditJson(const AuditRecord& audit,
                         const std::string& previous_hash);

bool validAuditBatchIdentity(const AuditBatch& batch) {
    if (batch.batch_id.empty() || batch.producer_endpoint_id.empty() ||
        batch.producer_epoch == 0 || batch.records.empty() ||
        batch.records.size() - 1 >
            std::numeric_limits<std::uint64_t>::max() -
                batch.first_sequence ||
        batch.first_sequence + batch.records.size() - 1 !=
            batch.last_sequence) {
        return false;
    }
    for (std::size_t i = 0; i < batch.records.size(); ++i) {
        const auto& context = batch.records[i].context;
        if (context.producer_endpoint_id != batch.producer_endpoint_id ||
            context.producer_epoch != batch.producer_epoch ||
            context.producer_sequence != batch.first_sequence + i) {
            return false;
        }
    }
    return true;
}

std::string sequenceKey(const std::string& endpoint,
                        std::uint64_t epoch,
                        std::uint64_t sequence) {
    return endpoint + "|" + std::to_string(epoch) + "|" +
           std::to_string(sequence);
}

std::string producerKey(const std::string& endpoint,
                        std::uint64_t epoch) {
    return endpoint + "|" + std::to_string(epoch);
}

bool validTaskPriority(TaskPriority value) {
    return value == TaskPriority::P0 || value == TaskPriority::P1 ||
           value == TaskPriority::P2;
}

bool validEventSeverity(EventSeverity value) {
    return value == EventSeverity::Debug ||
           value == EventSeverity::Info ||
           value == EventSeverity::Warning ||
           value == EventSeverity::Error ||
           value == EventSeverity::Critical;
}

bool validEventDurability(DurabilityClass value) {
    return value == DurabilityClass::D0Volatile ||
           value == DurabilityClass::D1Buffered ||
           value == DurabilityClass::D2Journaled ||
           value == DurabilityClass::D3Fsynced;
}

bool validAuditDurability(DurabilityClass value) {
    return value == DurabilityClass::D3Fsynced ||
           value == DurabilityClass::D4TamperEvident;
}

bool validSideEffectState(SideEffectState value) {
    return value == SideEffectState::NotApplicable ||
           value == SideEffectState::NotStarted ||
           value == SideEffectState::Committed ||
           value == SideEffectState::ConfirmedNotExecuted ||
           value == SideEffectState::Unknown ||
           value == SideEffectState::Compensated;
}

bool boundedField(const std::string& value) {
    return value.size() <= kMaxMetadataFieldBytes;
}

// Audit fields are stable identifiers/references, never arbitrary prose.
// This deliberately excludes whitespace, quotes, JSON punctuation and all
// non-ASCII bytes so producer-side redaction cannot be bypassed by moving
// user text into a nominal "hash" or policy field.
bool safeAuditToken(const std::string& value,
                    std::size_t max_bytes,
                    bool allow_empty = false) {
    if (value.empty()) return allow_empty;
    if (value.size() > max_bytes) return false;
    return std::all_of(
        value.begin(), value.end(), [](unsigned char byte) {
            return std::isalnum(byte) != 0 || byte == '.' ||
                   byte == '_' || byte == '-' || byte == ':' ||
                   byte == '/' || byte == '|' || byte == '@' ||
                   byte == '+';
        });
}

bool validAuditPrivacyContract(const AuditRecord& audit) {
    static const std::set<std::string> supported_types{
        "Authorization", "Confirmation", "SafetyDecision",
        "SideEffect", "Configuration", "PrivacyAccess",
        "ExceptionLifecycleMutation"};
    const auto safe_optional =
        [](const std::optional<std::string>& value,
           std::size_t limit) {
            return !value ||
                   safeAuditToken(*value, limit);
        };
    const auto safe_vector =
        [](const std::vector<std::string>& values,
           std::size_t max_count, std::size_t max_bytes) {
            return values.size() <= max_count &&
                   std::all_of(
                       values.begin(), values.end(),
                       [max_bytes](const std::string& value) {
                           return safeAuditToken(value, max_bytes);
                       });
        };
    const auto safe_context_optional =
        [](const std::optional<std::string>& value) {
            return !value ||
                   safeAuditToken(*value, 256);
        };
    return supported_types.count(audit.audit_type) != 0 &&
           safeAuditToken(audit.audit_id, 256) &&
           safeAuditToken(audit.context.request_id, 256) &&
           safeAuditToken(audit.context.trace_id, 256) &&
           safeAuditToken(audit.context.span_id, 256, true) &&
           safeAuditToken(
               audit.context.producer_endpoint_id, 256) &&
           safe_context_optional(
               audit.context.causal_parent_event_id) &&
           safe_context_optional(audit.context.session_id) &&
           safe_context_optional(audit.context.plan_id) &&
           safe_context_optional(audit.context.pid) &&
           safe_context_optional(audit.context.activation_id) &&
           safe_context_optional(audit.context.execution_id) &&
           safeAuditToken(audit.actor_id_hash, 256) &&
           safeAuditToken(audit.actor_role, 128) &&
           safe_optional(audit.delegated_by_hash, 256) &&
           safeAuditToken(audit.subject_id_hash, 256) &&
           safeAuditToken(audit.action, 256) &&
           safeAuditToken(audit.interface_name, 256) &&
           safe_optional(audit.capability_id, 256) &&
           safe_vector(audit.object_refs, 32, 256) &&
           safe_vector(audit.object_versions, 32, 256) &&
           (audit.object_versions.empty() ||
            audit.object_versions.size() ==
                audit.object_refs.size()) &&
           safeAuditToken(audit.decision, 128) &&
           safeAuditToken(audit.policy_id, 128) &&
           safeAuditToken(audit.policy_version, 64) &&
           safe_vector(audit.evidence_hashes, 32, 256) &&
           safeAuditToken(
               audit.before_fact_summary, 256, true) &&
           safeAuditToken(
               audit.after_fact_summary, 256, true) &&
           !audit.privacy_labels.empty() &&
           safe_vector(audit.privacy_labels, kMaxPrivacyLabels,
                       128) &&
           safeAuditToken(
               audit.redaction_policy_version, 64) &&
           safeAuditToken(audit.retention_class, 64) &&
           safe_optional(audit.legal_hold_id, 256);
}

Status validateEventBatchBounds(const LogEventBatch& batch) {
    if (batch.records.size() > kMaxBatchRecords ||
        !boundedField(batch.batch_id) ||
        !boundedField(batch.producer_endpoint_id) ||
        batch.checksum.size() > 256 ||
        !safeAuditToken(batch.redaction_proof, 256)) {
        return Status::Error(
            "data_log", "LOG_BATCH_TOO_LARGE",
            "event batch exceeds a configured structural limit");
    }
    std::size_t total_bytes = 0;
    for (const auto& event : batch.records) {
        if (!boundedField(event.event_id) ||
            !boundedField(event.event_type) ||
            !boundedField(event.module) ||
            !boundedField(event.interface_name) ||
            !boundedField(event.operation) ||
            !boundedField(event.outcome) ||
            event.payload_summary_json.size() >
                kMaxPayloadSummaryBytes ||
            event.privacy_labels.size() >
                kMaxPrivacyLabels ||
            std::any_of(
                event.privacy_labels.begin(),
                event.privacy_labels.end(),
                [](const std::string& label) {
                    return !boundedField(label);
                })) {
            return Status::Error(
                "data_log", "LOG_RECORD_TOO_LARGE",
                "event record exceeds a configured structural limit");
        }
        const auto record_bytes = eventJson(event).dump().size();
        if (record_bytes > kMaxRecordBytes ||
            total_bytes > kMaxBatchBytes - record_bytes) {
            return Status::Error(
                "data_log", "LOG_BATCH_TOO_LARGE",
                "serialized event batch exceeds its byte budget");
        }
        total_bytes += record_bytes;
    }
    return Status::Ok();
}

Status validateAuditBatchBounds(const AuditBatch& batch) {
    if (batch.records.size() > kMaxBatchRecords ||
        !boundedField(batch.batch_id) ||
        !boundedField(batch.producer_endpoint_id) ||
        batch.checksum.size() > 256 ||
        !safeAuditToken(batch.redaction_proof, 256)) {
        return Status::Error(
            "data_log", "LOG_BATCH_TOO_LARGE",
            "audit batch exceeds a configured structural limit");
    }
    std::size_t total_bytes = 0;
    for (const auto& audit : batch.records) {
        if (!boundedField(audit.audit_id) ||
            !boundedField(audit.audit_type) ||
            !boundedField(audit.actor_id_hash) ||
            !boundedField(audit.actor_role) ||
            !boundedField(audit.subject_id_hash) ||
            !boundedField(audit.action) ||
            !boundedField(audit.interface_name) ||
            !boundedField(audit.decision) ||
            !boundedField(audit.policy_id) ||
            !boundedField(audit.policy_version) ||
            audit.object_refs.size() > kMaxPrivacyLabels ||
            audit.object_versions.size() > kMaxPrivacyLabels ||
            audit.evidence_hashes.size() > kMaxPrivacyLabels ||
            audit.privacy_labels.size() > kMaxPrivacyLabels ||
            !boundedField(audit.before_fact_summary) ||
            !boundedField(audit.after_fact_summary) ||
            !boundedField(audit.redaction_policy_version) ||
            !boundedField(audit.retention_class)) {
            return Status::Error(
                "data_log", "LOG_RECORD_TOO_LARGE",
                "audit record exceeds a configured structural limit");
        }
        const auto record_bytes = auditJson(audit, "").dump().size();
        if (record_bytes > kMaxRecordBytes ||
            total_bytes > kMaxBatchBytes - record_bytes) {
            return Status::Error(
                "data_log", "LOG_BATCH_TOO_LARGE",
                "serialized audit batch exceeds its byte budget");
        }
        total_bytes += record_bytes;
    }
    return Status::Ok();
}

// Host contract for producer identity.  Normal writers must bind the batch
// to their authenticated endpoint/epoch.  AgentService may forward an
// already-validated producer batch only through its explicit proxy lane;
// the platform IPC layer must replace this host authorization marker with
// credential verification.
bool producerAuthenticated(
    const CallContext& call, const std::string& endpoint,
    std::uint64_t epoch) {
    if (call.caller_endpoint_id.empty() ||
        call.caller_process_epoch == 0) {
        return false;
    }
    if (call.caller_endpoint_id == endpoint &&
        call.caller_process_epoch == epoch) {
        return true;
    }
    return call.caller == CallerModuleId::AgentService &&
           call.authorization_ref ==
               "trusted-observability-proxy";
}

bool authorizedAgentServiceObserver(const CallContext& call) {
    return hasHostModuleIdentity(
               call, CallerModuleId::AgentService) ||
           (call.caller == CallerModuleId::AgentService &&
            call.authorization_ref ==
                "trusted-observability-proxy" &&
            !call.caller_endpoint_id.empty() &&
            call.caller_process_epoch != 0);
}


}  // namespace
}  // namespace master_agent::data_log

