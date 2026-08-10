#pragma once

/**
 * @file mcp_wire.h
 * @brief Defines MCP-compatible tool discovery and invocation payloads.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "master_agent/atomic_service/atomic_service.h"
#include "master_agent/common/types.h"

namespace master_agent::atomic_service {

enum class McpWireMethod : std::uint8_t {
    ToolsList,
    ToolsCall
};

enum class McpJsonRpcErrorCode : std::int32_t {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603
};

struct McpWireRequest {
    nlohmann::json id;
    McpWireMethod method = McpWireMethod::ToolsList;
    std::optional<std::string> cursor;
    std::string tool_name;
    nlohmann::json arguments = nlohmann::json::object();
};

struct McpWireParseResult {
    Status status;
    std::optional<McpWireRequest> request;
    nlohmann::json response_id = nullptr;
    McpJsonRpcErrorCode protocol_error =
        McpJsonRpcErrorCode::InvalidRequest;

    bool ok() const noexcept {
        return status.ok && request.has_value();
    }
};

/**
 * @brief Converts between JSON-RPC MCP messages and typed atomic-service calls.
 *
 * Parsing is strict: unsupported methods, malformed identifiers, unknown fields,
 * and schema violations produce bounded JSON-RPC errors.
 */
class McpProtocolAdapter final {
public:
    static constexpr std::size_t kMaxRequestBytes = 64U * 1024U;
    static constexpr std::size_t kMaxResponseBytes = 1024U * 1024U;
    static constexpr std::size_t kMaxToolsPerPage = 256U;

    /// Parses and bounds one JSON-RPC request without invoking a tool.
    McpWireParseResult parseRequest(
        std::string_view wire_request) const;

    /// Serializes a typed request with the canonical MCP method and field names.
    Result<std::string> serializeRequest(
        const McpWireRequest& request) const;

    /// Converts a validated tools/call request into the internal MCP contract.
    Result<McpCallToolRequest> toCallToolRequest(
        const McpWireRequest& request) const;

    Result<std::string> serializeToolsListResult(
        const nlohmann::json& id,
        const std::vector<McpToolDefinition>& tools,
        const std::optional<std::string>& next_cursor =
            std::nullopt) const;

    Result<std::string> serializeCallToolResult(
        const nlohmann::json& id,
        const CallToolResult& result) const;

    /// Returns a bounded JSON-RPC error and never exposes internal diagnostics.
    Result<std::string> serializeErrorResponse(
        const nlohmann::json& id,
        McpJsonRpcErrorCode error_code,
        const std::string& safe_message = {}) const;
};

}  // namespace master_agent::atomic_service
