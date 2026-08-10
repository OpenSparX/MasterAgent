/**
 * @file mcp_wire.cpp
 * @brief Implements strict MCP JSON-RPC parsing and serialization.
 */

#include "master_agent/atomic_service/mcp_wire.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace master_agent::atomic_service {
namespace {

constexpr std::size_t kMaxJsonNesting = 32U;
constexpr std::size_t kMaxIdBytes = 128U;
constexpr std::size_t kMaxCursorBytes = 1024U;
constexpr std::size_t kMaxToolNameBytes = 128U;
constexpr std::size_t kMaxTitleBytes = 512U;
constexpr std::size_t kMaxDescriptionBytes = 8192U;
constexpr std::size_t kMaxArgumentNodes = 4096U;
constexpr std::size_t kMaxArgumentDepth = 16U;
constexpr std::size_t kMaxArgumentMembers = 256U;
constexpr std::size_t kMaxArgumentArrayItems = 1024U;
constexpr std::size_t kMaxArgumentStringBytes = 16U * 1024U;
constexpr std::size_t kMaxSchemaNodes = 8192U;
constexpr std::size_t kMaxSchemaDepth = 32U;
constexpr std::size_t kMaxSchemaMembers = 512U;
constexpr std::size_t kMaxSchemaArrayItems = 2048U;
constexpr std::size_t kMaxSchemaStringBytes = 64U * 1024U;
constexpr std::size_t kMaxResultTextItems = 64U;
constexpr std::size_t kMaxResultTextBytes = 64U * 1024U;
constexpr std::int64_t kMaxInteroperableInteger = 9007199254740991LL;
constexpr std::string_view kNumericBusinessIdPrefix =
    "mcp-jsonrpc-int:";

enum class RawJsonPreflight : std::uint8_t {
    Acceptable,
    Malformed,
    TooDeep
};

struct JsonBounds {
    std::size_t max_nodes = 0;
    std::size_t max_depth = 0;
    std::size_t max_object_members = 0;
    std::size_t max_array_items = 0;
    std::size_t max_string_bytes = 0;
};

Status wireStatus(std::string code, std::string message) {
    return Status::Error("atomic_service", std::move(code),
                         std::move(message), false,
                         SideEffectState::NotApplicable);
}

McpWireParseResult parseFailure(
    std::string stable_code, std::string message,
    McpJsonRpcErrorCode protocol_error,
    nlohmann::json response_id = nullptr) {
    McpWireParseResult result;
    result.status =
        wireStatus(std::move(stable_code), std::move(message));
    result.response_id = std::move(response_id);
    result.protocol_error = protocol_error;
    return result;
}

bool isValidUtf8(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first =
            static_cast<unsigned char>(text[offset]);
        if (first <= 0x7FU) {
            ++offset;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
            code_point = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            code_point = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (offset + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t index = 1; index <= continuation_count;
             ++index) {
            const auto byte = static_cast<unsigned char>(
                text[offset + index]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            code_point =
                (code_point << 6U) | (byte & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        offset += continuation_count + 1U;
    }
    return true;
}

bool isSafeText(std::string_view text, std::size_t max_bytes,
                bool allow_empty, bool allow_line_breaks) {
    if ((!allow_empty && text.empty()) || text.size() > max_bytes ||
        !isValidUtf8(text)) {
        return false;
    }
    for (const auto raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == 0U) {
            return false;
        }
        if (byte < 0x20U &&
            !(allow_line_breaks &&
              (byte == static_cast<unsigned char>('\t') ||
               byte == static_cast<unsigned char>('\n') ||
               byte == static_cast<unsigned char>('\r')))) {
            return false;
        }
    }
    return true;
}

bool isValidToolName(std::string_view name) {
    if (name.empty() || name.size() > kMaxToolNameBytes) {
        return false;
    }
    return std::all_of(
        name.begin(), name.end(), [](char raw) {
            const auto byte = static_cast<unsigned char>(raw);
            return (byte >= static_cast<unsigned char>('a') &&
                    byte <= static_cast<unsigned char>('z')) ||
                   (byte >= static_cast<unsigned char>('A') &&
                    byte <= static_cast<unsigned char>('Z')) ||
                   (byte >= static_cast<unsigned char>('0') &&
                    byte <= static_cast<unsigned char>('9')) ||
                   raw == '_' || raw == '-' || raw == '.';
        });
}

bool isValidCursor(const std::string& cursor) {
    return isSafeText(cursor, kMaxCursorBytes, false, false);
}

bool isValidRequestId(const nlohmann::json& id) {
    if (id.is_string()) {
        const auto& value = id.get_ref<const std::string&>();
        return isSafeText(value, kMaxIdBytes, false, false) &&
               value.compare(0U, kNumericBusinessIdPrefix.size(),
                             kNumericBusinessIdPrefix) != 0;
    }
    if (id.is_number_unsigned()) {
        return id.get<std::uint64_t>() <=
               static_cast<std::uint64_t>(
                   kMaxInteroperableInteger);
    }
    if (id.is_number_integer()) {
        const auto value = id.get<std::int64_t>();
        return value >= -kMaxInteroperableInteger &&
               value <= kMaxInteroperableInteger;
    }
    return false;
}

std::string canonicalBusinessRequestId(
    const nlohmann::json& id) {
    if (id.is_string()) {
        return id.get<std::string>();
    }
    if (id.is_number_unsigned()) {
        return std::string(kNumericBusinessIdPrefix) +
               std::to_string(id.get<std::uint64_t>());
    }
    return std::string(kNumericBusinessIdPrefix) +
           std::to_string(id.get<std::int64_t>());
}

Status validateJsonValue(const nlohmann::json& value,
                         const JsonBounds& bounds,
                         std::size_t depth,
                         std::size_t& node_count) {
    if (depth > bounds.max_depth ||
        node_count >= bounds.max_nodes) {
        return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                          "JSON value exceeds structural limits");
    }
    ++node_count;

    if (value.is_discarded() || value.is_binary()) {
        return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                          "JSON value uses an unsupported type");
    }
    if (value.is_string()) {
        if (!isSafeText(value.get_ref<const std::string&>(),
                        bounds.max_string_bytes, true, true)) {
            return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                              "JSON string is invalid or too large");
        }
        return Status::Ok();
    }
    if (value.is_number_float() &&
        !std::isfinite(value.get<double>())) {
        return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                          "JSON number must be finite");
    }
    if (value.is_array()) {
        if (value.size() > bounds.max_array_items) {
            return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                              "JSON array exceeds its item limit");
        }
        for (const auto& item : value) {
            const auto item_status = validateJsonValue(
                item, bounds, depth + 1U, node_count);
            if (!item_status.ok) {
                return item_status;
            }
        }
        return Status::Ok();
    }
    if (value.is_object()) {
        if (value.size() > bounds.max_object_members) {
            return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                              "JSON object exceeds its member limit");
        }
        for (const auto& item : value.items()) {
            if (!isSafeText(item.key(), 512U, true, false)) {
                return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                                  "JSON object key is invalid");
            }
            const auto item_status = validateJsonValue(
                item.value(), bounds, depth + 1U, node_count);
            if (!item_status.ok) {
                return item_status;
            }
        }
    }
    return Status::Ok();
}

Status validateJsonValue(const nlohmann::json& value,
                         const JsonBounds& bounds) {
    std::size_t node_count = 0;
    return validateJsonValue(value, bounds, 0U, node_count);
}

bool isForbiddenArgumentKey(std::string_view key) {
    static constexpr std::array<std::string_view, 19U> forbidden{
        "__proto__",
        "prototype",
        "constructor",
        "_meta",
        "runtime",
        "runtimeContext",
        "runtime_context",
        "trustedRuntime",
        "trusted_runtime",
        "caller_module_id",
        "deadline_mono_ns",
        "fencing_token",
        "tool_catalog_snapshot_id",
        "tool_digest",
        "policy_digest",
        "granted_permissions",
        "resource_lease_refs",
        "principal_id_hash",
        "authorization_ref"};
    return std::find(forbidden.begin(), forbidden.end(), key) !=
           forbidden.end();
}

bool containsForbiddenArgumentKey(
    const nlohmann::json& value) {
    if (value.is_object()) {
        for (const auto& item : value.items()) {
            if (isForbiddenArgumentKey(item.key()) ||
                containsForbiddenArgumentKey(item.value())) {
                return true;
            }
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (containsForbiddenArgumentKey(item)) {
                return true;
            }
        }
    }
    return false;
}

bool hasOnlyKeys(
    const nlohmann::json& object,
    std::initializer_list<std::string_view> allowed) {
    if (!object.is_object() || object.size() > allowed.size()) {
        return false;
    }
    for (const auto& item : object.items()) {
        const auto found =
            std::find(allowed.begin(), allowed.end(), item.key());
        if (found == allowed.end()) {
            return false;
        }
    }
    return true;
}

RawJsonPreflight preflightRawJson(std::string_view wire) {
    std::array<char, kMaxJsonNesting> open_delimiters{};
    std::size_t nesting = 0;
    bool in_string = false;
    bool escaped = false;
    for (const auto raw : wire) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (raw == '\\') {
                escaped = true;
            } else if (raw == '"') {
                in_string = false;
            }
            continue;
        }
        if (raw == '"') {
            in_string = true;
        } else if (raw == '{' || raw == '[') {
            if (nesting >= kMaxJsonNesting) {
                return RawJsonPreflight::TooDeep;
            }
            open_delimiters[nesting] = raw;
            ++nesting;
        } else if (raw == '}' || raw == ']') {
            if (nesting == 0U) {
                return RawJsonPreflight::Malformed;
            }
            const auto expected_open =
                raw == '}' ? '{' : '[';
            if (open_delimiters[nesting - 1U] !=
                expected_open) {
                return RawJsonPreflight::Malformed;
            }
            --nesting;
        }
    }
    if (in_string || escaped || nesting != 0U) {
        return RawJsonPreflight::Malformed;
    }
    return RawJsonPreflight::Acceptable;
}

nlohmann::json parseJsonRejectingDuplicateKeys(
    std::string_view wire, bool& duplicate_key) {
    std::vector<std::set<std::string>> object_key_stack;
    const nlohmann::json::parser_callback_t callback =
        [&object_key_stack, &duplicate_key](
            int, nlohmann::json::parse_event_t event,
            nlohmann::json& parsed) {
            if (event ==
                nlohmann::json::parse_event_t::object_start) {
                object_key_stack.emplace_back();
            } else if (event ==
                       nlohmann::json::parse_event_t::key) {
                if (object_key_stack.empty() ||
                    !object_key_stack.back()
                         .insert(parsed.get<std::string>())
                         .second) {
                    duplicate_key = true;
                }
            } else if (event ==
                       nlohmann::json::parse_event_t::object_end) {
                if (!object_key_stack.empty()) {
                    object_key_stack.pop_back();
                }
            }
            return true;
        };
    return nlohmann::json::parse(
        wire.begin(), wire.end(), callback, false, false);
}

Status validateCallArguments(const nlohmann::json& arguments) {
    if (!arguments.is_object()) {
        return wireStatus("ATOMIC_MCP_INVALID_PARAMS",
                          "tools/call arguments must be an object");
    }
    const JsonBounds bounds{kMaxArgumentNodes, kMaxArgumentDepth,
                            kMaxArgumentMembers,
                            kMaxArgumentArrayItems,
                            kMaxArgumentStringBytes};
    auto status = validateJsonValue(arguments, bounds);
    if (!status.ok) {
        status.error.code = "ATOMIC_MCP_INVALID_PARAMS";
        status.error.safe_detail_code =
            "ATOMIC_MCP_INVALID_PARAMS";
        return status;
    }
    if (containsForbiddenArgumentKey(arguments)) {
        return wireStatus(
            "ATOMIC_MCP_INVALID_PARAMS",
            "tools/call arguments contain a reserved field");
    }
    return Status::Ok();
}

Status validateTypedRequest(const McpWireRequest& request) {
    if (!isValidRequestId(request.id)) {
        return wireStatus("ATOMIC_MCP_INVALID_REQUEST",
                          "JSON-RPC id is invalid");
    }
    if (request.method == McpWireMethod::ToolsList) {
        if (!request.tool_name.empty() ||
            !request.arguments.is_object() ||
            !request.arguments.empty() ||
            (request.cursor &&
             !isValidCursor(*request.cursor))) {
            return wireStatus("ATOMIC_MCP_INVALID_PARAMS",
                              "tools/list request fields are invalid");
        }
        return Status::Ok();
    }
    if (request.cursor || !isValidToolName(request.tool_name)) {
        return wireStatus("ATOMIC_MCP_INVALID_PARAMS",
                          "tools/call request fields are invalid");
    }
    return validateCallArguments(request.arguments);
}

Status validateToolDefinition(
    const McpToolDefinition& definition) {
    if (!isValidToolName(definition.name) ||
        !isSafeText(definition.title, kMaxTitleBytes, true, true) ||
        !isSafeText(definition.description, kMaxDescriptionBytes,
                    false, true) ||
        !isSafeText(definition.annotations.title, kMaxTitleBytes,
                    true, true)) {
        return wireStatus("ATOMIC_MCP_RESULT_INVALID",
                          "MCP Tool text fields are invalid");
    }
    if (!definition.input_schema.is_object() ||
        definition.input_schema.value("type", "") != "object") {
        return wireStatus("ATOMIC_MCP_RESULT_INVALID",
                          "MCP inputSchema must describe an object");
    }
    if (!definition.output_schema.empty() &&
        (!definition.output_schema.is_object() ||
         definition.output_schema.value("type", "") != "object")) {
        return wireStatus("ATOMIC_MCP_RESULT_INVALID",
                          "MCP outputSchema must describe an object");
    }
    const JsonBounds schema_bounds{
        kMaxSchemaNodes, kMaxSchemaDepth, kMaxSchemaMembers,
        kMaxSchemaArrayItems, kMaxSchemaStringBytes};
    auto status =
        validateJsonValue(definition.input_schema, schema_bounds);
    if (!status.ok) {
        return wireStatus("ATOMIC_MCP_RESULT_INVALID",
                          "MCP inputSchema exceeds wire limits");
    }
    if (!definition.output_schema.empty()) {
        status = validateJsonValue(definition.output_schema,
                                   schema_bounds);
        if (!status.ok) {
            return wireStatus("ATOMIC_MCP_RESULT_INVALID",
                              "MCP outputSchema exceeds wire limits");
        }
    }
    return Status::Ok();
}

nlohmann::json toolDefinitionToJson(
    const McpToolDefinition& definition) {
    nlohmann::json annotations{
        {"readOnlyHint", definition.annotations.read_only_hint},
        {"destructiveHint",
         definition.annotations.destructive_hint},
        {"idempotentHint",
         definition.annotations.idempotent_hint},
        {"openWorldHint", definition.annotations.open_world_hint}};
    if (!definition.annotations.title.empty()) {
        annotations["title"] = definition.annotations.title;
    }

    nlohmann::json tool{
        {"name", definition.name},
        {"description", definition.description},
        {"inputSchema", definition.input_schema},
        {"annotations", std::move(annotations)}};
    if (!definition.title.empty()) {
        tool["title"] = definition.title;
    }
    if (!definition.output_schema.empty()) {
        tool["outputSchema"] = definition.output_schema;
    }
    return tool;
}

Result<std::string> dumpBounded(const nlohmann::json& value,
                                std::size_t max_bytes) {
    try {
        auto serialized = value.dump();
        if (serialized.size() > max_bytes) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_RESPONSE_TOO_LARGE",
                "MCP response exceeds the wire byte limit"));
        }
        return Result<std::string>::Success(
            std::move(serialized));
    } catch (...) {
        return Result<std::string>::Failure(wireStatus(
            "ATOMIC_MCP_SERIALIZATION_FAILED",
            "MCP response serialization failed"));
    }
}

const char* defaultErrorMessage(
    McpJsonRpcErrorCode error_code) {
    switch (error_code) {
        case McpJsonRpcErrorCode::ParseError:
            return "Parse error";
        case McpJsonRpcErrorCode::InvalidRequest:
            return "Invalid Request";
        case McpJsonRpcErrorCode::MethodNotFound:
            return "Method not found";
        case McpJsonRpcErrorCode::InvalidParams:
            return "Invalid params";
        case McpJsonRpcErrorCode::InternalError:
            return "Internal error";
    }
    return nullptr;
}

}  // namespace

McpWireParseResult McpProtocolAdapter::parseRequest(
    std::string_view wire_request) const {
    if (wire_request.empty()) {
        return parseFailure(
            "ATOMIC_MCP_PARSE_ERROR", "MCP request is empty",
            McpJsonRpcErrorCode::ParseError);
    }
    if (wire_request.size() > kMaxRequestBytes) {
        return parseFailure(
            "ATOMIC_MCP_INVALID_REQUEST",
            "MCP request exceeds the wire byte limit",
            McpJsonRpcErrorCode::InvalidRequest);
    }
    const auto preflight = preflightRawJson(wire_request);
    if (preflight == RawJsonPreflight::TooDeep) {
        return parseFailure(
            "ATOMIC_MCP_INVALID_REQUEST",
            "MCP request exceeds the nesting limit",
            McpJsonRpcErrorCode::InvalidRequest);
    }
    if (preflight == RawJsonPreflight::Malformed) {
        return parseFailure(
            "ATOMIC_MCP_PARSE_ERROR",
            "MCP request is not valid JSON",
            McpJsonRpcErrorCode::ParseError);
    }

    try {
        bool duplicate_key = false;
        const auto parsed = parseJsonRejectingDuplicateKeys(
            wire_request, duplicate_key);
        if (parsed.is_discarded()) {
            return parseFailure(
                "ATOMIC_MCP_PARSE_ERROR",
                "MCP request is not valid JSON",
                McpJsonRpcErrorCode::ParseError);
        }
        if (duplicate_key) {
            return parseFailure(
                "ATOMIC_MCP_INVALID_REQUEST",
                "MCP request contains a duplicate key",
                McpJsonRpcErrorCode::InvalidRequest);
        }
        if (!parsed.is_object() ||
            !hasOnlyKeys(parsed,
                         {"jsonrpc", "id", "method", "params"}) ||
            !parsed.contains("jsonrpc") ||
            !parsed.contains("id") ||
            !parsed.contains("method")) {
            return parseFailure(
                "ATOMIC_MCP_INVALID_REQUEST",
                "MCP request has an invalid top-level shape",
                McpJsonRpcErrorCode::InvalidRequest);
        }

        nlohmann::json response_id = nullptr;
        if (isValidRequestId(parsed["id"])) {
            response_id = parsed["id"];
        } else {
            return parseFailure(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC id is invalid",
                McpJsonRpcErrorCode::InvalidRequest);
        }
        if (!parsed["jsonrpc"].is_string() ||
            parsed["jsonrpc"] != "2.0" ||
            !parsed["method"].is_string()) {
            return parseFailure(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC version or method is invalid",
                McpJsonRpcErrorCode::InvalidRequest,
                response_id);
        }

        McpWireRequest request;
        request.id = response_id;
        const auto& method =
            parsed["method"].get_ref<const std::string&>();
        if (method == "tools/list") {
            request.method = McpWireMethod::ToolsList;
            if (parsed.contains("params")) {
                const auto& params = parsed["params"];
                if (!params.is_object() ||
                    !hasOnlyKeys(params, {"cursor"})) {
                    return parseFailure(
                        "ATOMIC_MCP_INVALID_PARAMS",
                        "tools/list params are invalid",
                        McpJsonRpcErrorCode::InvalidParams,
                        response_id);
                }
                if (params.contains("cursor")) {
                    if (!params["cursor"].is_string() ||
                        !isValidCursor(
                            params["cursor"].get_ref<
                                const std::string&>())) {
                        return parseFailure(
                            "ATOMIC_MCP_INVALID_PARAMS",
                            "tools/list cursor is invalid",
                            McpJsonRpcErrorCode::InvalidParams,
                            response_id);
                    }
                    request.cursor =
                        params["cursor"].get<std::string>();
                }
            }
        } else if (method == "tools/call") {
            request.method = McpWireMethod::ToolsCall;
            if (!parsed.contains("params") ||
                !parsed["params"].is_object() ||
                !hasOnlyKeys(parsed["params"],
                             {"name", "arguments"}) ||
                parsed["params"].size() != 2U ||
                !parsed["params"].contains("name") ||
                !parsed["params"].contains("arguments") ||
                !parsed["params"]["name"].is_string()) {
                return parseFailure(
                    "ATOMIC_MCP_INVALID_PARAMS",
                    "tools/call params are invalid",
                    McpJsonRpcErrorCode::InvalidParams,
                    response_id);
            }
            request.tool_name =
                parsed["params"]["name"].get<std::string>();
            request.arguments = parsed["params"]["arguments"];
            if (!isValidToolName(request.tool_name)) {
                return parseFailure(
                    "ATOMIC_MCP_INVALID_PARAMS",
                    "tools/call name is invalid",
                    McpJsonRpcErrorCode::InvalidParams,
                    response_id);
            }
            const auto arguments_status =
                validateCallArguments(request.arguments);
            if (!arguments_status.ok) {
                return parseFailure(
                    "ATOMIC_MCP_INVALID_PARAMS",
                    arguments_status.error.message,
                    McpJsonRpcErrorCode::InvalidParams,
                    response_id);
            }
        } else {
            return parseFailure(
                "ATOMIC_MCP_METHOD_NOT_FOUND",
                "JSON-RPC method is not supported",
                McpJsonRpcErrorCode::MethodNotFound,
                response_id);
        }

        McpWireParseResult result;
        result.status = Status::Ok();
        result.request = std::move(request);
        result.response_id = std::move(response_id);
        result.protocol_error =
            McpJsonRpcErrorCode::InvalidRequest;
        return result;
    } catch (...) {
        return parseFailure(
            "ATOMIC_MCP_INTERNAL_ERROR",
            "MCP request parsing failed safely",
            McpJsonRpcErrorCode::InternalError);
    }
}

Result<std::string> McpProtocolAdapter::serializeRequest(
    const McpWireRequest& request) const {
    try {
        const auto status = validateTypedRequest(request);
        if (!status.ok) {
            return Result<std::string>::Failure(status);
        }
        nlohmann::json params = nlohmann::json::object();
        std::string method;
        if (request.method == McpWireMethod::ToolsList) {
            method = "tools/list";
            if (request.cursor) {
                params["cursor"] = *request.cursor;
            }
        } else {
            method = "tools/call";
            params["name"] = request.tool_name;
            params["arguments"] = request.arguments;
        }
        const nlohmann::json wire{
            {"jsonrpc", "2.0"},
            {"id", request.id},
            {"method", std::move(method)},
            {"params", std::move(params)}};
        return dumpBounded(wire, kMaxRequestBytes);
    } catch (...) {
        return Result<std::string>::Failure(wireStatus(
            "ATOMIC_MCP_SERIALIZATION_FAILED",
            "MCP request serialization failed"));
    }
}

Result<McpCallToolRequest>
McpProtocolAdapter::toCallToolRequest(
    const McpWireRequest& request) const {
    try {
        const auto status = validateTypedRequest(request);
        if (!status.ok) {
            return Result<McpCallToolRequest>::Failure(status);
        }
        if (request.method != McpWireMethod::ToolsCall) {
            return Result<McpCallToolRequest>::Failure(wireStatus(
                "ATOMIC_MCP_INVALID_REQUEST",
                "only tools/call maps to McpCallToolRequest"));
        }
        McpCallToolRequest business_request;
        business_request.jsonrpc = "2.0";
        business_request.id =
            canonicalBusinessRequestId(request.id);
        business_request.method = "tools/call";
        business_request.name = request.tool_name;
        business_request.arguments = request.arguments;
        return Result<McpCallToolRequest>::Success(
            std::move(business_request));
    } catch (...) {
        return Result<McpCallToolRequest>::Failure(wireStatus(
            "ATOMIC_MCP_INTERNAL_ERROR",
            "MCP call mapping failed safely"));
    }
}

Result<std::string>
McpProtocolAdapter::serializeToolsListResult(
    const nlohmann::json& id,
    const std::vector<McpToolDefinition>& tools,
    const std::optional<std::string>& next_cursor) const {
    try {
        if (!isValidRequestId(id)) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC response id is invalid"));
        }
        if (tools.size() > kMaxToolsPerPage ||
            (next_cursor && !isValidCursor(*next_cursor))) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_RESULT_INVALID",
                "ListToolsResult page is invalid"));
        }

        nlohmann::json serialized_tools =
            nlohmann::json::array();
        for (const auto& definition : tools) {
            const auto status =
                validateToolDefinition(definition);
            if (!status.ok) {
                return Result<std::string>::Failure(status);
            }
            serialized_tools.push_back(
                toolDefinitionToJson(definition));
        }
        nlohmann::json result{
            {"tools", std::move(serialized_tools)}};
        if (next_cursor) {
            result["nextCursor"] = *next_cursor;
        }
        const nlohmann::json response{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", std::move(result)}};
        return dumpBounded(response, kMaxResponseBytes);
    } catch (...) {
        return Result<std::string>::Failure(wireStatus(
            "ATOMIC_MCP_SERIALIZATION_FAILED",
            "ListToolsResult serialization failed"));
    }
}

Result<std::string>
McpProtocolAdapter::serializeCallToolResult(
    const nlohmann::json& id,
    const CallToolResult& result) const {
    try {
        if (!isValidRequestId(id)) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC response id is invalid"));
        }
        if (result.text_content.size() >
            kMaxResultTextItems) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_RESULT_INVALID",
                "CallToolResult has too many content items"));
        }
        nlohmann::json content = nlohmann::json::array();
        for (const auto& text : result.text_content) {
            if (!isSafeText(text, kMaxResultTextBytes, true,
                            true)) {
                return Result<std::string>::Failure(wireStatus(
                    "ATOMIC_MCP_RESULT_INVALID",
                    "CallToolResult text is invalid"));
            }
            content.push_back(
                nlohmann::json{{"type", "text"},
                               {"text", text}});
        }

        const JsonBounds result_bounds{
            kMaxSchemaNodes, kMaxArgumentDepth,
            kMaxSchemaMembers, kMaxSchemaArrayItems,
            kMaxResultTextBytes};
        if (!result.structured_content.is_null() &&
            !result.structured_content.is_object()) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_RESULT_INVALID",
                "CallToolResult structuredContent must be an object"));
        }
        if (!result.structured_content.is_null()) {
            const auto status = validateJsonValue(
                result.structured_content, result_bounds);
            if (!status.ok) {
                return Result<std::string>::Failure(wireStatus(
                    "ATOMIC_MCP_RESULT_INVALID",
                    "CallToolResult structuredContent is invalid"));
            }
        }

        nlohmann::json protocol_result{
            {"content", std::move(content)},
            {"isError", result.is_error}};
        if (!result.structured_content.is_null()) {
            protocol_result["structuredContent"] =
                result.structured_content;
        }
        const nlohmann::json response{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"result", std::move(protocol_result)}};
        return dumpBounded(response, kMaxResponseBytes);
    } catch (...) {
        return Result<std::string>::Failure(wireStatus(
            "ATOMIC_MCP_SERIALIZATION_FAILED",
            "CallToolResult serialization failed"));
    }
}

Result<std::string>
McpProtocolAdapter::serializeErrorResponse(
    const nlohmann::json& id,
    McpJsonRpcErrorCode error_code,
    const std::string& safe_message) const {
    try {
        if (!id.is_null() && !isValidRequestId(id)) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC error response id is invalid"));
        }
        const auto* fixed_message =
            defaultErrorMessage(error_code);
        if (fixed_message == nullptr) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC error code is invalid"));
        }
        const std::string message =
            safe_message.empty() ? fixed_message : safe_message;
        if (!isSafeText(message, 256U, false, false)) {
            return Result<std::string>::Failure(wireStatus(
                "ATOMIC_MCP_INVALID_REQUEST",
                "JSON-RPC error message is invalid"));
        }
        const nlohmann::json response{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error",
             {{"code", static_cast<std::int32_t>(error_code)},
              {"message", message}}}};
        return dumpBounded(response, kMaxResponseBytes);
    } catch (...) {
        return Result<std::string>::Failure(wireStatus(
            "ATOMIC_MCP_SERIALIZATION_FAILED",
            "JSON-RPC error serialization failed"));
    }
}

}  // namespace master_agent::atomic_service
