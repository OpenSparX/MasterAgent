#pragma once
/**
 * @file sparx_trace.h
 * @brief DataLog/legacy TaskEvent trace parsing and public projection.
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
    std::string module;
    std::string operation;
    std::string outcome;
    std::string status;
    std::string stage;
    std::string request_id;
    std::string trace_id;
    std::string span_id;
    std::string plan_id;
    std::string execution_id;
    std::string error_ref;
    std::string payload_digest;
    std::int64_t occurred_at_utc_ms = 0;
    std::int64_t occurred_at_mono_ns = 0;
    std::uint64_t producer_sequence = 0;
    nlohmann::json payload = nlohmann::json::object();
};

struct TraceFilter {
    std::string trace_id;
    std::string plan_id;
    std::string execution_id;
    bool last_trace = false;
    std::size_t max_records = 1000;
};

TraceRecord traceRecordFromJson(const nlohmann::json& value);
std::vector<TraceRecord> loadTraceRecords(const std::string& path);
std::vector<TraceRecord> filterTraceRecords(
    const std::vector<TraceRecord>& records, const TraceFilter& filter);
nlohmann::json traceRecordsToJson(const std::vector<TraceRecord>& records);
nlohmann::json traceListToJson(const std::vector<TraceRecord>& records);
std::string traceRecordsToText(const std::vector<TraceRecord>& records);
std::string traceListToText(const std::vector<TraceRecord>& records);

}  // namespace sparx
