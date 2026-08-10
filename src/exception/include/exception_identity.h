#pragma once

/**
 * @file exception_identity.h
 * @brief Private exception operation digests, transaction identity, and CRC helpers.
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

std::string occurrenceDigest(const ExceptionOccurrence& occurrence) {
    const Json encoded{
        {"occurrence_id", occurrence.occurrence_id},
        {"schema_version", occurrence.schema_version},
        {"producer_endpoint_id", occurrence.producer_endpoint_id},
        {"producer_epoch", occurrence.producer_epoch},
        {"producer_sequence", occurrence.producer_sequence},
        {"domain", occurrence.domain},
        {"code", occurrence.code},
        {"reported_severity",
         static_cast<std::uint8_t>(occurrence.reported_severity)},
        {"impact", static_cast<std::uint8_t>(occurrence.impact)},
        {"source_module", occurrence.source_module},
        {"source_interface", occurrence.source_interface},
        {"operation", occurrence.operation},
        {"object_ref", occurrence.object_ref.value_or("")},
        {"capability_id", occurrence.capability_id.value_or("")},
        {"side_effect_state",
         occurrence.side_effect_state
             ? toString(*occurrence.side_effect_state)
             : ""},
        {"recoverable_hint", occurrence.recoverable_hint},
        {"retryable_hint", occurrence.retryable_hint},
        {"retry_scope_hint", occurrence.retry_scope_hint},
        {"bounded_detail_code", occurrence.bounded_detail_code},
        {"bounded_detail_summary", occurrence.bounded_detail_summary},
        {"evidence_event_ids", occurrence.evidence_event_ids},
        {"evidence_audit_ids", occurrence.evidence_audit_ids},
        {"privacy_labels", occurrence.privacy_labels},
        {"occurred_at_utc_ms", occurrence.occurred_at_utc_ms},
        {"occurred_at_mono_ns", occurrence.occurred_at_mono_ns},
        {"received_at_utc_ms", occurrence.received_at_utc_ms},
        {"context",
         Json{{"request_id", occurrence.context.request_id},
              {"trace_id", occurrence.context.trace_id},
              {"span_id", occurrence.context.span_id},
              {"causal_parent_event_id",
               occurrence.context.causal_parent_event_id.value_or("")},
              {"session_id",
               occurrence.context.session_id.value_or("")},
              {"plan_id", occurrence.context.plan_id.value_or("")},
              {"pid", occurrence.context.pid.value_or("")},
              {"activation_id",
               occurrence.context.activation_id.value_or("")},
              {"execution_id",
               occurrence.context.execution_id.value_or("")},
              {"producer_endpoint_id",
               occurrence.context.producer_endpoint_id},
              {"producer_epoch",
               occurrence.context.producer_epoch},
              {"producer_sequence",
               occurrence.context.producer_sequence},
              {"boot_id", occurrence.context.boot_id},
              {"task_priority",
               static_cast<std::uint8_t>(
                   occurrence.context.task_priority)},
              {"deadline_mono_ns",
               occurrence.context.deadline_mono_ns}}},
        {"redaction_policy_version",
         occurrence.redaction_policy_version},
        {"normalizer_version", occurrence.normalizer_version}};
    return secureDigest(encoded.dump());
}

std::string reportDigest(const ExceptionReportRequest& request) {
    Json encoded{{"report_id", request.report_id},
                 {"batch_checksum", request.batch_checksum},
                 {"source_redaction_proof",
                  request.source_redaction_proof},
                 {"requested_durability",
                  static_cast<std::uint8_t>(
                      request.requested_durability)}};
    encoded["occurrences"] = Json::array();
    for (const auto& occurrence : request.occurrences) {
        encoded["occurrences"].push_back(occurrenceDigest(occurrence));
    }
    return secureDigest(encoded.dump());
}

std::string mutationDigest(const ExceptionMutationRequest& request,
                           ExceptionLifecycle target) {
    return secureDigest(
        Json{{"mutation_id", request.mutation_id},
             {"exception_id", request.exception_id},
             {"expected_group_version",
              request.expected_group_version},
             {"actor_id_hash", request.actor_id_hash},
             {"actor_role", request.actor_role},
             {"reason_code", request.reason_code},
             {"verification_evidence_refs",
              request.verification_evidence_refs},
             {"resolution_waiver_id",
              request.resolution_waiver_id},
             {"target", static_cast<std::uint8_t>(target)}}
            .dump());
}

std::string transactionId(const std::string& kind,
                          const std::string& id,
                          const std::string& digest) {
    return "exception-" + kind + "-" +
           secureDigest(kind + "|" + id + "|" + digest).substr(0, 32);
}

std::uint32_t crc32(const std::string& bytes) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask =
                0U - static_cast<std::uint32_t>(crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

}  // namespace
}  // namespace master_agent::exception

