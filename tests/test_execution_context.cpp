#include "master_agent/runtime/reference_runtime.h"
#include "check.h"
#include <future>
#include <thread>
using namespace master_agent;
using namespace master_agent::reference;
int main() {
    Json schema{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}};
    Runtime runtime({1, 32, 8});
    int calls = 0;
    CHECK(runtime.registerContextTool({"effect", "context-aware", schema,
        [&](const ExecutionContext& context, const Json&) {
            ++calls;
            CHECK(context.session_id == "s" && context.request_id == "1");
            CHECK(context.idempotencyKey() == "73.31");
            context.options.cancellation.cancel(); // Cancellation after a confirmed effect.
            return ToolOutcome::committed(42);
        }}));
    CHECK(runtime.registerSkill("effect", "effect", Json::object()));
    RunOptions cancelled;
    cancelled.cancellation.cancel();
    CHECK(runtime.run({"unused", "1", "effect"}, cancelled).error->code == "CANCELLED");
    RunOptions expired;
    expired.deadline = std::chrono::steady_clock::now();
    CHECK(runtime.run({"unused", "1", "effect"}, expired).error->code == "DEADLINE_EXCEEDED");
    CHECK(calls == 0);
    CHECK(runtime.run({"s", "1", "effect"}).value->output == 42);
    CHECK(runtime.run({"s", "1", "effect"}, cancelled).value->replayed && calls == 1);
    CHECK(runtime.registerContextTool({"rejected", "known failure", schema,
        [](const ExecutionContext&, const Json&) { return ToolOutcome::failed("Insufficient stock"); }}));
    CHECK(runtime.registerSkill("reject", "rejected", Json::object()));
    CHECK(runtime.run({"s", "2", "reject"}).error->code == "TOOL_FAILED");
    CHECK(runtime.unresolved().value->empty());
    CHECK(runtime.registerContextTool({"lost", "lost receipt", schema,
        [](const ExecutionContext&, const Json&) { return ToolOutcome::unknown("Service committed; response lost"); }}));
    CHECK(runtime.registerSkill("lost", "lost", Json::object()));
    CHECK(runtime.run({"s", "3", "lost"}).error->code == "UNKNOWN");
    CHECK(runtime.unresolved().value->size() == 1);
    CHECK(runtime.reconcile("s", "3", {true, "verified", "External receipt found"}));
    CHECK(runtime.run({"s", "3", "lost"}).value->output == "verified");
    // Delimiters and Unicode do not collide across the session/request boundary.
    CHECK(ExecutionContext{"a:b", "c", {}}.idempotencyKey() != ExecutionContext{"a", "b:c", {}}.idempotencyKey());
    CHECK(ExecutionContext{"中", "文", {}}.idempotencyKey() == "e4b8ad.e69687");
    Runtime during_model;
    RunOptions options;
    int effects = 0;
    CHECK(during_model.registerContextTool({"effect", "must not run", schema,
        [&](const ExecutionContext&, const Json&) { ++effects; return ToolOutcome::committed(1); }}));
    CHECK(during_model.setModel([&](const Json&) {
        options.cancellation.cancel();
        return Result<std::string>::success(R"({"tool":"effect","arguments":{}})");
    }));
    CHECK(during_model.run({"s", "m", "model"}, options).error->code == "CANCELLED");
    CHECK(effects == 0);
    RunOptions short_deadline;
    short_deadline.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    CHECK(during_model.setModel([&](const Json&) {
        while (std::chrono::steady_clock::now() < *short_deadline.deadline) std::this_thread::yield();
        return Result<std::string>::success(R"({"tool":"effect","arguments":{}})");
    }));
    CHECK(during_model.run({"s", "expired-model", "model"}, short_deadline).error->code == "DEADLINE_EXCEEDED");
    CHECK(effects == 0);
    Runtime cooperative;
    RunOptions signal;
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    CHECK(cooperative.registerContextTool({"wait", "cooperative cancellation", schema,
        [&](const ExecutionContext& context, const Json&) {
            entered.set_value();
            while (!context.stopRequested()) std::this_thread::yield();
            return ToolOutcome::failed("Cancelled before any side effect");
        }}));
    CHECK(cooperative.registerSkill("wait", "wait", Json::object()));
    auto running = std::async(std::launch::async, [&] { return cooperative.run({"s", "w", "wait"}, signal); });
    CHECK(entered_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    signal.cancellation.cancel();
    CHECK(running.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(running.get().error->code == "TOOL_FAILED");
}
