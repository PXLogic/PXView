#pragma once
#include "pv/api/types.h"
#include <string>
#include <map>
#include <functional>
#include <vector>
#include <cstdint>

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

    // P0-3: Binary response support. When is_binary is true, binary_payload
    // contains the raw binary frame data and result_json contains a small
    // JSON header describing the payload. The transport layer sends a JSON
    // text frame followed by a binary WebSocket frame.
    bool is_binary = false;
    std::vector<uint8_t> binary_payload;
    std::string binary_content_type;  // e.g. "application/octet-stream"
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

// ---- P1-3: Batch call support ----
// A single call within a batch_call request.
struct BatchCall {
    std::string method;
    std::string params_json;
};

// P1-1/P1-2: Viewport subscription descriptor.
// Each WS client can have at most one active viewport subscription.
struct ViewportSubscription {
    uint64_t start_sample = 0;
    uint64_t end_sample = 0;
    int32_t  width_px = 0;          // Target pixel width for the rendered viewport
    std::vector<int16_t> channels;  // Channel indices to include
    bool     is_active = false;     // True when subscription is active
    // P1-2: Delta frame tracking — last sample position sent to this client.
    // On each timer tick, only data beyond this position is sent as a delta.
    uint64_t last_sent_sample = 0;
};

} // namespace pv::api
