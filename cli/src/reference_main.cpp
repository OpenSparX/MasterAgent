#include "master_agent/runtime/reference_runtime.h"
#ifdef MASTER_AGENT_HAS_HTTP
#include "master_agent/runtime/http_model.h"
#endif
#include <iostream>
#include <charconv>
#include <cstdlib>
#include <string>

using namespace master_agent;
using namespace master_agent::reference;
namespace {
void help() {
    std::cout << "sparx — open reference runtime\n"
                 "  sparx version\n"
                 "  sparx demo automotive\n"
                 "  sparx run [--input TEXT] [--endpoint URL] [--model NAME]\n"
                 "            [--session ID] [--request-id ID] [--state FILE] [--jsonl]\n"
                 "            [--timeout-ms N] [--api-key-env NAME]\n"
                 "  sparx recover --state FILE\n"
                 "  sparx reconcile --state FILE --session ID --request-id ID\n"
                 "       --outcome committed|failed --note EVIDENCE [--output JSON]\n"
                 "Durable runs require --request-id with --input, or explicit IDs in JSONL.\n"
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
    if (command != "run" && command != "recover" && command != "reconcile" && !demo) { std::cerr << "Unsupported command. Use sparx --help.\n"; return 2; }
    std::string input, endpoint, model = "local", session = "cli", request;
    std::string state, outcome, note, output = "null", key_env;
    int timeout_ms = 30000;
    bool single = false, jsonl = false;
    std::vector<std::string> options;
    if (!demo) for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help") { help(); return 0; }
        options.push_back(flag);
        if (flag == "--jsonl") { jsonl = true; continue; }
        if (i + 1 >= argc) { std::cerr << "Missing option value\n"; return 2; }
        const std::string value = argv[++i];
        if (flag == "--input") { input = value; single = true; }
        else if (flag == "--endpoint") endpoint = value;
        else if (flag == "--model") model = value;
        else if (flag == "--session") session = value;
        else if (flag == "--request-id") request = value;
        else if (flag == "--state") {
            if (value.empty()) { std::cerr << "--state must not be empty\n"; return 2; }
            state = value;
        }
        else if (flag == "--outcome") outcome = value;
        else if (flag == "--note") note = value;
        else if (flag == "--output") output = value;
        else if (flag == "--api-key-env") key_env = value;
        else if (flag == "--timeout-ms") {
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), timeout_ms);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || timeout_ms <= 0 || timeout_ms > 300000) {
                std::cerr << "--timeout-ms must be 1..300000\n"; return 2;
            }
        }
        else { std::cerr << "Unknown option: " << flag << '\n'; return 2; }
    }
    if (command == "run" && !single && !request.empty()) { std::cerr << "--request-id requires --input\n"; return 2; }
    for (const auto& flag : options) {
        const bool recovery_flag = flag == "--state" || (command == "reconcile" &&
            (flag == "--session" || flag == "--request-id" || flag == "--outcome" || flag == "--note" || flag == "--output"));
        if (((command == "recover" || command == "reconcile") && !recovery_flag) ||
            (command == "run" && (flag == "--outcome" || flag == "--note" || flag == "--output"))) {
            std::cerr << "Option does not apply to this command: " << flag << '\n'; return 2;
        }
    }
    if ((jsonl && single) || (command == "run" && !state.empty() && !jsonl && (!single || request.empty()))) {
        std::cerr << "Use --state with --input and --request-id, or --jsonl with explicit IDs\n"; return 2;
    }
    if ((command == "recover" || command == "reconcile") && state.empty()) { std::cerr << "--state is required\n"; return 2; }
    Runtime runtime;
    if (!state.empty()) {
        auto status = runtime.openStore(state);
        if (!status) { std::cout << encode(Result<Reply>::failure({status.error_code, status.error_message, "", 500})).dump() << '\n'; return 1; }
    }
    if (command == "recover") {
        auto records = runtime.unresolved();
        if (!records) return 1;
        Json pending = Json::array();
        for (const auto& item : *records) pending.push_back({{"session_id", item.turn.session_id}, {"request_id", item.turn.request_id},
            {"input", item.turn.input}, {"tool", item.tool}, {"arguments", item.arguments}, {"state", "UNKNOWN"}});
        std::cout << Json{{"ok", true}, {"unresolved", pending}}.dump() << '\n'; return 0;
    }
    if (command == "reconcile") {
        auto parsed = Json::parse(output, nullptr, false);
        if (request.empty() || session.empty() || note.empty() || parsed.is_discarded() || (outcome != "committed" && outcome != "failed")) {
            std::cerr << "Reconciliation requires IDs, committed|failed outcome, evidence note and valid JSON output\n"; return 2;
        }
        auto status = runtime.reconcile(session, request, {outcome == "committed", parsed, note});
        std::cout << Json{{"ok", status.ok}, {"error", {{"code", status.error_code}, {"message", status.error_message}}}}.dump() << '\n';
        return status ? 0 : 1;
    }
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
        HttpModelConfig config; config.endpoint = endpoint; config.model = model; config.timeout_ms = timeout_ms;
        if (!key_env.empty()) {
            const char* key = std::getenv(key_env.c_str());
            if (!key || !*key) { std::cerr << "Requested credential environment variable is empty\n"; return 2; }
            config.api_key = key;
        }
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
        Result<Reply> result;
        if (jsonl) {
            try {
                const auto item = Json::parse(input);
                if (!item.is_object() || item.size() != 3) throw std::runtime_error("Expected session_id, request_id and input");
                result = runtime.run({item.at("session_id").get<std::string>(), item.at("request_id").get<std::string>(), item.at("input").get<std::string>()});
            } catch (const std::exception&) {
                result = Result<Reply>::failure({"INVALID_REQUEST", "Expected JSON object with session_id, request_id and input strings", "", 400});
            }
        } else result = runtime.run({session, std::to_string(++sequence), input});
        std::cout << encode(result).dump() << std::endl;
        success = success && result.ok();
    }
    return success ? 0 : 1;
}
