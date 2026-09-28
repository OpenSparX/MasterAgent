#pragma once

#include "master_agent/common/types.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace master_agent::reference {
using Json = nlohmann::json;
using ToolHandler = std::function<Result<Json>(const Json&)>;
// Return JSON text: {"response":"..."} or {"tool":"name","arguments":{...}}.
// Callbacks run synchronously; the adapter owns timeout/cancellation semantics.
using ModelHandler = std::function<Result<std::string>(const Json& messages)>;

struct Tool {
    std::string name;
    std::string description;
    // Supported schema: object, primitive properties, required, enum,
    // numeric minimum/maximum, additionalProperties:false. Others are rejected.
    Json parameters;
    ToolHandler execute;
};

// Copies share a thread-safe cancellation signal. Cancellation is cooperative.
class CancellationToken {
public:
    void cancel() const { cancelled_->store(true); }
    bool cancelled() const { return cancelled_->load(); }
private:
    std::shared_ptr<std::atomic<bool>> cancelled_ = std::make_shared<std::atomic<bool>>(false);
};

struct RunOptions {
    std::optional<std::chrono::steady_clock::time_point> deadline;
    CancellationToken cancellation;
};

struct ExecutionContext {
    std::string session_id;
    std::string request_id;
    RunOptions options;
    bool stopRequested() const;
    // Collision-free encoding of session/request bytes; scope to one application/store.
    std::string idempotencyKey() const;
};

enum class ToolState { Committed, Failed, Unknown };

// Failed means the external outcome is definitely unsuccessful. A lost response
// after sending a mutation is Unknown, even when the transport reports an error.
class ToolOutcome {
public:
    static ToolOutcome committed(Json output);
    static ToolOutcome failed(std::string message);
    static ToolOutcome unknown(std::string message);
    ToolState state() const { return state_; }
private:
    friend class Runtime;
    ToolOutcome(ToolState state, Json output, StructuredError error)
        : state_(state), output_(std::move(output)), error_(std::move(error)) {}
    ToolState state_;
    Json output_;
    StructuredError error_;
};

struct ContextTool {
    std::string name;
    std::string description;
    Json parameters;
    std::function<ToolOutcome(const ExecutionContext&, const Json&)> execute;
};

struct Turn {
    std::string session_id;
    std::string request_id;
    std::string input;
};

struct Reply {
    std::string session_id;
    std::string request_id;
    std::string route;  // skill, model, model_tool
    std::string tool;
    Json output;
    bool replayed = false;
};

struct Limits {
    std::size_t sessions = 32;
    std::size_t requests_per_session = 256;
    std::size_t history_turns = 8;
    std::size_t output_bytes = 65536;
};

struct RecoveryRecord {
    Turn turn;
    std::string tool;
    Json arguments;
};

struct Resolution {
    bool committed = false;
    Json output;
    std::string note;  // Required explanation/evidence supplied by the operator.
};

namespace detail { class DurableStore; }

// A synchronous, in-process reference runtime. Concurrent/reentrant calls return
// BUSY. Tool callbacks are trusted host code, not sandboxed. Call openStore()
// before run() for persistent receipts/history and conservative crash recovery.
class Runtime {
public:
    explicit Runtime(Limits limits = {});
    ~Runtime();
    Status openStore(const std::string& path);
    Result<std::vector<RecoveryRecord>> unresolved();
    Status reconcile(const std::string& session_id, const std::string& request_id,
                     const Resolution& resolution);
    Status registerTool(Tool tool);
    Status registerContextTool(ContextTool tool);
    Status registerSkill(std::string phrase, std::string tool, Json arguments);
    Status setModel(ModelHandler model);
    Result<Reply> run(const Turn& turn, const RunOptions& options = {});
    // Discards history AND deduplication. Refuses sessions with UNKNOWN outcomes.
    Status clearSession(const std::string& session_id);

private:
    struct Skill { std::string tool; Json arguments; };
    struct Cached {
        std::string input;
        Result<Reply> result;
        std::string tool;
        Json arguments = Json::object();
    };
    struct Session {
        std::map<std::string, Cached> requests;
        std::vector<Json> history;
    };
    Result<Reply> execute(const Turn& turn, Session& session, const ExecutionContext& context);
    void remember(Session& session, const Turn& turn, const Reply& reply);
    Limits limits_;
    std::mutex mutex_;
    std::map<std::string, ContextTool> tools_;
    std::map<std::string, Skill> skills_;
    std::map<std::string, Session> sessions_;
    ModelHandler model_;
    std::unique_ptr<detail::DurableStore> store_;
    Status storage_status_ = Status::Ok();
};
}  // namespace master_agent::reference
