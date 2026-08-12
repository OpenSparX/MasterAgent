#include "sparx_commands.h"
#include "sparx_trace.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace sparx {
namespace {

void printUsage() {
    std::cout << R"(
  sparx trace — inspect runtime TaskEvent records.

  Usage:
    sparx trace show <trace.jsonl>       Render a trace for the terminal
    sparx trace export <trace.jsonl>     Export a trace as JSON

  Options:
    --format=text|json                  Output format (default: text)
    --plan <plan-id>                    Include only events for a plan
    --execution <execution-id>          Include only one execution
    --max-records <n>                   Bound output (default: 1000)

  Input is an append-only JSONL file produced from orchestrator TaskEvents.
  A JSON array is also accepted for tooling and fixtures.

  Example:
    sparx trace show .sparx/trace.jsonl --plan plan-123
    sparx trace export .sparx/trace.jsonl --format=json

)" << std::endl;
}

bool parseSize(const std::string& value, std::size_t& result) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (consumed != value.size()) return false;
        result = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int cmd_trace(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return 0;
    }

    const auto& subcommand = args[0];
    if (subcommand != "show" && subcommand != "export") {
        std::cerr << "  unknown trace subcommand: " << subcommand << "\n"
                  << "  try: sparx trace show <trace.jsonl>\n";
        return 1;
    }

    std::string path;
    std::string format = subcommand == "export" ? "json" : "text";
    TraceFilter filter;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg.rfind("--format=", 0) == 0) {
            format = arg.substr(9);
        } else if (arg == "--format" && i + 1 < args.size()) {
            format = args[++i];
        } else if (arg == "--plan" && i + 1 < args.size()) {
            filter.plan_id = args[++i];
        } else if (arg == "--execution" && i + 1 < args.size()) {
            filter.execution_id = args[++i];
        } else if (arg == "--max-records" && i + 1 < args.size()) {
            if (!parseSize(args[++i], filter.max_records)) {
                std::cerr << "  invalid --max-records value\n";
                return 1;
            }
        } else if (path.empty()) {
            path = arg;
        } else {
            std::cerr << "  unexpected trace argument: " << arg << "\n";
            return 1;
        }
    }

    if (path.empty()) {
        std::cerr << "  missing trace file argument\n"
                  << "  usage: sparx trace " << subcommand
                  << " <trace.jsonl>\n";
        return 1;
    }
    if (format != "text" && format != "json") {
        std::cerr << "  unknown format '" << format
                  << "' (supported: text, json)\n";
        return 1;
    }

    try {
        const auto records = filterTraceRecords(loadTraceRecords(path), filter);
        if (format == "json") {
            std::cout << traceRecordsToJson(records).dump(2) << "\n";
        } else {
            std::cout << traceRecordsToText(records);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "  ✗ cannot read trace: " << error.what() << "\n";
        return 1;
    }
}

}  // namespace sparx
