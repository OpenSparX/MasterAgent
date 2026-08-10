#pragma once

/**
 * @file journal_integrity.h
 * @brief Private digest, authentication, CRC, and journal-frame integrity helpers.
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

std::string eventBatchDigest(const LogEventBatch& batch) {
    nlohmann::json encoded{
        {"batch_id", batch.batch_id},
        {"producer_endpoint_id", batch.producer_endpoint_id},
        {"producer_epoch", batch.producer_epoch},
        {"first_sequence", batch.first_sequence},
        {"last_sequence", batch.last_sequence},
        {"checksum", batch.checksum},
        {"redaction_proof", batch.redaction_proof}};
    encoded["records"] = nlohmann::json::array();
    for (const auto& record : batch.records) {
        auto item = eventJson(record);
        item["requested_durability"] =
            static_cast<std::uint8_t>(record.requested_durability);
        item["severity"] = static_cast<std::uint8_t>(record.severity);
        item["privacy_labels"] = record.privacy_labels;
        encoded["records"].push_back(std::move(item));
    }
    return secureDigest(encoded.dump());
}

std::string eventRecordDigest(const LogEvent& event) {
    return secureDigest(eventJson(event).dump());
}

std::string auditBatchDigest(const AuditBatch& batch) {
    nlohmann::json encoded{
        {"batch_id", batch.batch_id},
        {"producer_endpoint_id", batch.producer_endpoint_id},
        {"producer_epoch", batch.producer_epoch},
        {"first_sequence", batch.first_sequence},
        {"last_sequence", batch.last_sequence},
        {"checksum", batch.checksum},
        {"redaction_proof", batch.redaction_proof}};
    encoded["records"] = nlohmann::json::array();
    for (const auto& record : batch.records) {
        auto item = auditJson(record, "");
        item["requested_durability"] =
            static_cast<std::uint8_t>(record.requested_durability);
        encoded["records"].push_back(std::move(item));
    }
    return secureDigest(encoded.dump());
}

std::string auditRecordDigest(const AuditRecord& audit) {
    return secureDigest(auditJson(audit, "").dump());
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

std::string hexToBytes(const std::string& hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("invalid digest encoding");
    }
    const auto nibble = [](char value) -> unsigned char {
        if (value >= '0' && value <= '9') {
            return static_cast<unsigned char>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<unsigned char>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<unsigned char>(value - 'A' + 10);
        }
        throw std::runtime_error("invalid digest encoding");
    };
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        bytes.push_back(static_cast<char>(
            (nibble(hex[index]) << 4U) |
            nibble(hex[index + 1])));
    }
    return bytes;
}

// RFC 2104 construction over the project's SHA-256 primitive.
std::string hmacSha256(std::string key,
                       const std::string& message) {
    constexpr std::size_t kBlockSize = 64;
    if (key.size() > kBlockSize) {
        key = hexToBytes(secureDigest(key));
    }
    key.resize(kBlockSize, '\0');
    std::string inner_pad(kBlockSize, '\0');
    std::string outer_pad(kBlockSize, '\0');
    for (std::size_t index = 0; index < kBlockSize; ++index) {
        const auto byte =
            static_cast<unsigned char>(key[index]);
        inner_pad[index] =
            static_cast<char>(byte ^ 0x36U);
        outer_pad[index] =
            static_cast<char>(byte ^ 0x5cU);
    }
    const auto inner =
        hexToBytes(secureDigest(inner_pad + message));
    return secureDigest(outer_pad + inner);
}

std::string anchorAuthenticationCode(
    const std::string& key,
    const AuditAnchorSnapshot& anchor) {
    return hmacSha256(
        key,
        nlohmann::json{
            {"generation", anchor.generation},
            {"hash_chain_head", anchor.hash_chain_head},
            {"key_generation", anchor.key_generation}}
            .dump());
}

void addBatchMetadata(
    nlohmann::json& encoded, const std::string& journal_kind,
    const std::string& batch_id, const std::string& batch_digest,
    const std::string& producer_endpoint_id, std::uint64_t producer_epoch,
    std::uint64_t first_sequence, std::uint64_t last_sequence,
    const std::string& checksum, const std::string& redaction_proof,
    AppendDisposition disposition, std::uint32_t accepted_count,
    std::uint32_t rejected_count, DurabilityClass achieved_durability,
    const std::string& durability_ack_id,
    std::uint32_t batch_frame_count,
    const std::string& key_generation = {},
    std::uint64_t anchor_generation = 0) {
    encoded["_journal_kind"] = journal_kind;
    encoded["_journal_frame_version"] = kJournalFrameVersion;
    encoded["_batch_id"] = batch_id;
    encoded["_batch_digest"] = batch_digest;
    encoded["_batch_producer_endpoint_id"] = producer_endpoint_id;
    encoded["_batch_producer_epoch"] = producer_epoch;
    encoded["_batch_first_sequence"] = first_sequence;
    encoded["_batch_last_sequence"] = last_sequence;
    encoded["_batch_checksum"] = checksum;
    encoded["_batch_redaction_proof"] = redaction_proof;
    encoded["_batch_disposition"] =
        static_cast<std::uint8_t>(disposition);
    encoded["_batch_accepted_count"] = accepted_count;
    encoded["_batch_rejected_count"] = rejected_count;
    encoded["_batch_achieved_durability"] =
        static_cast<std::uint8_t>(achieved_durability);
    encoded["_durability_ack_id"] = durability_ack_id;
    encoded["_batch_frame_count"] = batch_frame_count;
    encoded["_key_generation"] = key_generation;
    encoded["_anchor_generation"] = anchor_generation;
}

constexpr const char* kBatchMetadataKeys[] = {
    "_batch_id",
    "_batch_digest",
    "_batch_producer_endpoint_id",
    "_batch_producer_epoch",
    "_batch_first_sequence",
    "_batch_last_sequence",
    "_batch_checksum",
    "_batch_redaction_proof",
    "_batch_disposition",
    "_batch_accepted_count",
    "_batch_rejected_count",
    "_batch_achieved_durability",
    "_durability_ack_id",
    "_batch_frame_count",
    "_key_generation",
    "_anchor_generation"};

// Expands an Event batch frame into the existing canonical record recovery
// shape.  Batch metadata is authoritative at the outer transaction frame.
nlohmann::json bindEventRecordToBatch(
    nlohmann::json record, const nlohmann::json& batch_frame) {
    record["_journal_kind"] = "event";
    record["_journal_frame_version"] = kJournalFrameVersion;
    for (const auto* key : kBatchMetadataKeys) {
        record[key] = batch_frame.at(key);
    }
    return record;
}

// Audit hashes bind the same batch envelope that is protected by the outer
// frame.  Reject any inner/outer drift before rebuilding the chain.
bool auditRecordMatchesBatch(
    const nlohmann::json& record,
    const nlohmann::json& batch_frame) {
    if (record.value("_journal_kind", std::string{}) != "audit" ||
        record.value("_journal_frame_version", std::uint32_t{0}) !=
            kJournalFrameVersion) {
        return false;
    }
    return std::all_of(
        std::begin(kBatchMetadataKeys),
        std::end(kBatchMetadataKeys),
        [&](const char* key) {
            return record.contains(key) &&
                   record.at(key) == batch_frame.at(key);
        });
}

nlohmann::json finalizeJournalFrame(nlohmann::json encoded) {
    const auto payload = encoded.dump();
    encoded["_frame_length"] = payload.size();
    encoded["_frame_hash"] = secureDigest(payload);
    encoded["_frame_crc32"] = crc32(payload);
    return encoded;
}

nlohmann::json verifyJournalFrame(nlohmann::json encoded) {
    const auto frame_length =
        encoded.at("_frame_length").get<std::uint64_t>();
    const auto frame_hash =
        encoded.at("_frame_hash").get<std::string>();
    const auto frame_crc =
        encoded.at("_frame_crc32").get<std::uint32_t>();
    encoded.erase("_frame_length");
    encoded.erase("_frame_hash");
    encoded.erase("_frame_crc32");
    const auto payload = encoded.dump();
    if (frame_length != payload.size() ||
        frame_hash != secureDigest(payload) ||
        frame_crc != crc32(payload) ||
        encoded.at("_journal_frame_version")
                .get<std::uint32_t>() != kJournalFrameVersion) {
        throw std::runtime_error("journal frame integrity");
    }
    return encoded;
}

// All physical frames of one logical batch must carry exactly the same
// transaction envelope.  The per-frame Record payload is intentionally
// excluded; its own canonical frame SHA/CRC is verified separately.
std::string recoveredBatchEnvelopeDigest(
    const nlohmann::json& encoded) {
    return secureDigest(
        nlohmann::json{
            {"journal_kind", encoded.at("_journal_kind")},
            {"batch_id", encoded.at("_batch_id")},
            {"batch_digest", encoded.at("_batch_digest")},
            {"producer_endpoint_id",
             encoded.at("_batch_producer_endpoint_id")},
            {"producer_epoch", encoded.at("_batch_producer_epoch")},
            {"first_sequence", encoded.at("_batch_first_sequence")},
            {"last_sequence", encoded.at("_batch_last_sequence")},
            {"checksum", encoded.at("_batch_checksum")},
            {"redaction_proof",
             encoded.at("_batch_redaction_proof")},
            {"disposition", encoded.at("_batch_disposition")},
            {"accepted_count",
             encoded.at("_batch_accepted_count")},
            {"rejected_count",
             encoded.at("_batch_rejected_count")},
            {"achieved_durability",
             encoded.at("_batch_achieved_durability")},
            {"durability_ack_id",
             encoded.at("_durability_ack_id")},
            {"frame_count", encoded.at("_batch_frame_count")},
            {"key_generation", encoded.at("_key_generation")},
            {"anchor_generation",
             encoded.at("_anchor_generation")}}
            .dump());
}


}  // namespace
}  // namespace master_agent::data_log

