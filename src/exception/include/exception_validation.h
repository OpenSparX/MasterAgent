#pragma once

/**
 * @file exception_validation.h
 * @brief Private exception boundary, lifecycle, grouping, and severity validation helpers.
 *
 * This header is private to Exception Management and is not part of the installed API.
 */

#include "master_agent/exception/exception_manager.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
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

namespace master_agent::exception {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kJournalSchemaVersion = 1;
constexpr char kKeySeparator = '\x1f';

// Exception journal metadata is a closed set of stable identifiers and
// references. It must never become a fallback store for free-form user text.
bool safeExceptionReference(const std::string& value,
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

bool validExceptionPrivacyContract(
    const ExceptionOccurrence& occurrence) {
    static const std::set<std::string> allowed_labels{
        "EXCEPTION_METADATA", "SAFETY_METADATA",
        "OPERATIONAL_METADATA", "PSEUDONYMIZED"};
    if (occurrence.privacy_labels.empty() ||
        occurrence.privacy_labels.size() > 8 ||
        !safeExceptionReference(
            occurrence.bounded_detail_summary, 512, true) ||
        !safeExceptionReference(
            occurrence.bounded_detail_code, 256, true) ||
        !safeExceptionReference(
            occurrence.redaction_policy_version, 64) ||
        !safeExceptionReference(
            occurrence.normalizer_version, 64)) {
        return false;
    }
    std::set<std::string> unique;
    return std::all_of(
        occurrence.privacy_labels.begin(),
        occurrence.privacy_labels.end(),
        [&unique](const std::string& label) {
            return allowed_labels.count(label) != 0 &&
                   unique.insert(label).second;
        });
}

Status validateAgentServiceCaller(const CallContext& call) {
    const bool trusted_host =
        hasHostModuleIdentity(call, CallerModuleId::AgentService);
    const bool trusted_observation_lane =
        call.caller == CallerModuleId::AgentService &&
        call.caller_endpoint_id == "agent-service/exception" &&
        call.caller_process_epoch != 0;
    if ((!trusted_host && !trusted_observation_lane) ||
        !isValidTaskPriority(call.priority)) {
        return Status::Error(
            "exception", "EXM_UNAUTHORIZED_SOURCE",
            "persistent ExceptionManager APIs require AgentService");
    }
    return Status::Ok();
}

/**
 * Exception reports may originate from Agent Service or from the
 * Preprocessing Engine dependency explicitly defined by the preprocessing
 * preprocessing design. Lifecycle queries and mutations remain
 * Agent-Service-only.
 */
Status validateExceptionReportCaller(const CallContext& call) {
    if (hasHostModuleIdentity(
            call, CallerModuleId::PreprocessingEngine)) {
        return isValidTaskPriority(call.priority)
                   ? Status::Ok()
                   : Status::Error(
                         "exception",
                         "EXM_UNAUTHORIZED_SOURCE",
                         "exception report priority is invalid");
    }
    return validateAgentServiceCaller(call);
}

bool validSeverity(ExceptionSeverity value) {
    return value == ExceptionSeverity::Info ||
           value == ExceptionSeverity::Warning ||
           value == ExceptionSeverity::Error ||
           value == ExceptionSeverity::Critical;
}

bool validImpact(ExceptionImpact value) {
    return value == ExceptionImpact::None ||
           value == ExceptionImpact::Degraded ||
           value == ExceptionImpact::RequestFailed ||
           value == ExceptionImpact::SafetyAffected;
}

bool validLifecycle(ExceptionLifecycle value) {
    return value == ExceptionLifecycle::Open ||
           value == ExceptionLifecycle::Acknowledged ||
           value == ExceptionLifecycle::Mitigating ||
           value == ExceptionLifecycle::Resolved ||
           value == ExceptionLifecycle::Reopened;
}

// Lifecycle wire values are compatibility identifiers, not an ordering.
// Reopened is intentionally appended to the enum, so every allowed edge is
// stated explicitly instead of inferred through an ordinal comparison.
bool validLifecycleTransition(ExceptionLifecycle from,
                              ExceptionLifecycle to) {
    if (from == to) return true;
    switch (from) {
        case ExceptionLifecycle::Open:
            return to == ExceptionLifecycle::Acknowledged ||
                   to == ExceptionLifecycle::Mitigating ||
                   to == ExceptionLifecycle::Resolved;
        case ExceptionLifecycle::Acknowledged:
            return to == ExceptionLifecycle::Mitigating ||
                   to == ExceptionLifecycle::Resolved;
        case ExceptionLifecycle::Mitigating:
            return to == ExceptionLifecycle::Resolved;
        case ExceptionLifecycle::Resolved:
            // RESOLVED -> REOPENED is driven transactionally by report().
            return false;
        case ExceptionLifecycle::Reopened:
            return to == ExceptionLifecycle::Acknowledged ||
                   to == ExceptionLifecycle::Mitigating ||
                   to == ExceptionLifecycle::Resolved;
    }
    return false;
}

bool validDisposition(ExceptionDisposition value) {
    return value == ExceptionDisposition::NewGroup ||
           value == ExceptionDisposition::Aggregated ||
           value == ExceptionDisposition::DuplicateOccurrence ||
           value == ExceptionDisposition::Reopened ||
           value == ExceptionDisposition::Rejected;
}

bool validEscalation(EscalationKind value) {
    return value == EscalationKind::None ||
           value == EscalationKind::Ops ||
           value == EscalationKind::SafetyCandidate;
}

bool validSideEffect(SideEffectState value) {
    return value == SideEffectState::NotApplicable ||
           value == SideEffectState::NotStarted ||
           value == SideEffectState::Committed ||
           value == SideEffectState::ConfirmedNotExecuted ||
           value == SideEffectState::Unknown ||
           value == SideEffectState::Compensated;
}

bool validPriority(TaskPriority value) {
    return value == TaskPriority::P0 ||
           value == TaskPriority::P1 ||
           value == TaskPriority::P2;
}

bool validEventSeverity(data_log::EventSeverity value) {
    return value == data_log::EventSeverity::Debug ||
           value == data_log::EventSeverity::Info ||
           value == data_log::EventSeverity::Warning ||
           value == data_log::EventSeverity::Error ||
           value == data_log::EventSeverity::Critical;
}

bool validEventDurability(data_log::DurabilityClass value) {
    return value == data_log::DurabilityClass::D0Volatile ||
           value == data_log::DurabilityClass::D1Buffered ||
           value == data_log::DurabilityClass::D2Journaled ||
           value == data_log::DurabilityClass::D3Fsynced;
}

bool validAuditDurability(data_log::DurabilityClass value) {
    return value == data_log::DurabilityClass::D3Fsynced ||
           value == data_log::DurabilityClass::D4TamperEvident;
}

bool validGroupState(const ExceptionGroup& group) {
    return !group.exception_id.empty() &&
           !group.fingerprint.empty() &&
           group.version != 0 && !group.domain.empty() &&
           !group.code.empty() &&
           validSeverity(group.current_severity) &&
           validImpact(group.aggregate_impact) &&
           validLifecycle(group.lifecycle) &&
           validEscalation(group.current_escalation);
}

bool validAcceptedState(const ExceptionAccepted& accepted) {
    return !accepted.occurrence_id.empty() &&
           !accepted.exception_id.empty() &&
           !accepted.fingerprint.empty() &&
           accepted.group_version != 0 &&
           validDisposition(accepted.disposition) &&
           validSeverity(accepted.applied_severity) &&
           validLifecycle(accepted.lifecycle) &&
           validEscalation(accepted.escalation) &&
           (accepted.achieved_durability ==
                data_log::DurabilityClass::D2Journaled ||
            accepted.achieved_durability ==
                data_log::DurabilityClass::D3Fsynced) &&
           !accepted.durability_ack_id.empty();
}

bool validObservationBatch(
    const data_log::LogEventBatch& batch) {
    if (batch.batch_id.empty() ||
        batch.producer_endpoint_id.empty() ||
        batch.producer_epoch == 0 ||
        batch.records.empty() ||
        batch.first_sequence == 0 ||
        batch.last_sequence < batch.first_sequence ||
        batch.last_sequence - batch.first_sequence + 1 !=
            batch.records.size()) {
        return false;
    }
    for (std::size_t index = 0;
         index < batch.records.size(); ++index) {
        const auto& event = batch.records[index];
        if (event.schema_version != 1 ||
            event.event_id.empty() ||
            event.event_type.empty() ||
            !validEventSeverity(event.severity) ||
            !validEventDurability(event.requested_durability) ||
            (event.requested_durability !=
                 data_log::DurabilityClass::D2Journaled &&
             event.requested_durability !=
                 data_log::DurabilityClass::D3Fsynced) ||
            !validPriority(event.context.task_priority) ||
            event.context.producer_endpoint_id !=
                batch.producer_endpoint_id ||
            event.context.producer_epoch !=
                batch.producer_epoch ||
            event.context.producer_sequence !=
                batch.first_sequence + index) {
            return false;
        }
    }
    return true;
}

std::string producerKey(const std::string& endpoint,
                        std::uint64_t epoch) {
    return endpoint + kKeySeparator + std::to_string(epoch);
}

std::string occurrenceKey(const std::string& endpoint,
                          std::uint64_t epoch,
                          const std::string& occurrence_id) {
    return producerKey(endpoint, epoch) + kKeySeparator + occurrence_id;
}

std::string sequenceKey(const std::string& endpoint,
                        std::uint64_t epoch, std::uint64_t sequence) {
    return producerKey(endpoint, epoch) + kKeySeparator +
           std::to_string(sequence);
}

ExceptionSeverity maxSeverity(ExceptionSeverity left,
                              ExceptionSeverity right) {
    return static_cast<std::uint8_t>(left) >
                   static_cast<std::uint8_t>(right)
               ? left
               : right;
}

ExceptionImpact maxImpact(ExceptionImpact left, ExceptionImpact right) {
    return static_cast<std::uint8_t>(left) >
                   static_cast<std::uint8_t>(right)
               ? left
               : right;
}

EscalationKind maxEscalation(EscalationKind left, EscalationKind right) {
    return static_cast<std::uint8_t>(left) >
                   static_cast<std::uint8_t>(right)
               ? left
               : right;
}

}  // namespace
}  // namespace master_agent::exception

