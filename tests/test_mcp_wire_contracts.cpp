/**
 * @file test_mcp_wire_contracts.cpp
 * @brief Verifies MCP JSON-RPC discovery, invocation, and bounded error contracts.
 */

#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/atomic_service/mcp_wire.h"
#include "test_support.h"

using namespace master_agent;
using namespace master_agent::atomic_service;
using master_agent::test_support::expect;

namespace {

void expectParseFailure(
    const McpProtocolAdapter& adapter,
    const std::string& wire,
    McpJsonRpcErrorCode expected_protocol_error,
    const std::string& message) {
    const auto parsed = adapter.parseRequest(wire);
    expect(!parsed.ok() && !parsed.status.ok &&
               parsed.protocol_error == expected_protocol_error,
           message);
}

void testToolsCallWireRoundTripAndBusinessMapping() {
    McpProtocolAdapter adapter;
    const std::string wire = R"json({
        "jsonrpc":"2.0",
        "id":"operation-K401",
        "method":"tools/call",
        "params":{
            "name":"com_sgm_service_climate_setAutoFanSpeed",
            "arguments":{"location":"FRONT","mode":"NORMAL"}
        }
    })json";

    const auto parsed = adapter.parseRequest(wire);
    expect(parsed.ok() && parsed.request &&
               parsed.request->method ==
                   McpWireMethod::ToolsCall &&
               parsed.request->id == "operation-K401" &&
               parsed.request->tool_name ==
                   "com_sgm_service_climate_setAutoFanSpeed" &&
               parsed.request->arguments["location"] == "FRONT",
           "tools/call wire request must parse into its exact MCP fields");

    const auto business =
        adapter.toCallToolRequest(*parsed.request);
    expect(business.status.ok && business.value &&
               business.value->jsonrpc == "2.0" &&
               business.value->id == "operation-K401" &&
               business.value->method == "tools/call" &&
               business.value->name ==
                   parsed.request->tool_name &&
               business.value->arguments ==
                   parsed.request->arguments,
           "wire adapter must map tools/call without changing the manager API");

    const auto serialized =
        adapter.serializeRequest(*parsed.request);
    expect(serialized.status.ok && serialized.value,
           "validated tools/call must serialize");
    const auto round_trip =
        adapter.parseRequest(*serialized.value);
    expect(round_trip.ok() && round_trip.request &&
               round_trip.request->id == parsed.request->id &&
               round_trip.request->tool_name ==
                   parsed.request->tool_name &&
               round_trip.request->arguments ==
                   parsed.request->arguments,
           "tools/call parse/serialize must round trip");

    const auto numeric = adapter.parseRequest(
        R"json({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"vehicle.read","arguments":{}}})json");
    expect(numeric.ok() && numeric.request &&
               numeric.request->id.is_number_integer(),
           "JSON-RPC integer IDs must remain integers on the wire");
    const auto numeric_business =
        adapter.toCallToolRequest(*numeric.request);
    expect(numeric_business.status.ok &&
               numeric_business.value &&
               numeric_business.value->id ==
                   "mcp-jsonrpc-int:7",
           "integer wire IDs must have a type-preserving business ID");
}

void testToolsListWireProjectionAndRoundTrip() {
    McpProtocolAdapter adapter;
    const auto parsed = adapter.parseRequest(
        R"json({"jsonrpc":"2.0","id":"list-1","method":"tools/list","params":{"cursor":"page-2"}})json");
    expect(parsed.ok() && parsed.request &&
               parsed.request->method ==
                   McpWireMethod::ToolsList &&
               parsed.request->cursor &&
               *parsed.request->cursor == "page-2",
           "tools/list cursor must parse as an opaque bounded string");

    const auto request_wire =
        adapter.serializeRequest(*parsed.request);
    expect(request_wire.status.ok && request_wire.value,
           "tools/list request must serialize");
    const auto request_json =
        nlohmann::json::parse(*request_wire.value);
    expect(request_json["method"] == "tools/list" &&
               request_json["params"]["cursor"] == "page-2",
           "tools/list request serialization must use standard params.cursor");

    const auto response = adapter.serializeToolsListResult(
        parsed.request->id, defaultClimateMcpTools(), "page-3");
    expect(response.status.ok && response.value,
           "registered MCP Tool definitions must serialize");
    const auto response_json =
        nlohmann::json::parse(*response.value);
    expect(response_json.size() == 3U &&
               response_json["jsonrpc"] == "2.0" &&
               response_json["id"] == "list-1" &&
               response_json["result"].size() == 2U &&
               response_json["result"]["nextCursor"] ==
                   "page-3" &&
               response_json["result"]["tools"].size() == 2U,
           "ListToolsResult must contain only tools and optional nextCursor");

    const auto& tool = response_json["result"]["tools"][0];
    expect(tool["name"] ==
                   "com_sgm_service_climate_setAirCirculationMode" &&
               tool["inputSchema"]["type"] == "object" &&
               tool["outputSchema"]["type"] == "object" &&
               tool["annotations"]["title"] ==
                   "setAirCirculationMode" &&
               tool["annotations"]["idempotentHint"] == true &&
               !tool.contains("tool_digest") &&
               !tool.contains("policy_digest") &&
               !tool.contains("provider_id") &&
               !response_json["result"].contains("snapshot_id"),
           "tools/list must expose MCP definitions without trusted governance fields");

    const auto no_params = adapter.parseRequest(
        R"json({"jsonrpc":"2.0","id":8,"method":"tools/list"})json");
    expect(no_params.ok() && no_params.request &&
               !no_params.request->cursor,
           "standard tools/list requests may omit empty params");
}

void testCallToolResultAndErrorProjection() {
    McpProtocolAdapter adapter;
    CallToolResult result;
    result.structured_content =
        nlohmann::json{{"success", true},
                       {"appliedMode", "NORMAL"},
                       {"errorCode", ""}};
    result.text_content.push_back(
        result.structured_content.dump());
    result.is_error = false;

    const auto serialized =
        adapter.serializeCallToolResult(9, result);
    expect(serialized.status.ok && serialized.value,
           "CallToolResult must serialize");
    const auto response =
        nlohmann::json::parse(*serialized.value);
    expect(response["id"].is_number_integer() &&
               response["id"] == 9 &&
               response["result"]["content"][0]["type"] ==
                   "text" &&
               response["result"]["content"][0]["text"] ==
                   result.text_content[0] &&
               response["result"]["structuredContent"] ==
                   result.structured_content &&
               response["result"]["isError"] == false,
           "CallToolResult must use MCP content/structuredContent/isError");

    const auto error = adapter.serializeErrorResponse(
        "list-9", McpJsonRpcErrorCode::MethodNotFound);
    expect(error.status.ok && error.value,
           "standard JSON-RPC errors must serialize");
    const auto error_json = nlohmann::json::parse(*error.value);
    expect(error_json["id"] == "list-9" &&
               error_json["error"]["code"] == -32601 &&
               error_json["error"]["message"] ==
                   "Method not found" &&
               !error_json.contains("result"),
           "JSON-RPC error response must preserve a trusted ID and code");
}

void testRejectsMalformedAndDangerousWireShapes() {
    McpProtocolAdapter adapter;
    expectParseFailure(
        adapter, "{",
        McpJsonRpcErrorCode::ParseError,
        "malformed JSON must be a JSON-RPC parse error");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"a","id":"b","method":"tools/list","params":{}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "duplicate JSON object keys must be rejected");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"a","method":"tools/call","params":{"name":"vehicle.read","arguments":{"mode":"A","mode":"B"}}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "duplicate nested argument keys must be rejected before mapping");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"m","method":"resources/list","params":{}})json",
        McpJsonRpcErrorCode::MethodNotFound,
        "unknown JSON-RPC methods must be rejected");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"x","method":"tools/list","params":{},"runtime":{}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "extra top-level fields must be rejected");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":null,"method":"tools/list","params":{}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "null request IDs must be rejected");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":1.5,"method":"tools/list","params":{}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "fractional request IDs must be rejected");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"mcp-jsonrpc-int:7","method":"tools/list","params":{}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "the numeric business-ID namespace must be reserved");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":9007199254740992,"method":"tools/list","params":{}})json",
        McpJsonRpcErrorCode::InvalidRequest,
        "non-interoperable integer request IDs must be rejected");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"l","method":"tools/list","params":{"cursor":"a","extra":1}})json",
        McpJsonRpcErrorCode::InvalidParams,
        "tools/list must reject unknown params");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"c","method":"tools/call","params":{"name":"vehicle.read"}})json",
        McpJsonRpcErrorCode::InvalidParams,
        "tools/call must require arguments");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"c","method":"tools/call","params":{"name":"vehicle.read","arguments":[]}})json",
        McpJsonRpcErrorCode::InvalidParams,
        "tools/call arguments must be an object");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"c","method":"tools/call","params":{"name":"vehicle.read","arguments":{"runtime":{"priority":"P0"}}}})json",
        McpJsonRpcErrorCode::InvalidParams,
        "untrusted runtime context must not enter arguments");
    expectParseFailure(
        adapter,
        R"json({"jsonrpc":"2.0","id":"c","method":"tools/call","params":{"name":"vehicle.read","arguments":{"payload":{"__proto__":{"admin":true}}}}})json",
        McpJsonRpcErrorCode::InvalidParams,
        "prototype-pollution-shaped arguments must be rejected");

    std::string deeply_nested =
        R"json({"jsonrpc":"2.0","id":"deep","method":"tools/call","params":{"name":"vehicle.read","arguments":{"value":)json";
    for (std::size_t index = 0; index < 40U; ++index) {
        deeply_nested.push_back('[');
    }
    deeply_nested += "0";
    for (std::size_t index = 0; index < 40U; ++index) {
        deeply_nested.push_back(']');
    }
    deeply_nested += "}}}";
    expectParseFailure(
        adapter, deeply_nested,
        McpJsonRpcErrorCode::InvalidRequest,
        "deeply nested JSON must be rejected before recursive validation");

    CallToolResult non_finite;
    non_finite.structured_content =
        nlohmann::json{{"value",
                        std::numeric_limits<double>::quiet_NaN()}};
    const auto rejected_result =
        adapter.serializeCallToolResult("result-1", non_finite);
    expect(!rejected_result.status.ok,
           "non-finite programmatic JSON values must not serialize as MCP data");

    CallToolResult scalar_result;
    scalar_result.structured_content = 42;
    const auto rejected_scalar =
        adapter.serializeCallToolResult("result-2", scalar_result);
    expect(!rejected_scalar.status.ok,
           "MCP structuredContent must remain a JSON object");
}

}  // namespace

int main() {
    testToolsCallWireRoundTripAndBusinessMapping();
    testToolsListWireProjectionAndRoundTrip();
    testCallToolResultAndErrorProjection();
    testRejectsMalformedAndDangerousWireShapes();
    return 0;
}
