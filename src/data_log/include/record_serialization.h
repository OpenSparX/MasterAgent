#pragma once

/**
 * @file record_serialization.h
 * @brief Private event and audit JSON serialization helpers.
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

nlohmann::json eventJson(const LogEvent& event) {
    nlohmann::json encoded{
        {"event_id", event.event_id},
        {"schema_version", event.schema_version},
        {"event_type", event.event_type},
        {"module", event.module},
        {"interface_name", event.interface_name},
        {"operation", event.operation},
        {"request_id", event.context.request_id},
        {"trace_id", event.context.trace_id},
        {"span_id", event.context.span_id},
        {"producer_endpoint_id", event.context.producer_endpoint_id},
        {"producer_epoch", event.context.producer_epoch},
        {"producer_sequence", event.context.producer_sequence},
        {"boot_id", event.context.boot_id},
        {"priority", toString(event.context.task_priority)},
        {"deadline_mono_ns", event.context.deadline_mono_ns},
        {"outcome", event.outcome},
        {"occurred_at_utc_ms", event.occurred_at_utc_ms},
        {"occurred_at_mono_ns", event.occurred_at_mono_ns},
        {"severity", static_cast<std::uint8_t>(event.severity)},
        {"requested_durability",
         static_cast<std::uint8_t>(event.requested_durability)},
        {"privacy_labels", event.privacy_labels},
        {"payload_summary", event.payload_summary_json},
        {"redaction_policy_version", event.redaction_policy_version}};
    if (event.context.causal_parent_event_id) {
        encoded["causal_parent_event_id"] =
            *event.context.causal_parent_event_id;
    }
    if (event.context.session_id) {
        encoded["session_id"] = *event.context.session_id;
    }
    if (event.context.plan_id) {
        encoded["plan_id"] = *event.context.plan_id;
    }
    if (event.context.pid) encoded["pid"] = *event.context.pid;
    if (event.context.activation_id) {
        encoded["activation_id"] = *event.context.activation_id;
    }
    if (event.context.execution_id) {
        encoded["execution_id"] = *event.context.execution_id;
    }
    if (event.old_state) encoded["old_state"] = *event.old_state;
    if (event.new_state) encoded["new_state"] = *event.new_state;
    if (event.error_ref) encoded["error_ref"] = *event.error_ref;
    return encoded;
}

nlohmann::json auditJson(const AuditRecord& audit,
                         const std::string& previous_hash) {
    nlohmann::json encoded{
        {"audit_id", audit.audit_id},
        {"schema_version", audit.schema_version},
        {"audit_type", audit.audit_type},
        {"request_id", audit.context.request_id},
        {"trace_id", audit.context.trace_id},
        {"span_id", audit.context.span_id},
        {"producer_endpoint_id", audit.context.producer_endpoint_id},
        {"producer_epoch", audit.context.producer_epoch},
        {"producer_sequence", audit.context.producer_sequence},
        {"boot_id", audit.context.boot_id},
        {"priority", toString(audit.context.task_priority)},
        {"deadline_mono_ns", audit.context.deadline_mono_ns},
        {"actor_id_hash", audit.actor_id_hash},
        {"actor_role", audit.actor_role},
        {"subject_id_hash", audit.subject_id_hash},
        {"action", audit.action},
        {"interface_name", audit.interface_name},
        {"object_refs", audit.object_refs},
        {"object_versions", audit.object_versions},
        {"decision", audit.decision},
        {"policy_id", audit.policy_id},
        {"policy_version", audit.policy_version},
        {"evidence_hashes", audit.evidence_hashes},
        {"before_fact_summary", audit.before_fact_summary},
        {"after_fact_summary", audit.after_fact_summary},
        {"side_effect_state", toString(audit.side_effect_state)},
        {"privacy_labels", audit.privacy_labels},
        {"redaction_policy_version",
         audit.redaction_policy_version},
        {"retention_class", audit.retention_class},
        {"occurred_at_utc_ms", audit.occurred_at_utc_ms},
        {"occurred_at_mono_ns", audit.occurred_at_mono_ns},
        {"requested_durability",
         static_cast<std::uint8_t>(audit.requested_durability)},
        {"previous_hash", previous_hash}};
    if (audit.delegated_by_hash) {
        encoded["delegated_by_hash"] =
            *audit.delegated_by_hash;
    }
    if (audit.capability_id) {
        encoded["capability_id"] = *audit.capability_id;
    }
    if (audit.legal_hold_id) {
        encoded["legal_hold_id"] = *audit.legal_hold_id;
    }
    if (audit.context.causal_parent_event_id) {
        encoded["causal_parent_event_id"] =
            *audit.context.causal_parent_event_id;
    }
    if (audit.context.session_id) {
        encoded["session_id"] = *audit.context.session_id;
    }
    if (audit.context.plan_id) {
        encoded["plan_id"] = *audit.context.plan_id;
    }
    if (audit.context.pid) encoded["pid"] = *audit.context.pid;
    if (audit.context.activation_id) {
        encoded["activation_id"] = *audit.context.activation_id;
    }
    if (audit.context.execution_id) {
        encoded["execution_id"] = *audit.context.execution_id;
    }
    return encoded;
}

}  // namespace
}  // namespace master_agent::data_log

