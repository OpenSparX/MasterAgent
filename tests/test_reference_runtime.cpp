#include "master_agent/runtime/reference_runtime.h"
#include "check.h"
#include <stdexcept>

using namespace master_agent;
using namespace master_agent::reference;
int main() {
    Runtime runtime;
    int calls = 0;
    Json schema{{"type", "object"}, {"properties", {{"value", {{"type", "integer"}, {"minimum", 0}, {"maximum", 10}}}}}, {"required", {"value"}}, {"additionalProperties", false}};
    CHECK(runtime.registerTool({"set", "Set a value", schema, [&](const Json& args) {
        ++calls;
        CHECK(!runtime.run({"recursive", "1", "set five"}));
        return Result<Json>::success(args);
    }}));
    CHECK(!runtime.registerTool({"set", "duplicate", schema, [](const Json&) { return Result<Json>::success(0); }}));
    CHECK(!runtime.registerSkill("bad", "set", {{"value", 11}}));
    CHECK(runtime.registerSkill("set five", "set", {{"value", 5}}));
    auto first = runtime.run({"a", "1", "set five"});
    CHECK(first && first.value->route == "skill" && first.value->output["value"] == 5 && calls == 1);
    auto replay = runtime.run({"a", "1", "set five"});
    CHECK(replay && replay.value->replayed && calls == 1);
    CHECK(runtime.run({"a", "1", "different"}).error->code == "REQUEST_CONFLICT");
    CHECK(runtime.run({"b", "1", "set five"}) && calls == 2);
    CHECK(runtime.run({"a", "2", "unmatched"}).error->code == "NO_ROUTE");
    std::string decision = R"({"tool":"set","arguments":{"value":8}})";
    CHECK(runtime.setModel([&](const Json& messages) {
        CHECK(messages.front()["role"] == "system");
        return Result<std::string>::success(decision);
    }));
    CHECK(runtime.run({"a", "3", "model"}).value->route == "model_tool" && calls == 3);
    int sequence = 3;
    for (auto output : {R"({"tool":"set","arguments":{"value":11}})",
                        R"({"tool":"set","arguments":{"value":"8"}})",
                        R"({"tool":"set","arguments":{}})",
                        R"({"tool":"set","arguments":{"value":1,"extra":true}})",
                        R"({"tool":"unknown","arguments":{}})",
                        R"({"response":"text","tool":"set","arguments":{"value":1}})",
                        "not json"}) {
        decision = output;
        CHECK(!runtime.run({"a", std::to_string(++sequence), "model"}));
        CHECK(calls == 3);
    }
    decision = R"({"response":"你好"})";
    CHECK(runtime.run({"a", std::to_string(++sequence), "hello"}).value->output == "你好");
    CHECK(runtime.registerTool({"uncertain", "Throws after side effect", schema, [&](const Json&) -> Result<Json> { ++calls; throw std::runtime_error("after effect"); }}));
    CHECK(runtime.registerSkill("uncertain", "uncertain", {{"value", 1}}));
    CHECK(runtime.run({"c", "1", "uncertain"}).error->code == "UNKNOWN");
    CHECK(runtime.run({"c", "1", "uncertain"}).error->code == "UNKNOWN" && calls == 4);
    CHECK(runtime.clearSession("a"));
    Runtime limited({1, 1, 0});
    CHECK(limited.setModel([](const Json&) { return Result<std::string>::success(R"({"response":"ok"})"); }));
    CHECK(limited.run({"s", "1", "a"}));
    CHECK(limited.run({"s", "2", "a"}).error->code == "RESOURCE_LIMIT");
    CHECK(limited.run({"t", "1", "a"}).error->code == "RESOURCE_LIMIT");
    auto unsupported = schema; unsupported["properties"]["value"]["pattern"] = "x";
    CHECK(!runtime.registerTool({"invalid", "invalid", unsupported, [](const Json&) { return Result<Json>::success(0); }}));
}
