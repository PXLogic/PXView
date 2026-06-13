#pragma once
#include "types.h"
#include <string>
#include <map>
#include <functional>

namespace pv::api {

// ---- JSON-RPC / MCP protocol structures ----

struct JsonRpcRequest {
    std::string method;
    std::string params_json;  // Raw JSON string for flexibility
    int id = 0;
    bool has_id = false;      // MCP notifications have no "id" field
    bool is_mcp = false;      // True when routed through MCP transport
    std::string mcp_tool_name;      // For MCP tools/call: the tool name from params.name
    std::string mcp_tool_args;      // For MCP tools/call: the arguments from params.arguments
};

struct JsonRpcResponse {
    std::string result_json;  // Raw JSON string on success
    std::string error_json;   // Raw JSON string on failure
    int id = 0;
    bool success = true;
    bool is_mcp_direct = false;  // If true, result_json is already the full MCP result (not wrapped in content)
    bool is_mcp_error = false;   // If true, this is an MCP error (content + isError)
};

// ---- ITransport — transport layer interface ----

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
};

// ---- IJsonRpcHandler — JSON-RPC request handler ----

class IJsonRpcHandler {
public:
    virtual ~IJsonRpcHandler() = default;
    virtual JsonRpcResponse handle_request(const JsonRpcRequest& req) = 0;
};

} // namespace pv::api
