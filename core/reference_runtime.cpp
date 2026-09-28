#include "master_agent/runtime/reference_runtime.h"
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
}  // namespace

Runtime::Runtime(Limits limits) : limits_(limits) {}
Status Runtime::registerTool(Tool tool) {
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
    sessions_.erase(id);
    return Status::Ok();
}
Result<Reply> Runtime::run(const Turn& turn) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock) return fail<Reply>("BUSY", "Runtime is executing another operation");
    if (turn.session_id.empty() || turn.request_id.empty() || turn.input.empty() || turn.input.size() > 65536 || turn.session_id.size() > 256 || turn.request_id.size() > 256)
        return fail<Reply>("INVALID_REQUEST", "Nonempty bounded session, request and input are required");
    if (!sessions_.count(turn.session_id) && sessions_.size() >= limits_.sessions) return fail<Reply>("RESOURCE_LIMIT", "Session limit reached");
    auto& session = sessions_[turn.session_id];
    if (auto it = session.requests.find(turn.request_id); it != session.requests.end()) {
        if (it->second.input != turn.input) return fail<Reply>("REQUEST_CONFLICT", "Request ID was already used for different input");
        auto result = it->second.result;
        if (result) result.value->replayed = true;
        return result;
    }
    if (session.requests.size() >= limits_.requests_per_session) return fail<Reply>("RESOURCE_LIMIT", "Request limit reached; start a new session");
    auto result = execute(turn, session);
    // Cache failures too: an UNKNOWN tool outcome must never trigger an automatic retry.
    session.requests.emplace(turn.request_id, Cached{turn.input, result});
    if (result) {
        session.history.push_back({{"role", "user"}, {"content", turn.input}});
        session.history.push_back({{"role", "assistant"}, {"content", result.value->output.dump()}});
        while (session.history.size() / 2 > limits_.history_turns) session.history.erase(session.history.begin(), session.history.begin() + 2);
    }
    return result;
}
Result<Reply> Runtime::execute(const Turn& turn, Session& session) {
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
    try {
        auto result = tool->second.execute(arguments);
        if (!result) return Result<Reply>::failure(result.error.value_or(StructuredError{"TOOL_ERROR", "Tool returned no result", "", 500}));
        reply.output = *result;
    } catch (...) {
        // The handler may have performed its side effect before throwing.
        return fail<Reply>("UNKNOWN", "Tool threw; reconcile its outcome before issuing a new request");
    }
    return Result<Reply>::success(std::move(reply));
}
}  // namespace master_agent::reference
