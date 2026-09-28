#include "master_agent/runtime/reference_runtime.h"
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>
using namespace master_agent;
using namespace master_agent::reference;
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const std::string mode = argv[1], path = argv[2], effect = argv[3];
    Runtime runtime;
    auto status = runtime.openStore(path);
    if (!status) {
        // Ignoring openStore failure must not silently run in memory mode.
        auto guarded = runtime.run({"s", "1", "increment"});
        std::cout << Json{{"error", status.error_code}, {"run_blocked", !guarded}}.dump() << std::endl;
        return 1;
    }
    if (mode == "init") return 0;
    if (mode == "hold") { std::cout << "ready" << std::endl; std::string line; std::getline(std::cin, line); return 0; }
    Json schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    status = runtime.registerTool({"counter.increment", "Append a durable external effect", schema,
        [&](const Json&) -> Result<Json> {
            int fd = open(effect.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0600);
            if (fd < 0) throw std::runtime_error("Cannot open effect");
            const auto count = write(fd, "effect\n", 7);
            const auto sync = fsync(fd); close(fd);
            if (count != 7 || sync != 0) throw std::runtime_error("Cannot persist effect");
            if (mode == "crash") _exit(86); // No destructors or SQLite close.
            if (mode == "throw") throw std::runtime_error("after effect");
            return Result<Json>::success({{"count", 1}});
        }});
    if (!status || !runtime.registerSkill("increment", "counter.increment", Json::object())) return 3;
    auto result = runtime.run({"s", "1", "increment"});
    Json output{{"ok", result.ok()}};
    if (result) { output["replayed"] = result.value->replayed; output["output"] = result.value->output; }
    else output["error"] = result.error->code;
    auto pending = runtime.unresolved();
    output["unresolved"] = pending ? pending.value->size() : 0;
    std::cout << output.dump() << std::endl;
    return result ? 0 : 1;
}
