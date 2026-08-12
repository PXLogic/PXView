// mcp_server.h — MCP server: tool registration, schema, dispatch
//
// Part of the PXView MCP SDK.
//
// McpServer is the central orchestrator:
//   1. Tool registration (Builder API, Struct API, Void API)
//   2. Schema generation (tools/list)
//   3. Exception-driven dispatch (tools/call)
//   4. Protocol handling (initialize, ping)
//
// Usage:
//   mcp::McpServer server("PXView", "1.5.7");
//   server.set_instructions_file("pv/mcp/mcp_instructions.txt");
//
//   // Builder API
//   server.tool("get_devices", "List connected devices")
//       .param<bool>("includeSimulationDevices", "Include simulation devices")
//       .on_call([&](const mcp::Params& p) {
//           return mcp::json_result(app_svc->get_device_list());
//       });
//
//   // Struct API
//   server.tool<StartCaptureParams>("start_capture", "Start a capture")
//       .on_call_struct([&](const StartCaptureParams& p) {
//           return mcp::text("started");
//       });
//
//   // Void API (no params)
//   server.tool_void("stop_capture", "Stop the current capture")
//       .on_call([&]() {
//           return mcp::text("stopped");
//       });
//
//   // Protocol dispatch
//   json resp = server.handle_request(method, params, id);
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include "mcp_tool.h"
#include "mcp_params.h"
#include "mcp_schema.h"
#include "mcp_content.h"
#include "mcp_errors.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace mcp {

using json = nlohmann::json;

// ──────────────────────────────────────────────────────────────────
//  McpServer
// ──────────────────────────────────────────────────────────────────

class McpServer {
public:
    McpServer(std::string_view name, std::string_view version);

    // ── Tool registration: Builder API ──
    // Returns a reference to the ToolDesc for chaining param<T>().on_call()
    ToolDesc& tool(std::string_view name, std::string_view description);

    // ── Tool registration: Void API (no params) ──
    ToolDesc& tool_void(std::string_view name, std::string_view description);

    // ── Tool registration: Struct API ──
    // Template parameter P must have a schema_for<P>() specialisation
    // (via MCP_SCHEMA macro).  The handler receives const P&.
    template <typename P>
    ToolDesc& tool(std::string_view name, std::string_view description) {
        auto desc = std::make_unique<ToolDesc>();
        desc->name        = std::string(name);
        desc->description = std::string(description);
        desc->input_schema = get_cached_schema<P>();
        desc->kind        = ToolDesc::HandlerKind::Struct;

        size_t idx = tools_.size();
        tools_.push_back(std::move(desc));
        tool_index_[std::string(name)] = idx;

        // The on_call handler is set separately via on_call_struct().
        // McpServer stores a StructHandler that does from_json<P> → call.

        return *tools_[idx];
    }

    // Set the struct handler — wraps a lambda taking const P&
    template <typename P, typename F>
    void set_struct_handler(std::string_view name, F&& handler) {
        auto it = tool_index_.find(std::string(name));
        if (it == tool_index_.end())
            throw std::runtime_error("Tool not found: " + std::string(name));

        auto& desc = *tools_[it->second];
        desc.on_call_struct(
            [h = std::forward<F>(handler)](const json& j) -> ToolResult {
                try {
                    P params = j.get<P>();
                    return h(params);
                } catch (const json::exception& e) {
                    throw ToolError(std::string("Parameter parsing failed: ")
                                    + e.what());
                }
            }
        );
    }

    // ── Instructions ──
    void set_instructions(std::string_view text);
    void set_instructions_file(std::string_view path);
    const std::string& instructions() const { return instructions_; }

    // ── Protocol handlers ──

    // initialize — returns ServerInfo + capabilities + instructions
    json handle_initialize() const;

    // tools/list — returns all tool schemas
    json handle_tools_list() const;

    // tools/call — dispatches to handler, catches exceptions
    json handle_tools_call(std::string_view tool_name,
                            const json& args) const;

    // ping — returns empty result
    json handle_ping() const;

    // ── Unified entry point ──
    // Dispatches based on method name: initialize, tools/list, tools/call, ping
    // Returns the JSON-RPC result (success or error)
    json handle_request(std::string_view method,
                         const json& params,
                         int id) const;

    // ── Queries ──
    const ToolDesc* find_tool(std::string_view name) const;
    size_t tool_count() const { return tools_.size(); }
    std::vector<std::string> tool_names() const;

private:
    std::string name_;
    std::string version_;
    std::string instructions_;

    std::vector<std::unique_ptr<ToolDesc>> tools_;
    std::unordered_map<std::string, size_t> tool_index_;

    json make_error(int code, const std::string& message, int id) const;
    json make_success(const json& result, int id) const;
};

// ──────────────────────────────────────────────────────────────────
//  Convenience: Struct API tool registration with handler in one call
// ──────────────────────────────────────────────────────────────────
//
//  Usage:
//    server.tool_with<StartCaptureParams>(
//        "start_capture", "Start a capture",
//        [&](const StartCaptureParams& p) -> mcp::ToolResult {
//            // handler
//        });
//
template <typename P, typename Server, typename F>
void register_struct_tool(Server& server,
                           std::string_view name,
                           std::string_view description,
                           F&& handler) {
    server.template tool<P>(name, description);
    server.template set_struct_handler<P>(name, std::forward<F>(handler));
}

// ──────────────────────────────────────────────────────────────────
//  Utility: base64 encoding (for binary sample data)
// ──────────────────────────────────────────────────────────────────

std::string base64_encode(const std::vector<uint8_t>& data);

} // namespace mcp
