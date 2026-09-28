#include <master_agent/runtime/reference_runtime.h>
#ifdef TEST_HTTP
#include <master_agent/runtime/http_model.h>
#endif
using namespace master_agent;
using namespace master_agent::reference;
int main() {
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
