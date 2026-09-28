#include "master_agent/runtime/reference_runtime.h"
#include "durable_store.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace master_agent::reference {
namespace {
template<class T> Result<T> fail(const std::string& code, const std::string& text) {
    return Result<T>::failure({code, text, "", 400});
}
bool primitive(const Json& value, const std::string& type) {
    if (type == "string") return value.is_string();
    if (type == "boolean") return value.is_boolean();
    if (type == "integer") return value.is_number_integer();
    if (type == "number") return value.is_number() && std::isfinite(value.get<double>());
    return false;
}
Status schemaValid(const Json& schema) {
    if (!schema.is_object() || schema.value("type", "") != "object" ||
        !schema.contains("properties") || !schema["properties"].is_object() ||
        !schema.contains("additionalProperties") || schema["additionalProperties"] != false)
        return Status::Error("INVALID_SCHEMA", "Expected object schema with additionalProperties:false");
    const std::set<std::string> root_keys{"type", "properties", "required", "additionalProperties"};
    for (const auto& item : schema.items())
        if (!root_keys.count(item.key())) return Status::Error("INVALID_SCHEMA", "Unsupported schema keyword: " + item.key());
    for (const auto& item : schema["properties"].items()) {
        const auto& field = item.value();
        if (!field.is_object()) return Status::Error("INVALID_SCHEMA", "Property must be an object");
        const auto type = field.value("type", "");
        if (type != "string" && type != "boolean" && type != "integer" && type != "number")
            return Status::Error("INVALID_SCHEMA", "Only primitive property types are supported");
        for (const auto& keyword : field.items()) {
            const auto& k = keyword.key();
            if (k != "type" && k != "description" && k != "enum" && k != "minimum" && k != "maximum")
                return Status::Error("INVALID_SCHEMA", "Unsupported property keyword: " + k);
        }
        if (field.contains("description") && !field["description"].is_string())
            return Status::Error("INVALID_SCHEMA", "description must be a string");
        for (const auto* bound : {"minimum", "maximum"})
            if (field.contains(bound) && ((type != "number" && type != "integer") || !primitive(field[bound], "number")))
                return Status::Error("INVALID_SCHEMA", "Invalid numeric bound");
        if (field.contains("minimum") && field.contains("maximum") && field["minimum"] > field["maximum"])
            return Status::Error("INVALID_SCHEMA", "minimum exceeds maximum");
        if (field.contains("enum")) {
            if (!field["enum"].is_array() || field["enum"].empty()) return Status::Error("INVALID_SCHEMA", "enum must be a nonempty array");
            for (const auto& value : field["enum"])
                if (!primitive(value, type)) return Status::Error("INVALID_SCHEMA", "enum type mismatch");
        }
    }
    if (schema.contains("required")) {
        if (!schema["required"].is_array()) return Status::Error("INVALID_SCHEMA", "required must be an array");
        for (const auto& name : schema["required"])
            if (!name.is_string() || !schema["properties"].contains(name.get<std::string>()))
                return Status::Error("INVALID_SCHEMA", "Unknown required property");
    }
    return Status::Ok();
}
Status validate(const Json& args, const Json& schema) {
    if (!args.is_object()) return Status::Error("INVALID_ARGUMENTS", "Arguments must be an object");
    for (const auto& key : schema.value("required", Json::array()))
        if (!args.contains(key.get<std::string>())) return Status::Error("INVALID_ARGUMENTS", "Missing field: " + key.get<std::string>());
    for (const auto& arg : args.items()) {
        if (!schema["properties"].contains(arg.key())) return Status::Error("INVALID_ARGUMENTS", "Unknown field: " + arg.key());
        const auto& field = schema["properties"][arg.key()];
        if (!primitive(arg.value(), field["type"].get<std::string>())) return Status::Error("INVALID_ARGUMENTS", "Wrong type: " + arg.key());
        if (field.contains("enum") && std::find(field["enum"].begin(), field["enum"].end(), arg.value()) == field["enum"].end())
            return Status::Error("INVALID_ARGUMENTS", "Value outside enum: " + arg.key());
        if ((field.contains("minimum") && arg.value() < field["minimum"]) ||
            (field.contains("maximum") && arg.value() > field["maximum"]))
            return Status::Error("INVALID_ARGUMENTS", "Value outside range: " + arg.key());
    }
    return Status::Ok();
}
Status stopped(const ExecutionContext& context) {
    if (context.options.cancellation.cancelled()) return Status::Error("CANCELLED", "Execution cancelled before tool invocation");
    if (context.options.deadline && std::chrono::steady_clock::now() >= *context.options.deadline)
        return Status::Error("DEADLINE_EXCEEDED", "Execution deadline elapsed before tool invocation");
    return Status::Ok();
}
}  // namespace

bool ExecutionContext::stopRequested() const { return !stopped(*this); }
std::string ExecutionContext::idempotencyKey() const {
    constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (const auto* part : {&session_id, &request_id}) {
        if (!key.empty()) key += '.';
        for (unsigned char byte : *part) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    }
    return key;
}
ToolOutcome ToolOutcome::committed(Json output) { return {ToolState::Committed, std::move(output), {}}; }
ToolOutcome ToolOutcome::failed(std::string message) { return {ToolState::Failed, {}, {"TOOL_FAILED", std::move(message), "", 422}}; }
ToolOutcome ToolOutcome::unknown(std::string message) { return {ToolState::Unknown, {}, {"UNKNOWN", std::move(message), "", 409}}; }

Runtime::Runtime(Limits limits) : limits_(limits) {}
Runtime::~Runtime() = default;

Status Runtime::openStore(const std::string& path) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return Status::Error("BUSY");
    if (store_ || !sessions_.empty()) return Status::Error("STORE_ALREADY_ACTIVE", "Open a store before creating sessions");
    auto candidate = std::make_unique<detail::DurableStore>();
    storage_status_ = candidate->open(path);
    if (!storage_status_) return storage_status_;
    auto snapshot = candidate->load(limits_);
    if (!snapshot) {
        storage_status_ = Status::Error("STORAGE_ERROR", snapshot.error->message);
        return storage_status_;
    }
    std::map<std::string, Session> recovered;
    for (auto& entry : snapshot.value->histories) recovered[entry.first].history = std::move(entry.second);
    for (auto& entry : snapshot.value->requests) {
        recovered[entry.turn.session_id].requests.emplace(entry.turn.request_id,
            Cached{entry.turn.input, std::move(entry.result), entry.tool, std::move(entry.arguments)});
    }
    sessions_ = std::move(recovered);
    store_ = std::move(candidate);
    return Status::Ok();
}

Result<std::vector<RecoveryRecord>> Runtime::unresolved() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return fail<std::vector<RecoveryRecord>>("BUSY", "Runtime is executing");
    std::vector<RecoveryRecord> records;
    for (const auto& session : sessions_) for (const auto& request : session.second.requests) {
        const auto& cached = request.second;
        if (cached.result.error && cached.result.error->code == "UNKNOWN")
            records.push_back({{session.first, request.first, cached.input}, cached.tool, cached.arguments});
    }
    return Result<std::vector<RecoveryRecord>>::success(std::move(records));
}

void Runtime::remember(Session& session, const Turn& turn, const Reply& reply) {
    Json user{{"role", "user"}, {"content", turn.input}};
    Json assistant{{"role", "assistant"}, {"content", reply.output.dump()}};
    session.history.reserve(session.history.size() + 2);
    session.history.push_back(std::move(user));
    session.history.push_back(std::move(assistant));
    while (session.history.size() / 2 > limits_.history_turns)
        session.history.erase(session.history.begin(), session.history.begin() + 2);
}

Status Runtime::reconcile(const std::string& session_id, const std::string& request_id, const Resolution& resolution) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return Status::Error("BUSY");
    if (!storage_status_) return storage_status_;
    if (resolution.note.empty() || resolution.note.size() > 4096) return Status::Error("INVALID_RESOLUTION", "A bounded evidence note is required");
    auto session = sessions_.find(session_id);
    if (session == sessions_.end() || !session->second.requests.count(request_id)) return Status::Error("NOT_FOUND", "No such request");
    auto& cached = session->second.requests.at(request_id);
    if (!cached.result.error || cached.result.error->code != "UNKNOWN") return Status::Error("NOT_UNKNOWN", "Only UNKNOWN requests can be reconciled");
    try {
        if (resolution.output.dump().size() > limits_.output_bytes) return Status::Error("INVALID_RESOLUTION", "Output exceeds limit");
        Json(resolution.note).dump(); // Validate UTF-8 before any store mutation.
        auto result = resolution.committed
            ? Result<Reply>::success({session_id, request_id, "reconciled", cached.tool, resolution.output, false})
            : fail<Reply>("RECONCILED_FAILED", resolution.note);
        auto previous_history = session->second.history;
        if (result) remember(session->second, {session_id, request_id, cached.input}, *result);
        if (store_) {
            storage_status_ = store_->complete({session_id, request_id, cached.input}, result, session->second.history, resolution.note);
            if (!storage_status_) { session->second.history = std::move(previous_history); return storage_status_; }
        }
        cached.result = std::move(result);
        return Status::Ok();
    } catch (const Json::exception& e) { return Status::Error("INVALID_RESOLUTION", e.what()); }
}

Status Runtime::registerTool(Tool tool) {
    if (!tool.execute) return Status::Error("INVALID_TOOL", "Tool requires a handler");
    return registerContextTool({std::move(tool.name), std::move(tool.description), std::move(tool.parameters),
        [handler = std::move(tool.execute)](const ExecutionContext&, const Json& args) {
            auto result = handler(args);
            if (result) return ToolOutcome::committed(std::move(*result));
            auto error = result.error.value_or(StructuredError{"TOOL_ERROR", "Tool returned no result", "", 500});
            if (error.code == "UNKNOWN") return ToolOutcome::unknown(error.message);
            return ToolOutcome(ToolState::Failed, {}, std::move(error));
        }});
}
Status Runtime::registerContextTool(ContextTool tool) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return Status::Error("BUSY");
    if (tool.name.empty() || !tool.execute || tools_.count(tool.name)) return Status::Error("INVALID_TOOL", "Tool requires a unique name and handler");
    try { auto status = schemaValid(tool.parameters); if (!status) return status; }
    catch (const Json::exception& e) { return Status::Error("INVALID_SCHEMA", e.what()); }
    tools_.emplace(tool.name, std::move(tool));
    return Status::Ok();
}
Status Runtime::registerSkill(std::string phrase, std::string tool, Json arguments) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return Status::Error("BUSY");
    if (phrase.empty() || skills_.count(phrase) || !tools_.count(tool)) return Status::Error("INVALID_SKILL", "Skill needs a unique phrase and registered tool");
    auto status = validate(arguments, tools_.at(tool).parameters);
    if (!status) return status;
    skills_.emplace(std::move(phrase), Skill{std::move(tool), std::move(arguments)});
    return Status::Ok();
}
Status Runtime::setModel(ModelHandler model) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return Status::Error("BUSY");
    model_ = std::move(model);
    return Status::Ok();
}
Status Runtime::clearSession(const std::string& id) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return Status::Error("BUSY");
    if (!storage_status_) return storage_status_;
    if (auto it = sessions_.find(id); it != sessions_.end())
        for (const auto& request : it->second.requests)
            if (request.second.result.error && request.second.result.error->code == "UNKNOWN")
                return Status::Error("UNRESOLVED_REQUESTS", "Reconcile unknown outcomes before clearing the session");
    if (store_) { storage_status_ = store_->clear(id); if (!storage_status_) return storage_status_; }
    sessions_.erase(id);
    return Status::Ok();
}
Result<Reply> Runtime::run(const Turn& turn, const RunOptions& options) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return fail<Reply>("BUSY", "Runtime is executing another operation");
    if (!storage_status_) return fail<Reply>(storage_status_.error_code, storage_status_.error_message);
    if (turn.session_id.empty() || turn.request_id.empty() || turn.input.empty() || turn.input.size() > 65536 || turn.session_id.size() > 256 || turn.request_id.size() > 256)
        return fail<Reply>("INVALID_REQUEST", "Nonempty bounded session, request and input are required");
    try { Json{{"session", turn.session_id}, {"request", turn.request_id}, {"input", turn.input}}.dump(); }
    catch (const Json::exception&) { return fail<Reply>("INVALID_REQUEST", "Request text must be valid UTF-8"); }
    auto existing = sessions_.find(turn.session_id);
    if (existing != sessions_.end()) {
        auto it = existing->second.requests.find(turn.request_id);
        if (it != existing->second.requests.end()) {
            if (it->second.input != turn.input) return fail<Reply>("REQUEST_CONFLICT", "Request ID was already used for different input");
            auto result = it->second.result;
            if (result) result.value->replayed = true;
            return result;
        }
    }
    const ExecutionContext context{turn.session_id, turn.request_id, options};
    auto ready = stopped(context);
    if (!ready) return fail<Reply>(ready.error_code, ready.error_message);
    if (existing == sessions_.end() && sessions_.size() >= limits_.sessions) return fail<Reply>("RESOURCE_LIMIT", "Session limit reached");
    auto& session = sessions_[turn.session_id];
    if (session.requests.size() >= limits_.requests_per_session) return fail<Reply>("RESOURCE_LIMIT", "Request limit reached; start a new session");
    if (store_) {
        storage_status_ = store_->begin(turn);
        if (!storage_status_) return fail<Reply>(storage_status_.error_code, storage_status_.error_message);
    }
    auto& cached = session.requests.emplace(turn.request_id, Cached{turn.input,
        fail<Reply>("UNKNOWN", "Request started; outcome not recorded"), "", Json::object()}).first->second;
    auto previous_history = session.history;
    Result<Reply> result;
    try {
        result = execute(turn, session, context);
        if (result) {
            if (result.value->output.dump().size() > limits_.output_bytes)
                result = fail<Reply>(cached.tool.empty() ? "OUTPUT_LIMIT" : "UNKNOWN", "Output exceeds limit; inspect any tool side effect");
            else remember(session, turn, *result);
        }
    } catch (const std::exception&) {
        result = fail<Reply>(cached.tool.empty() ? "INTERNAL" : "UNKNOWN", "Unable to encode the outcome; inspect any tool side effect");
    }
    if (store_ && storage_status_) storage_status_ = store_->complete(turn, result, session.history);
    if (!storage_status_) {
        session.history = std::move(previous_history);
        result = fail<Reply>("UNKNOWN", "Could not durably record the outcome; reopen the store and reconcile before retrying");
    }
    cached.result = result;
    return result;
}
Result<Reply> Runtime::execute(const Turn& turn, Session& session, const ExecutionContext& context) {
    Reply reply{turn.session_id, turn.request_id, "skill", "", Json{}, false};
    Json arguments;
    if (auto it = skills_.find(turn.input); it != skills_.end()) {
        reply.tool = it->second.tool;
        arguments = it->second.arguments;
    } else {
        if (!model_) return fail<Reply>("NO_ROUTE", "No matching skill and no model configured");
        Json definitions = Json::array();
        for (const auto& entry : tools_) definitions.push_back({{"name", entry.first}, {"description", entry.second.description}, {"parameters", entry.second.parameters}});
        Json messages = Json::array({{{"role", "system"}, {"content", "Return only JSON: {\"response\":\"text\"} or {\"tool\":\"name\",\"arguments\":{...}}. Use only these tools: " + definitions.dump()}}});
        for (const auto& message : session.history) messages.push_back(message);
        messages.push_back({{"role", "user"}, {"content", turn.input}});
        Result<std::string> model_result;
        try { model_result = model_(messages); }
        catch (const std::exception& e) { return fail<Reply>("MODEL_ERROR", e.what()); }
        catch (...) { return fail<Reply>("MODEL_ERROR", "Model callback threw"); }
        if (!model_result) return Result<Reply>::failure(model_result.error.value_or(StructuredError{"MODEL_ERROR", "Model returned no result", "", 500}));
        if (model_result.value->size() > 65536) return fail<Reply>("INVALID_MODEL_OUTPUT", "Model output exceeds 64 KiB");
        auto decision = Json::parse(*model_result, nullptr, false);
        if (!decision.is_object()) return fail<Reply>("INVALID_MODEL_OUTPUT", "Expected a JSON decision object");
        if (decision.size() == 1 && decision.contains("response") && decision["response"].is_string()) {
            reply.route = "model";
            reply.output = decision["response"];
            return Result<Reply>::success(std::move(reply));
        }
        if (decision.size() != 2 || !decision.contains("tool") || !decision["tool"].is_string() || !decision.contains("arguments"))
            return fail<Reply>("INVALID_MODEL_OUTPUT", "Expected response or tool/arguments");
        reply.route = "model_tool";
        reply.tool = decision["tool"].get<std::string>();
        arguments = decision["arguments"];
    }
    auto tool = tools_.find(reply.tool);
    if (tool == tools_.end()) return fail<Reply>("UNKNOWN_TOOL", "Model selected an unregistered tool");
    auto valid = validate(arguments, tool->second.parameters);
    if (!valid) return fail<Reply>(valid.error_code, valid.error_message);
    auto ready = stopped(context);
    if (!ready) return fail<Reply>(ready.error_code, ready.error_message);
    auto& receipt = session.requests.at(turn.request_id);
    receipt.tool = reply.tool;
    receipt.arguments = arguments;
    if (store_) {
        storage_status_ = store_->dispatch(turn, reply.tool, arguments);
        if (!storage_status_) return fail<Reply>("STORAGE_ERROR", "Dispatch was not persisted; tool was not called");
    }
    ready = stopped(context);
    if (!ready) return fail<Reply>(ready.error_code, ready.error_message);
    try {
        auto result = tool->second.execute(context, arguments);
        if (result.state_ != ToolState::Committed) return Result<Reply>::failure(std::move(result.error_));
        // A confirmed effect remains committed even if cancellation arrived during it.
        reply.output = std::move(result.output_);
    } catch (...) {
        // The handler may have performed its side effect before throwing.
        return fail<Reply>("UNKNOWN", "Tool threw; reconcile its outcome before issuing a new request");
    }
    return Result<Reply>::success(std::move(reply));
}
}  // namespace master_agent::reference
