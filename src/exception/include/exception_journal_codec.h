#pragma once

/**
 * @file exception_journal_codec.h
 * @brief Private exception and evidence journal serialization helpers.
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

Json groupToJson(const ExceptionGroup& group) {
    return Json{{"exception_id", group.exception_id},
                {"fingerprint", group.fingerprint},
                {"version", group.version},
                {"domain", group.domain},
                {"code", group.code},
                {"current_severity",
                 static_cast<std::uint8_t>(group.current_severity)},
                {"aggregate_impact",
                 static_cast<std::uint8_t>(group.aggregate_impact)},
                {"lifecycle",
                 static_cast<std::uint8_t>(group.lifecycle)},
                {"source_module", group.source_module},
                {"source_interface", group.source_interface},
                {"first_seen_at_utc_ms",
                 group.first_seen_at_utc_ms},
                {"last_seen_at_utc_ms", group.last_seen_at_utc_ms},
                {"occurrence_count", group.occurrence_count},
                {"duplicate_replay_count",
                 group.duplicate_replay_count},
                {"current_escalation",
                 static_cast<std::uint8_t>(
                     group.current_escalation)},
                {"bounded_occurrence_ids",
                 group.bounded_occurrence_ids}};
}

ExceptionGroup groupFromJson(const Json& encoded) {
    ExceptionGroup group;
    group.exception_id = encoded.at("exception_id").get<std::string>();
    group.fingerprint = encoded.at("fingerprint").get<std::string>();
    group.version = encoded.at("version").get<std::uint64_t>();
    group.domain = encoded.at("domain").get<std::string>();
    group.code = encoded.at("code").get<std::string>();
    group.current_severity = static_cast<ExceptionSeverity>(
        encoded.at("current_severity").get<std::uint8_t>());
    group.aggregate_impact = static_cast<ExceptionImpact>(
        encoded.at("aggregate_impact").get<std::uint8_t>());
    group.lifecycle = static_cast<ExceptionLifecycle>(
        encoded.at("lifecycle").get<std::uint8_t>());
    group.source_module =
        encoded.at("source_module").get<std::string>();
    group.source_interface =
        encoded.at("source_interface").get<std::string>();
    group.first_seen_at_utc_ms =
        encoded.at("first_seen_at_utc_ms").get<std::int64_t>();
    group.last_seen_at_utc_ms =
        encoded.at("last_seen_at_utc_ms").get<std::int64_t>();
    group.occurrence_count =
        encoded.at("occurrence_count").get<std::uint64_t>();
    group.duplicate_replay_count =
        encoded.at("duplicate_replay_count").get<std::uint64_t>();
    group.current_escalation = static_cast<EscalationKind>(
        encoded.at("current_escalation").get<std::uint8_t>());
    group.bounded_occurrence_ids =
        encoded.at("bounded_occurrence_ids")
            .get<std::vector<std::string>>();
    return group;
}

Json acceptedToJson(const ExceptionAccepted& accepted) {
    return Json{{"occurrence_id", accepted.occurrence_id},
                {"exception_id", accepted.exception_id},
                {"fingerprint", accepted.fingerprint},
                {"disposition",
                 static_cast<std::uint8_t>(accepted.disposition)},
                {"group_version", accepted.group_version},
                {"applied_severity",
                 static_cast<std::uint8_t>(
                     accepted.applied_severity)},
                {"total_count", accepted.total_count},
                {"lifecycle",
                 static_cast<std::uint8_t>(accepted.lifecycle)},
                {"escalation",
                 static_cast<std::uint8_t>(accepted.escalation)},
                {"achieved_durability",
                 static_cast<std::uint8_t>(
                     accepted.achieved_durability)},
                {"durability_ack_id",
                 accepted.durability_ack_id},
                {"fingerprint_policy_version",
                 accepted.fingerprint_policy_version}};
}

ExceptionAccepted acceptedFromJson(const Json& encoded) {
    ExceptionAccepted accepted;
    accepted.occurrence_id =
        encoded.at("occurrence_id").get<std::string>();
    accepted.exception_id =
        encoded.at("exception_id").get<std::string>();
    accepted.fingerprint =
        encoded.at("fingerprint").get<std::string>();
    accepted.disposition = static_cast<ExceptionDisposition>(
        encoded.at("disposition").get<std::uint8_t>());
    accepted.group_version =
        encoded.at("group_version").get<std::uint64_t>();
    accepted.applied_severity = static_cast<ExceptionSeverity>(
        encoded.at("applied_severity").get<std::uint8_t>());
    accepted.total_count =
        encoded.at("total_count").get<std::uint64_t>();
    accepted.lifecycle = static_cast<ExceptionLifecycle>(
        encoded.at("lifecycle").get<std::uint8_t>());
    accepted.escalation = static_cast<EscalationKind>(
        encoded.at("escalation").get<std::uint8_t>());
    accepted.achieved_durability =
        static_cast<data_log::DurabilityClass>(
            encoded.at("achieved_durability")
                .get<std::uint8_t>());
    accepted.durability_ack_id =
        encoded.at("durability_ack_id").get<std::string>();
    accepted.fingerprint_policy_version =
        encoded.at("fingerprint_policy_version")
            .get<std::string>();
    return accepted;
}

Json reportResultToJson(const ExceptionReportResult& result) {
    Json records = Json::array();
    for (const auto& accepted : result.results) {
        records.push_back(acceptedToJson(accepted));
    }
    return Json{{"report_id", result.report_id},
                {"results", std::move(records)},
                {"accepted_count", result.accepted_count},
                {"rejected_count", result.rejected_count},
                {"partial", result.partial}};
}

ExceptionReportResult reportResultFromJson(const Json& encoded) {
    ExceptionReportResult result;
    result.report_id = encoded.at("report_id").get<std::string>();
    for (const auto& item : encoded.at("results")) {
        result.results.push_back(acceptedFromJson(item));
    }
    result.accepted_count =
        encoded.at("accepted_count").get<std::uint32_t>();
    result.rejected_count =
        encoded.at("rejected_count").get<std::uint32_t>();
    result.partial = encoded.at("partial").get<bool>();
    return result;
}

Json observationContextToJson(const ObservationContext& context) {
    Json encoded{{"request_id", context.request_id},
                 {"trace_id", context.trace_id},
                 {"span_id", context.span_id},
                 {"producer_endpoint_id",
                  context.producer_endpoint_id},
                 {"producer_epoch", context.producer_epoch},
                 {"producer_sequence",
                  context.producer_sequence},
                 {"boot_id", context.boot_id},
                 {"task_priority",
                  static_cast<std::uint8_t>(
                      context.task_priority)},
                 {"deadline_mono_ns",
                  context.deadline_mono_ns}};
    encoded["causal_parent_event_id"] =
        context.causal_parent_event_id
            ? Json(*context.causal_parent_event_id)
            : Json(nullptr);
    encoded["session_id"] =
        context.session_id ? Json(*context.session_id) : Json(nullptr);
    encoded["plan_id"] =
        context.plan_id ? Json(*context.plan_id) : Json(nullptr);
    encoded["pid"] =
        context.pid ? Json(*context.pid) : Json(nullptr);
    encoded["activation_id"] =
        context.activation_id ? Json(*context.activation_id)
                              : Json(nullptr);
    encoded["execution_id"] =
        context.execution_id ? Json(*context.execution_id)
                             : Json(nullptr);
    return encoded;
}

ObservationContext observationContextFromJson(const Json& encoded) {
    ObservationContext context;
    context.request_id = encoded.value("request_id", "");
    context.trace_id = encoded.value("trace_id", "");
    context.span_id = encoded.value("span_id", "");
    if (encoded.contains("causal_parent_event_id") &&
        !encoded.at("causal_parent_event_id").is_null()) {
        context.causal_parent_event_id =
            encoded.at("causal_parent_event_id").get<std::string>();
    }
    if (encoded.contains("session_id") &&
        !encoded.at("session_id").is_null()) {
        context.session_id =
            encoded.at("session_id").get<std::string>();
    }
    if (encoded.contains("plan_id") &&
        !encoded.at("plan_id").is_null()) {
        context.plan_id =
            encoded.at("plan_id").get<std::string>();
    }
    if (encoded.contains("pid") && !encoded.at("pid").is_null()) {
        context.pid = encoded.at("pid").get<std::string>();
    }
    if (encoded.contains("activation_id") &&
        !encoded.at("activation_id").is_null()) {
        context.activation_id =
            encoded.at("activation_id").get<std::string>();
    }
    if (encoded.contains("execution_id") &&
        !encoded.at("execution_id").is_null()) {
        context.execution_id =
            encoded.at("execution_id").get<std::string>();
    }
    context.producer_endpoint_id =
        encoded.value("producer_endpoint_id", "");
    context.producer_epoch =
        encoded.value("producer_epoch", std::uint64_t{1});
    context.producer_sequence =
        encoded.value("producer_sequence", std::uint64_t{0});
    context.boot_id = encoded.value("boot_id", std::uint64_t{1});
    context.task_priority = static_cast<TaskPriority>(
        encoded.value("task_priority", std::uint8_t{1}));
    context.deadline_mono_ns =
        encoded.value("deadline_mono_ns", std::int64_t{0});
    return context;
}

Json eventToJson(const data_log::LogEvent& event) {
    Json encoded{{"event_id", event.event_id},
                 {"schema_version", event.schema_version},
                 {"event_type", event.event_type},
                 {"module", event.module},
                 {"interface_name", event.interface_name},
                 {"operation", event.operation},
                 {"context",
                  observationContextToJson(event.context)},
                 {"old_state", event.old_state.value_or("")},
                 {"new_state", event.new_state.value_or("")},
                 {"outcome", event.outcome},
                 {"occurred_at_utc_ms",
                  event.occurred_at_utc_ms},
                 {"occurred_at_mono_ns",
                  event.occurred_at_mono_ns},
                 {"severity",
                  static_cast<std::uint8_t>(event.severity)},
                 {"requested_durability",
                  static_cast<std::uint8_t>(
                      event.requested_durability)},
                 {"privacy_labels", event.privacy_labels},
                 {"payload_summary_json",
                  event.payload_summary_json},
                 {"redaction_policy_version",
                  event.redaction_policy_version}};
    encoded["error_ref"] =
        event.error_ref ? Json(*event.error_ref) : Json(nullptr);
    return encoded;
}

data_log::LogEvent eventFromJson(const Json& encoded) {
    data_log::LogEvent event;
    event.event_id = encoded.at("event_id").get<std::string>();
    event.schema_version =
        encoded.at("schema_version").get<std::uint32_t>();
    event.event_type = encoded.at("event_type").get<std::string>();
    event.module = encoded.at("module").get<std::string>();
    event.interface_name =
        encoded.at("interface_name").get<std::string>();
    event.operation = encoded.at("operation").get<std::string>();
    event.context =
        observationContextFromJson(encoded.at("context"));
    const auto old_state = encoded.value("old_state", "");
    const auto new_state = encoded.value("new_state", "");
    if (!old_state.empty()) event.old_state = old_state;
    if (!new_state.empty()) event.new_state = new_state;
    event.outcome = encoded.at("outcome").get<std::string>();
    if (encoded.contains("error_ref") &&
        !encoded.at("error_ref").is_null()) {
        event.error_ref =
            encoded.at("error_ref").get<std::string>();
    }
    event.occurred_at_utc_ms =
        encoded.at("occurred_at_utc_ms").get<std::int64_t>();
    event.occurred_at_mono_ns =
        encoded.at("occurred_at_mono_ns").get<std::int64_t>();
    event.severity = static_cast<data_log::EventSeverity>(
        encoded.at("severity").get<std::uint8_t>());
    event.requested_durability =
        static_cast<data_log::DurabilityClass>(
            encoded.at("requested_durability")
                .get<std::uint8_t>());
    event.privacy_labels =
        encoded.at("privacy_labels")
            .get<std::vector<std::string>>();
    event.payload_summary_json =
        encoded.at("payload_summary_json").get<std::string>();
    event.redaction_policy_version =
        encoded.at("redaction_policy_version")
            .get<std::string>();
    return event;
}

Json batchToJson(const data_log::LogEventBatch& batch) {
    Json records = Json::array();
    for (const auto& event : batch.records) {
        records.push_back(eventToJson(event));
    }
    return Json{{"batch_id", batch.batch_id},
                {"producer_endpoint_id",
                 batch.producer_endpoint_id},
                {"producer_epoch", batch.producer_epoch},
                {"first_sequence", batch.first_sequence},
                {"last_sequence", batch.last_sequence},
                {"checksum", batch.checksum},
                {"redaction_proof", batch.redaction_proof},
                {"records", std::move(records)}};
}

data_log::LogEventBatch batchFromJson(const Json& encoded) {
    data_log::LogEventBatch batch;
    batch.batch_id = encoded.at("batch_id").get<std::string>();
    batch.producer_endpoint_id =
        encoded.at("producer_endpoint_id").get<std::string>();
    batch.producer_epoch =
        encoded.at("producer_epoch").get<std::uint64_t>();
    batch.first_sequence =
        encoded.at("first_sequence").get<std::uint64_t>();
    batch.last_sequence =
        encoded.at("last_sequence").get<std::uint64_t>();
    batch.checksum = encoded.at("checksum").get<std::string>();
    batch.redaction_proof =
        encoded.at("redaction_proof").get<std::string>();
    for (const auto& item : encoded.at("records")) {
        batch.records.push_back(eventFromJson(item));
    }
    return batch;
}

}  // namespace
}  // namespace master_agent::exception

