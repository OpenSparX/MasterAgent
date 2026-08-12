#pragma once
/**
 * @file sparx_trace.h
 * @brief Runtime TaskEvent JSONL parsing and terminal/JSON rendering.
 */

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sparx {

struct TraceRecord {
    std::string event_id;
    std::string event_type;
    std::string plan_id;
    std::string pid;
    std::string activation_id;
    std::string execution_id;
    std::uint64_t plan_version = 0;
    std::uint64_t orchestrator_epoch = 0;
    std::int64_t occurred_at_utc_ms = 0;
    std::string payload_digest;
    std::string trace_id;
};

struct TraceFilter {
    std::string plan_id;
    std::string execution_id;
    std::size_t max_records = 1000;
};

/// Parses one TaskEvent JSON object and rejects incomplete runtime records.
TraceRecord traceRecordFromJson(const nlohmann::json& value);

/// Loads JSONL (one event per line) or a JSON array from disk.
std::vector<TraceRecord> loadTraceRecords(const std::string& path);

/// Applies stable selectors and preserves source order.
std::vector<TraceRecord> filterTraceRecords(
    const std::vector<TraceRecord>& records, const TraceFilter& filter);

nlohmann::json traceRecordsToJson(const std::vector<TraceRecord>& records);
std::string traceRecordsToText(const std::vector<TraceRecord>& records);

}  // namespace sparx
