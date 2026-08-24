#include "sparx_trace.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace sparx {
namespace {

std::string requiredString(const nlohmann::json& value, const char* key) {
    if (!value.contains(key) || !value.at(key).is_string() ||
        value.at(key).get<std::string>().empty()) {
        throw std::runtime_error(std::string("trace event requires non-empty '") + key + "'");
    }
    return value.at(key).get<std::string>();
}

nlohmann::json decodedPayload(const nlohmann::json& value) {
    if (!value.contains("payload_summary")) return nlohmann::json::object();
    if (value.at("payload_summary").is_object()) return value.at("payload_summary");
    if (!value.at("payload_summary").is_string()) return nlohmann::json::object();
    try { return nlohmann::json::parse(value.at("payload_summary").get<std::string>()); }
    catch (...) { return {{"summary", value.at("payload_summary")}}; }
}

void appendDocument(const nlohmann::json& document,
                    std::vector<TraceRecord>& records) {
    if (document.is_array()) {
        for (const auto& value : document) appendDocument(value, records);
        return;
    }
    if (!document.is_object()) throw std::runtime_error("trace record must be a JSON object");
    if (document.value("_journal_kind", std::string{}) == "event_batch") {
        if (!document.contains("records") || !document.at("records").is_array())
            throw std::runtime_error("event_batch requires records array");
        for (const auto& value : document.at("records")) records.push_back(traceRecordFromJson(value));
        return;
    }
    records.push_back(traceRecordFromJson(document));
}

struct TraceSummary {
    std::string trace_id;
    std::string request_id;
    std::string terminal;
    std::int64_t first_utc_ms = 0;
    std::int64_t last_utc_ms = 0;
    std::size_t event_count = 0;
};

std::vector<TraceSummary> summaries(const std::vector<TraceRecord>& records) {
    std::map<std::string, TraceSummary> index;
    for (const auto& record : records) {
        if (record.trace_id.empty()) continue;
        auto& item = index[record.trace_id];
        item.trace_id = record.trace_id;
        if (!record.request_id.empty()) item.request_id = record.request_id;
        if (item.event_count == 0 || record.occurred_at_utc_ms < item.first_utc_ms)
            item.first_utc_ms = record.occurred_at_utc_ms;
        item.last_utc_ms = std::max(item.last_utc_ms, record.occurred_at_utc_ms);
        ++item.event_count;
        if (record.event_type == "TURN_COMPLETED" || record.event_type == "TURN_FAILED" ||
            record.event_type == "TURN_PENDING") item.terminal = record.event_type;
    }
    std::vector<TraceSummary> result;
    for (auto& [_, value] : index) result.push_back(std::move(value));
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.last_utc_ms > b.last_utc_ms;
    });
    return result;
}

}  // namespace

TraceRecord traceRecordFromJson(const nlohmann::json& value) {
    if (!value.is_object()) throw std::runtime_error("trace record must be a JSON object");
    TraceRecord record;
    record.event_id = requiredString(value, "event_id");
    record.event_type = requiredString(value, "event_type");
    record.trace_id = requiredString(value, "trace_id");
    record.module = value.value("module", "orchestrator");
    record.operation = value.value("operation", "");
    record.outcome = value.value("outcome", "");
    record.request_id = value.value("request_id", "");
    record.span_id = value.value("span_id", "");
    record.plan_id = value.value("plan_id", "");
    record.execution_id = value.value("execution_id", "");
    record.error_ref = value.value("error_ref", "");
    record.payload_digest = value.value("payload_digest", "");
    record.occurred_at_utc_ms = value.value("occurred_at_utc_ms", std::int64_t{0});
    record.occurred_at_mono_ns = value.value("occurred_at_mono_ns", std::int64_t{0});
    record.producer_sequence = value.value("producer_sequence", std::uint64_t{0});
    record.payload = decodedPayload(value);
    record.stage = record.payload.value("stage", "");
    record.status = record.payload.value("status", record.outcome);
    return record;
}

std::vector<TraceRecord> loadTraceRecords(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open trace file: " + path);
    std::vector<TraceRecord> records;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try { appendDocument(nlohmann::json::parse(line), records); }
        catch (const std::exception& error) {
            throw std::runtime_error("invalid trace record at line " +
                                     std::to_string(line_number) + ": " + error.what());
        }
    }
    std::stable_sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        if (a.occurred_at_mono_ns != b.occurred_at_mono_ns)
            return a.occurred_at_mono_ns < b.occurred_at_mono_ns;
        return a.producer_sequence < b.producer_sequence;
    });
    return records;
}

std::vector<TraceRecord> filterTraceRecords(const std::vector<TraceRecord>& records,
                                             const TraceFilter& filter) {
    std::string selected_trace = filter.trace_id;
    if (filter.last_trace && selected_trace.empty()) {
        const auto list = summaries(records);
        if (!list.empty()) selected_trace = list.front().trace_id;
    }
    std::vector<TraceRecord> result;
    for (const auto& record : records) {
        if (!selected_trace.empty() && record.trace_id != selected_trace) continue;
        if (!filter.plan_id.empty() && record.plan_id != filter.plan_id) continue;
        if (!filter.execution_id.empty() && record.execution_id != filter.execution_id) continue;
        if (result.size() == filter.max_records) break;
        result.push_back(record);
    }
    return result;
}

nlohmann::json traceRecordsToJson(const std::vector<TraceRecord>& records) {
    nlohmann::json events = nlohmann::json::array();
    for (const auto& r : records) events.push_back({
        {"event_id", r.event_id}, {"event_type", r.event_type}, {"module", r.module},
        {"operation", r.operation}, {"stage", r.stage}, {"status", r.status},
        {"outcome", r.outcome}, {"request_id", r.request_id}, {"trace_id", r.trace_id},
        {"span_id", r.span_id}, {"plan_id", r.plan_id}, {"execution_id", r.execution_id},
        {"occurred_at_utc_ms", r.occurred_at_utc_ms},
        {"occurred_at_mono_ns", r.occurred_at_mono_ns}, {"error_ref", r.error_ref},
        {"payload_digest", r.payload_digest}, {"payload", r.payload}});
    return {{"schema_version", "masteragent.trace/v1alpha1"},
            {"trace_id", records.empty() ? "" : records.front().trace_id},
            {"event_count", records.size()}, {"events", std::move(events)}};
}

nlohmann::json traceListToJson(const std::vector<TraceRecord>& records) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& item : summaries(records)) result.push_back({
        {"trace_id", item.trace_id}, {"request_id", item.request_id},
        {"first_utc_ms", item.first_utc_ms}, {"last_utc_ms", item.last_utc_ms},
        {"event_count", item.event_count}, {"terminal", item.terminal}});
    return result;
}

std::string traceRecordsToText(const std::vector<TraceRecord>& records) {
    std::ostringstream output;
    output << "  MasterAgent trace · " << records.size() << " events";
    if (!records.empty()) output << " · " << records.front().trace_id;
    output << "\n\n";
    for (const auto& r : records) {
        output << "  " << r.occurred_at_utc_ms << "  "
               << (r.stage.empty() ? "-" : r.stage) << "  " << r.event_type;
        if (!r.status.empty()) output << "  [" << r.status << "]";
        if (!r.module.empty()) output << "  module=" << r.module;
        if (!r.error_ref.empty()) output << "  error=" << r.error_ref;
        output << "\n";
    }
    return output.str();
}

std::string traceListToText(const std::vector<TraceRecord>& records) {
    std::ostringstream output;
    const auto list = summaries(records);
    output << "  MasterAgent traces · " << list.size() << "\n\n";
    for (const auto& item : list) output << "  " << item.last_utc_ms << "  "
        << item.trace_id << "  events=" << item.event_count
        << "  " << (item.terminal.empty() ? "INCOMPLETE" : item.terminal) << "\n";
    return output.str();
}

}  // namespace sparx
