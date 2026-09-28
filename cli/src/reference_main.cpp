#include "master_agent/runtime/reference_runtime.h"
#ifdef MASTER_AGENT_HAS_HTTP
#include "master_agent/runtime/http_model.h"
#endif
#include <iostream>
#include <string>

using namespace master_agent;
using namespace master_agent::reference;
namespace {
void help() {
    std::cout << "sparx — open reference runtime\n"
                 "  sparx version\n"
                 "  sparx demo automotive\n"
                 "  sparx run [--input TEXT] [--endpoint URL] [--model NAME]\n"
                 "            [--session ID] [--request-id ID]\n"
                 "Without --input, reads one request per line until EOF.\n"
                 "Without --endpoint, runs deterministic skills only.\n"
                 "Example skills: set AC to 22 degrees; 把空调调到22度; vehicle status\n"
                 "The automotive tools control in-memory demo state, not a vehicle.\n";
}
Json encode(const Result<Reply>& result) {
    if (!result) {
        const auto e = result.error.value_or(StructuredError{"INTERNAL", "Missing result", "", 500});
        return {{"ok", false}, {"error", {{"code", e.code}, {"message", e.message}}}};
    }
    const auto& r = *result;
    return {{"ok", true}, {"session_id", r.session_id}, {"request_id", r.request_id},
            {"route", r.route}, {"tool", r.tool}, {"output", r.output}, {"replayed", r.replayed}};
}
}
int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") { help(); return 0; }
    const std::string command = argv[1];
    if (command == "version" && argc == 2) { std::cout << SPARX_VERSION << " (open reference runtime)\n"; return 0; }
    const bool demo = command == "demo" && argc == 3 && std::string(argv[2]) == "automotive";
    if (command != "run" && !demo) { std::cerr << "Unsupported command. Use sparx --help.\n"; return 2; }
    std::string input, endpoint, model = "local", session = "cli", request;
    bool single = false;
    if (!demo) for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") { help(); return 0; }
        if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
        const std::string value = argv[++i];
        if (flag == "--input") { input = value; single = true; }
        else if (flag == "--endpoint") endpoint = value;
        else if (flag == "--model") model = value;
        else if (flag == "--session") session = value;
        else if (flag == "--request-id") request = value;
        else { std::cerr << "Unknown option: " << flag << '\n'; return 2; }
    }
    if (!single && !request.empty()) { std::cerr << "--request-id requires --input\n"; return 2; }
    Runtime runtime;
    int temperature = 20;
    const Json empty_schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    const Json temperature_schema{{"type", "object"}, {"properties", {{"temperature", {{"type", "integer"}, {"minimum", 16}, {"maximum", 30}}}}}, {"required", {"temperature"}}, {"additionalProperties", false}};
    if (!runtime.registerTool({"ac.set_temperature", "Set demo AC temperature in Celsius", temperature_schema,
        [&temperature](const Json& args) { temperature = args.at("temperature").get<int>(); return Result<Json>::success({{"temperature", temperature}, {"simulated", true}}); }}) ||
        !runtime.registerTool({"vehicle.status", "Read demo vehicle status", empty_schema,
        [&temperature](const Json&) { return Result<Json>::success({{"temperature", temperature}, {"simulated", true}}); }}) ||
        !runtime.registerSkill("set AC to 22 degrees", "ac.set_temperature", {{"temperature", 22}}) ||
        !runtime.registerSkill("把空调调到22度", "ac.set_temperature", {{"temperature", 22}}) ||
        !runtime.registerSkill("vehicle status", "vehicle.status", Json::object())) {
        std::cerr << "Cannot initialize reference example\n"; return 1;
    }
    if (!endpoint.empty()) {
#ifdef MASTER_AGENT_HAS_HTTP
        if (endpoint.find("://") == std::string::npos) endpoint = "http://" + endpoint;
        if (endpoint.find('/', endpoint.find("://") + 3) == std::string::npos) endpoint += "/v1/chat/completions";
        HttpModelConfig config; config.endpoint = endpoint; config.model = model;
        runtime.setModel(makeHttpModel(std::move(config)));
#else
        std::cerr << "HTTP support is disabled; rebuild with MASTER_AGENT_ENABLE_HTTP=ON\n"; return 2;
#endif
    }
    if (demo) {
        auto set = runtime.run({"demo", "1", "set AC to 22 degrees"});
        auto read = runtime.run({"demo", "2", "vehicle status"});
        std::cout << encode(set).dump() << '\n' << encode(read).dump() << '\n';
        return set && read ? 0 : 1;
    }
    if (single) {
        auto result = runtime.run({session, request.empty() ? "1" : request, input});
        std::cout << encode(result).dump() << '\n'; return result ? 0 : 1;
    }
    unsigned sequence = 0;
    bool success = true;
    while (std::getline(std::cin, input)) {
        auto result = runtime.run({session, std::to_string(++sequence), input});
        std::cout << encode(result).dump() << std::endl;
        success = success && result.ok();
    }
    return success ? 0 : 1;
}
