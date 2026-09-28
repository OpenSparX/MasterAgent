#include <master_agent/runtime/reference_runtime.h>
using namespace master_agent;
using namespace master_agent::reference;
int main() {
    Runtime runtime;
    int calls = 0;
    Json schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    if (!runtime.registerTool({"ping", "Consumer integration", schema,
        [&](const Json&) { ++calls; return Result<Json>::success("pong"); }})) return 1;
    if (!runtime.registerSkill("ping", "ping", Json::object())) return 2;
    auto result = runtime.run({"consumer", "1", "ping"});
    return result && result.value->output == "pong" && calls == 1 ? 0 : 3;
}
