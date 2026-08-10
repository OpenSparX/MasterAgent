#pragma once

/**
 * @file journal_recovery_codec.h
 * @brief Private recovery deserialization helpers for committed records.
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

TaskPriority priorityFromString(const std::string& value) {
    if (value == "P0") return TaskPriority::P0;
    if (value == "P1") return TaskPriority::P1;
    if (value == "P2") return TaskPriority::P2;
    throw std::runtime_error("invalid priority");
}

SideEffectState sideEffectFromString(const std::string& value) {
    if (value == "NOT_APPLICABLE") {
        return SideEffectState::NotApplicable;
    }
    if (value == "NOT_STARTED") return SideEffectState::NotStarted;
    if (value == "COMMITTED") return SideEffectState::Committed;
    if (value == "CONFIRMED_NOT_EXECUTED") {
        return SideEffectState::ConfirmedNotExecuted;
    }
    if (value == "UNKNOWN") return SideEffectState::Unknown;
    if (value == "COMPENSATED") return SideEffectState::Compensated;
    throw std::runtime_error("invalid side effect");
}

ObservationContext observationContextFromJson(
    const nlohmann::json& encoded) {
    ObservationContext context;
    context.request_id = encoded.value("request_id", "");
    context.trace_id = encoded.value("trace_id", "");
    context.span_id = encoded.value("span_id", "");
    context.producer_endpoint_id =
        encoded.at("producer_endpoint_id").get<std::string>();
    context.producer_epoch =
        encoded.at("producer_epoch").get<std::uint64_t>();
    context.producer_sequence =
        encoded.at("producer_sequence").get<std::uint64_t>();
    context.boot_id =
        encoded.value("boot_id", std::uint64_t{0});
    context.task_priority =
        priorityFromString(encoded.at("priority").get<std::string>());
    context.deadline_mono_ns =
        encoded.value("deadline_mono_ns", std::int64_t{0});
    const auto load_optional =
        [&encoded](const char* key,
                   std::optional<std::string>& destination) {
            if (encoded.contains(key) && encoded.at(key).is_string()) {
                destination = encoded.at(key).get<std::string>();
            }
        };
    load_optional("causal_parent_event_id",
                  context.causal_parent_event_id);
    load_optional("session_id", context.session_id);
    load_optional("plan_id", context.plan_id);
    load_optional("pid", context.pid);
    load_optional("activation_id", context.activation_id);
    load_optional("execution_id", context.execution_id);
    return context;
}

LogEvent eventFromJson(const nlohmann::json& encoded) {
    LogEvent event;
    event.event_id = encoded.at("event_id").get<std::string>();
    event.schema_version =
        encoded.at("schema_version").get<std::uint32_t>();
    event.event_type = encoded.at("event_type").get<std::string>();
    event.module = encoded.value("module", "");
    event.interface_name = encoded.value("interface_name", "");
    event.operation = encoded.value("operation", "");
    event.context = observationContextFromJson(encoded);
    if (encoded.contains("old_state")) {
        event.old_state = encoded.at("old_state").get<std::string>();
    }
    if (encoded.contains("new_state")) {
        event.new_state = encoded.at("new_state").get<std::string>();
    }
    event.outcome = encoded.value("outcome", "");
    if (encoded.contains("error_ref")) {
        event.error_ref = encoded.at("error_ref").get<std::string>();
    }
    event.occurred_at_utc_ms =
        encoded.value("occurred_at_utc_ms", std::int64_t{0});
    event.occurred_at_mono_ns =
        encoded.value("occurred_at_mono_ns", std::int64_t{0});
    event.severity = static_cast<EventSeverity>(
        encoded.at("severity").get<std::uint8_t>());
    event.requested_durability = static_cast<DurabilityClass>(
        encoded.at("requested_durability").get<std::uint8_t>());
    event.privacy_labels =
        encoded.value("privacy_labels", std::vector<std::string>{});
    event.payload_summary_json =
        encoded.value("payload_summary", std::string{"{}"});
    event.redaction_policy_version =
        encoded.value("redaction_policy_version",
                      std::string{"redaction-v1"});
    return event;
}

AuditRecord auditFromJson(const nlohmann::json& encoded) {
    AuditRecord audit;
    audit.audit_id = encoded.at("audit_id").get<std::string>();
    audit.schema_version =
        encoded.at("schema_version").get<std::uint32_t>();
    audit.audit_type = encoded.at("audit_type").get<std::string>();
    audit.context = observationContextFromJson(encoded);
    audit.actor_id_hash = encoded.value("actor_id_hash", "");
    audit.actor_role = encoded.value("actor_role", "");
    if (encoded.contains("delegated_by_hash")) {
        audit.delegated_by_hash =
            encoded.at("delegated_by_hash").get<std::string>();
    }
    audit.subject_id_hash = encoded.value("subject_id_hash", "");
    audit.action = encoded.value("action", "");
    audit.interface_name = encoded.value("interface_name", "");
    if (encoded.contains("capability_id")) {
        audit.capability_id =
            encoded.at("capability_id").get<std::string>();
    }
    audit.object_refs = encoded.value(
        "object_refs", std::vector<std::string>{});
    audit.object_versions = encoded.value(
        "object_versions", std::vector<std::string>{});
    audit.decision = encoded.value("decision", "");
    audit.policy_id = encoded.value("policy_id", "");
    audit.policy_version = encoded.value("policy_version", "");
    audit.evidence_hashes = encoded.value(
        "evidence_hashes", std::vector<std::string>{});
    audit.before_fact_summary =
        encoded.value("before_fact_summary", "");
    audit.after_fact_summary =
        encoded.value("after_fact_summary", "");
    audit.side_effect_state = sideEffectFromString(
        encoded.at("side_effect_state").get<std::string>());
    audit.privacy_labels = encoded.value(
        "privacy_labels", std::vector<std::string>{});
    audit.redaction_policy_version =
        encoded.value("redaction_policy_version", "");
    audit.retention_class =
        encoded.value("retention_class", "");
    if (encoded.contains("legal_hold_id")) {
        audit.legal_hold_id =
            encoded.at("legal_hold_id").get<std::string>();
    }
    audit.occurred_at_utc_ms =
        encoded.value("occurred_at_utc_ms", std::int64_t{0});
    audit.occurred_at_mono_ns =
        encoded.value("occurred_at_mono_ns", std::int64_t{0});
    audit.requested_durability = static_cast<DurabilityClass>(
        encoded.at("requested_durability").get<std::uint8_t>());
    return audit;
}

}  // namespace
}  // namespace master_agent::data_log

