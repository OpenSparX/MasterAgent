#include "sparx_trace.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sparx {
namespace {

std::string requiredString(const nlohmann::json& value,
                           const char* key) {
    if (!value.contains(key) || !value.at(key).is_string() ||
        value.at(key).get<std::string>().empty()) {
        throw std::runtime_error(std::string("trace event requires non-empty '") +
                                 key + "'");
    }
    return value.at(key).get<std::string>();
}

}  // namespace

TraceRecord traceRecordFromJson(const nlohmann::json& value) {
    if (!value.is_object()) {
        throw std::runtime_error("trace record must be a JSON object");
    }

    TraceRecord record;
    record.event_id = requiredString(value, "event_id");
    record.event_type = requiredString(value, "event_type");
    record.plan_id = requiredString(value, "plan_id");
    record.plan_version = value.at("plan_version").get<std::uint64_t>();
    record.orchestrator_epoch = value.at("orchestrator_epoch").get<std::uint64_t>();
    record.occurred_at_utc_ms = value.at("occurred_at_utc_ms").get<std::int64_t>();
    record.payload_digest = requiredString(value, "payload_digest");
    record.trace_id = requiredString(value, "trace_id");
    if (record.plan_version == 0 || record.orchestrator_epoch == 0) {
        throw std::runtime_error("trace record has invalid version or epoch");
    }

    record.pid = value.value("pid", "");
    record.activation_id = value.value("activation_id", "");
    record.execution_id = value.value("execution_id", "");
    return record;
}

std::vector<TraceRecord> loadTraceRecords(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open trace file: " + path);

    std::ostringstream content;
    content << input.rdbuf();
    const auto text = content.str();
    std::vector<TraceRecord> records;
    if (text.empty()) return records;

    try {
        const auto document = nlohmann::json::parse(text);
        if (document.is_array()) {
            for (const auto& value : document) {
                records.push_back(traceRecordFromJson(value));
            }
            return records;
        }
        records.push_back(traceRecordFromJson(document));
        return records;
    } catch (const nlohmann::json::parse_error&) {
        // JSONL is intentionally supported for append-only runtime traces.
    }

    std::istringstream lines(text);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        try {
            records.push_back(traceRecordFromJson(nlohmann::json::parse(line)));
        } catch (const std::exception& error) {
            throw std::runtime_error("invalid trace record at line " +
                                     std::to_string(line_number) + ": " +
                                     error.what());
        }
    }
    return records;
}

std::vector<TraceRecord> filterTraceRecords(
    const std::vector<TraceRecord>& records, const TraceFilter& filter) {
    std::vector<TraceRecord> filtered;
    filtered.reserve(std::min(records.size(), filter.max_records));
    for (const auto& record : records) {
        if (!filter.plan_id.empty() && record.plan_id != filter.plan_id) continue;
        if (!filter.execution_id.empty() &&
            record.execution_id != filter.execution_id) continue;
        if (filtered.size() == filter.max_records) break;
        filtered.push_back(record);
    }
    return filtered;
}

nlohmann::json traceRecordsToJson(const std::vector<TraceRecord>& records) {
    auto output = nlohmann::json::array();
    for (const auto& record : records) {
        output.push_back({
            {"event_id", record.event_id},
            {"event_type", record.event_type},
            {"plan_id", record.plan_id},
            {"pid", record.pid},
            {"activation_id", record.activation_id},
            {"execution_id", record.execution_id},
            {"plan_version", record.plan_version},
            {"orchestrator_epoch", record.orchestrator_epoch},
            {"occurred_at_utc_ms", record.occurred_at_utc_ms},
            {"payload_digest", record.payload_digest},
            {"trace_id", record.trace_id}});
    }
    return output;
}

std::string traceRecordsToText(const std::vector<TraceRecord>& records) {
    std::ostringstream output;
    output << "  runtime trace · " << records.size() << " event"
           << (records.size() == 1 ? "" : "s") << "\n\n";
    for (const auto& record : records) {
        output << "  " << record.occurred_at_utc_ms << "  "
               << record.event_type;
        if (!record.pid.empty()) output << "  pid=" << record.pid;
        if (!record.execution_id.empty()) {
            output << "  execution=" << record.execution_id;
        }
        output << "  v" << record.plan_version << "\n";
    }
    return output.str();
}

}  // namespace sparx
