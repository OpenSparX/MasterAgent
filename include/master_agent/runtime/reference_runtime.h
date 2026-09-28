#pragma once

#include "master_agent/common/types.h"
#include <nlohmann/json.hpp>
#include <functional>
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
    Status registerSkill(std::string phrase, std::string tool, Json arguments);
    Status setModel(ModelHandler model);
    Result<Reply> run(const Turn& turn);
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
    Result<Reply> execute(const Turn& turn, Session& session);
    void remember(Session& session, const Turn& turn, const Reply& reply);
    Limits limits_;
    std::mutex mutex_;
    std::map<std::string, Tool> tools_;
    std::map<std::string, Skill> skills_;
    std::map<std::string, Session> sessions_;
    ModelHandler model_;
    std::unique_ptr<detail::DurableStore> store_;
    Status storage_status_ = Status::Ok();
};
}  // namespace master_agent::reference
