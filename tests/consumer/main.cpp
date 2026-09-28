#include <master_agent/runtime/reference_runtime.h>
#ifdef TEST_HTTP
#include <master_agent/runtime/http_model.h>
#endif
using namespace master_agent;
using namespace master_agent::reference;
int main(int argc, char** argv) {
#ifdef TEST_STORAGE
    if (argc == 2) {
        {
            Runtime durable;
            if (!durable.openStore(argv[1])) return 5;
            if (!durable.setModel([](const Json&) { return Result<std::string>::success(R"({"response":"stored"})"); })) return 6;
            if (!durable.run({"installed", "1", "persist this"})) return 7;
        }
        Runtime reopened;
        if (!reopened.openStore(argv[1])) return 8;
        auto receipt = reopened.run({"installed", "1", "persist this"});
        if (!receipt || !receipt.value->replayed || receipt.value->output != "stored") return 9;
        return reopened.clearSession("installed") ? 0 : 10;
    }
#endif
    Runtime runtime;
#ifdef TEST_HTTP
    if (!runtime.setModel(makeHttpModel({}))) return 4;
#endif
    int calls = 0;
    Json schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    if (!runtime.registerTool({"ping", "Consumer integration", schema,
        [&](const Json&) { ++calls; return Result<Json>::success("pong"); }})) return 1;
    if (!runtime.registerSkill("ping", "ping", Json::object())) return 2;
    auto result = runtime.run({"consumer", "1", "ping"});
    return result && result.value->output == "pong" && calls == 1 ? 0 : 3;
}
