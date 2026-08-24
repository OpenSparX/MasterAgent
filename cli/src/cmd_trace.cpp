#include "sparx_commands.h"
#include "sparx_trace.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace sparx {
namespace {

void printUsage() {
    std::cout << R"(
  sparx trace — inspect MasterAgent end-to-end traces.

  Usage:
    sparx trace list <events.jsonl> [--format=text|json]
    sparx trace show <events.jsonl> [--trace <id>|--last]
    sparx trace export <events.jsonl> [--trace <id>|--last] [--output <file>]

  The input may be DataLog's <runtime>/data_log/events.jsonl or a legacy
  Orchestrator TaskEvent JSONL file. Export emits masteragent.trace/v1alpha1.
)";
}

bool parseSize(const std::string& value, std::size_t& result) {
    try { std::size_t used = 0; const auto parsed = std::stoull(value, &used);
          if (used != value.size()) return false; result = parsed; return result > 0; }
    catch (...) { return false; }
}

}  // namespace

int cmd_trace(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printUsage(); return 0;
    }
    const auto& subcommand = args[0];
    if (subcommand != "list" && subcommand != "show" && subcommand != "export") {
        std::cerr << "  unknown trace subcommand: " << subcommand << "\n"; return 1;
    }
    std::string path, output_path;
    std::string format = subcommand == "export" ? "json" : "text";
    TraceFilter filter;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg.rfind("--format=", 0) == 0) format = arg.substr(9);
        else if (arg == "--format" && i + 1 < args.size()) format = args[++i];
        else if (arg == "--trace" && i + 1 < args.size()) filter.trace_id = args[++i];
        else if (arg == "--last") filter.last_trace = true;
        else if (arg == "--plan" && i + 1 < args.size()) filter.plan_id = args[++i];
        else if (arg == "--execution" && i + 1 < args.size()) filter.execution_id = args[++i];
        else if (arg == "--output" && i + 1 < args.size()) output_path = args[++i];
        else if (arg == "--max-records" && i + 1 < args.size()) {
            if (!parseSize(args[++i], filter.max_records)) {
                std::cerr << "  invalid --max-records value\n"; return 1;
            }
        } else if (path.empty()) path = arg;
        else { std::cerr << "  unexpected trace argument: " << arg << "\n"; return 1; }
    }
    if (path.empty()) { std::cerr << "  missing events.jsonl argument\n"; return 1; }
    if (format != "text" && format != "json") {
        std::cerr << "  unknown format '" << format << "'\n"; return 1;
    }
    try {
        const auto loaded = loadTraceRecords(path);
        if (subcommand == "list") {
            std::cout << (format == "json" ? traceListToJson(loaded).dump(2) + "\n"
                                            : traceListToText(loaded));
            return 0;
        }
        const auto selected = filterTraceRecords(loaded, filter);
        if (selected.empty()) { std::cerr << "  no matching trace events\n"; return 1; }
        const auto projected = traceRecordsToJson(selected).dump(2) + "\n";
        if (subcommand == "export" && !output_path.empty()) {
            std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot create output: " + output_path);
            output << projected;
            std::cout << "  exported " << selected.size() << " events to " << output_path << "\n";
        } else if (format == "json" || subcommand == "export") std::cout << projected;
        else std::cout << traceRecordsToText(selected);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "  ✗ cannot read trace: " << error.what() << "\n"; return 1;
    }
}

}  // namespace sparx
