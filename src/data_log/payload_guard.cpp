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
