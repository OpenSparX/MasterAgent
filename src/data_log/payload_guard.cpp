/**
 * @file payload_guard.cpp
 * @brief Rejects forbidden raw payloads before durable logging.
 */

#include "include/batch_validation.h"
#include "include/journal_integrity.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_recovery_codec.h"
#include "include/journal_io.h"
#include "include/record_serialization.h"
#include "include/trace_filter.h"

namespace master_agent::data_log {

bool DataLogService::containsForbiddenPayload(
    const std::string& event_type,
    const std::string& payload) {
    if (payload.size() > kMaxPayloadSummaryBytes) return true;
    try {
        bool duplicate_key = false;
        std::vector<std::set<std::string>> object_keys;
        const auto callback =
            [&](int, nlohmann::json::parse_event_t event,
                nlohmann::json& parsed) {
                if (event ==
                    nlohmann::json::parse_event_t::object_start) {
                    object_keys.emplace_back();
                } else if (
                    event == nlohmann::json::parse_event_t::key) {
                    if (object_keys.empty() ||
                        !object_keys.back()
                             .insert(parsed.get<std::string>())
                             .second) {
                        duplicate_key = true;
                    }
                } else if (
                    event ==
                    nlohmann::json::parse_event_t::object_end) {
                    if (!object_keys.empty()) {
                        object_keys.pop_back();
                    }
                }
                return true;
            };
        const auto encoded = nlohmann::json::parse(
            payload, callback, true, false);
        if (duplicate_key) return true;
        if (!encoded.is_object()) return true;
        const auto exactKeys =
            [&encoded](
                std::initializer_list<const char*> allowed) {
                if (encoded.size() != allowed.size()) return false;
                return std::all_of(
                    allowed.begin(), allowed.end(),
                    [&encoded](const char* key) {
                        return encoded.contains(key);
                    });
            };
        const auto safeReference = [](const std::string& value) {
            return !value.empty() &&
                   value.size() <= kMaxMetadataFieldBytes &&
                   std::all_of(
                       value.begin(), value.end(),
                       [](unsigned char byte) {
                           return std::isalnum(byte) != 0 ||
                                  byte == '_' || byte == '-' ||
                                  byte == '.' || byte == ':' ||
                                  byte == '/';
                       });
        };
        const auto safeString =
            [&](const char* key, bool allow_empty = false) {
                if (!encoded.at(key).is_string()) return false;
                const auto& value =
                    encoded.at(key)
                        .get_ref<const std::string&>();
                return (allow_empty && value.empty()) ||
                       safeReference(value);
            };

        // Unknown event types may carry no producer-defined values. This
        // provides a fail-closed default while still allowing metadata-only
        // lifecycle observations.
        if (encoded.empty()) return false;

        if (event_type == "RECOVERY_TEST") {
            return !(exactKeys({"recovered"}) &&
                     encoded.at("recovered").is_boolean());
        }
        if (event_type ==
            "EXCEPTION_OCCURRENCE_ACCEPTED") {
            return !(exactKeys(
                         {"domain", "code", "exception_id",
                          "transaction_id"}) &&
                     safeString("domain") &&
                     safeString("code") &&
                     safeString("exception_id") &&
                     safeString("transaction_id"));
        }
        if (event_type ==
            "EXCEPTION_LIFECYCLE_MUTATED") {
            if (!exactKeys(
                    {"exception_id", "actor_id_hash",
                     "actor_role", "reason_code",
                     "verification_evidence_refs",
                     "resolution_waiver_id",
                     "transaction_id"}) ||
                !safeString("exception_id") ||
                !safeString("actor_id_hash") ||
                !safeString("actor_role") ||
                !safeString("reason_code") ||
                !safeString("resolution_waiver_id", true) ||
                !safeString("transaction_id") ||
                !encoded.at("verification_evidence_refs")
                     .is_array() ||
                encoded.at("verification_evidence_refs")
                        .size() > 32) {
                return true;
            }
            for (const auto& reference :
                 encoded.at(
                     "verification_evidence_refs")) {
                if (!reference.is_string() ||
                    !safeReference(reference.get<std::string>())) {
                    return true;
                }
            }
            return false;
        }

        // The public Trace projection is deliberately metadata-only.  It may
        // carry closed scalar facts and digests, never arbitrary text.  Event
        // names are also closed here so a new producer cannot opt itself into
        // this path merely by choosing safe-looking JSON keys.
        const std::set<std::string> trace_event_types = {
            "TURN_ACCEPTED", "CONFIG_SNAPSHOT_BOUND", "PREPROCESS_COMPLETED",
            "MEMORY_CONTEXT_RECALLED", "INTENT_SUBMITTED",
            "RULE_ROUTE_RESOLVED", "RETRIEVAL_RESOLVED",
            "LOCAL_INFERENCE_OBSERVED", "LOCAL_INFERENCE_SKIPPED",
            "CLOUD_ESCALATION_REQUESTED", "CLOUD_ESCALATION_ALLOWED",
            "CLOUD_ESCALATION_DENIED", "CLOUD_PAYLOAD_SEALED",
            "CLOUD_FALLBACK_COMPLETED", "CLOUD_FALLBACK_FAILED",
            "CLOUD_ARBITRATION_SKIPPED", "CLOUD_INFERENCE_SKIPPED",
            "ATOMIC_EXECUTION_OBSERVED", "SUBAGENT_EXECUTION_OBSERVED",
            "INTENT_DECISION_VALIDATED", "PLAN_COMMITTED",
            "PLAN_OBSERVED", "TURN_COMPLETED", "TURN_FAILED",
            "TURN_PENDING"};
        if (trace_event_types.count(event_type) != 0) {
            // Preserve the frozen v1 AgentService summary while accepting the
            // richer, still value-closed trace projection below.
            if (exactKeys({"content", "request_linked"}) &&
                encoded.at("content").is_string() &&
                encoded.at("content").get<std::string>() == "redacted" &&
                encoded.at("request_linked").is_boolean()) {
                return false;
            }
            const std::set<std::string> top_keys = {
                "stage", "status", "input", "output", "privacy"};
            const std::set<std::string> fact_keys = {
                "type", "length", "digest", "context_blocks",
                "capability_catalog_digest", "decision_type",
                "reason_code", "reply_digest", "plan_id", "node_count",
                "reply_length", "turn_summary", "pending",
                "config_snapshot_id", "job_id", "model_stage", "model_id",
                "runtime", "prompt_digest", "model_output_digest",
                "prompt_tokens", "generated_tokens", "total_latency_ms",
                "payload_digest"};
            const std::set<std::string> execution_fact_keys = {
                "tool_name", "execution_id", "side_effect_state",
                "result_digest", "agent_id"};
            if (!encoded.contains("stage") || !encoded.contains("status") ||
                !safeString("stage") || !safeString("status")) {
                return true;
            }
            for (const auto& item : encoded.items()) {
                if (top_keys.count(item.key()) == 0) return true;
                if (item.key() == "stage" || item.key() == "status" ||
                    item.key() == "privacy") {
                    if (!item.value().is_string() ||
                        (!item.value().get<std::string>().empty() &&
                         !safeReference(item.value().get<std::string>()))) {
                        return true;
                    }
                    continue;
                }
                if (!item.value().is_object()) return true;
                for (const auto& fact : item.value().items()) {
                    if (fact_keys.count(fact.key()) == 0 &&
                        execution_fact_keys.count(fact.key()) == 0) return true;
                    if (fact.value().is_string()) {
                        const auto& value = fact.value().get_ref<const std::string&>();
                        if (!value.empty() && !safeReference(value)) return true;
                    } else if (!fact.value().is_boolean() &&
                               !fact.value().is_number_unsigned() &&
                               !fact.value().is_number_integer()) {
                        return true;
                    }
                }
            }
            return false;
        }

        // AgentService's default summary is deliberately value-closed:
        // accepting an arbitrary "content" string would merely move the
        // plaintext leak behind a safe-looking key.
        return !(exactKeys({"content", "request_linked"}) &&
                 encoded.at("content").is_string() &&
                 encoded.at("content").get<std::string>() ==
                     "redacted" &&
                 encoded.at("request_linked").is_boolean());
    } catch (...) {
        return true;
    }
}

}  // namespace master_agent::data_log
