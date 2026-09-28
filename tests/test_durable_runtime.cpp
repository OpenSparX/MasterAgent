#include "master_agent/runtime/reference_runtime.h"
#include "check.h"
#include <filesystem>
#include <chrono>
#include <stdexcept>
using namespace master_agent;
using namespace master_agent::reference;
int main() {
    struct Scratch {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("sparx-durable-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Scratch() { CHECK(std::filesystem::create_directory(path)); }
        ~Scratch() { std::error_code error; std::filesystem::remove_all(path, error); }
    } scratch;
    const auto db = (scratch.path / "requests.db").string();
    int calls = 0;
    Json schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    {
        Runtime runtime;
        CHECK(runtime.openStore(db));
        Runtime other;
        CHECK(other.openStore(db).error_code == "STORAGE_BUSY");
        CHECK(!other.run({"x", "1", "ignored-open-failure"}));
        CHECK(runtime.registerTool({"effect", "test", schema, [&](const Json&) { ++calls; return Result<Json>::success(1); }}));
        CHECK(runtime.registerSkill("effect", "effect", Json::object()));
        CHECK(runtime.run({"s", "1", "effect"}));
        CHECK(runtime.run({"s", "1", "effect"}).value->replayed && calls == 1);
        CHECK(runtime.registerTool({"throws", "test", schema, [&](const Json&) -> Result<Json> { ++calls; throw std::runtime_error("uncertain"); }}));
        CHECK(runtime.registerSkill("throws", "throws", Json::object()));
        CHECK(runtime.run({"s", "2", "throws"}).error->code == "UNKNOWN");
        CHECK(runtime.clearSession("s").error_code == "UNRESOLVED_REQUESTS");
    }
    {
        Runtime runtime;
        CHECK(runtime.openStore(db));
        // No tools are registered: replay and recovery must be independent of them.
        CHECK(runtime.run({"s", "1", "effect"}).value->replayed && calls == 2);
        CHECK(runtime.run({"s", "1", "different"}).error->code == "REQUEST_CONFLICT");
        auto pending = runtime.unresolved();
        CHECK(pending && pending.value->size() == 1 && pending.value->front().tool == "throws");
        CHECK(!runtime.reconcile("s", "2", {true, 2, ""}));
        CHECK(runtime.reconcile("s", "2", {true, 2, "Verified external effect #2"}));
        CHECK(runtime.unresolved().value->empty());
        CHECK(runtime.run({"s", "2", "throws"}).value->route == "reconciled");
        CHECK(!runtime.reconcile("s", "2", {true, 3, "cannot rewrite a final outcome"}));
        CHECK(runtime.setModel([](const Json& messages) {
            CHECK(messages.size() == 6); // system + two recovered turns + current input
            return Result<std::string>::success(R"({"response":"history recovered"})");
        }));
        CHECK(runtime.run({"s", "3", "question"}));
    }
    {
        Runtime runtime;
        CHECK(runtime.openStore(db));
        CHECK(runtime.run({"s", "2", "throws"}).value->output == 2);
        CHECK(runtime.clearSession("s"));
    }
    {
        Runtime runtime;
        CHECK(runtime.openStore(db));
        CHECK(runtime.run({"s", "1", "effect"}).error->code == "NO_ROUTE");
    }
    Runtime memory;
    CHECK(memory.run({"s", "1", std::string(1, static_cast<char>(0xff))}).error->code == "INVALID_REQUEST");
    Runtime small({1, 10, 1, 4});
    CHECK(small.registerTool({"large", "test", schema, [](const Json&) { return Result<Json>::success("too long"); }}));
    CHECK(small.registerSkill("large", "large", Json::object()));
    CHECK(small.run({"s", "1", "large"}).error->code == "UNKNOWN");
}
