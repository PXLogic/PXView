// mcp_server.cpp — MCP server implementation
//
// Part of the PXView MCP SDK.
//
// Implements:
//   - handle_initialize()  → ServerInfo + capabilities + instructions
//   - handle_tools_list()  → tool schemas array
//   - handle_tools_call()  → exception-driven dispatch
//   - handle_request()     → unified entry point
//   - require_session_impl → session guard backing
//   - require_mode_impl    → mode guard backing
//   - base64_encode        → utility for binary data encoding
//
// Licensed under GPL v2 or (at your option) any later version.

#include "mcp_server.h"
#include "pv/api/iapp_service.h"
#include "pv/api/isession_service.h"
#include "pv/api/types.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace mcp {

// ──────────────────────────────────────────────────────────────────
//  Constructor
// ──────────────────────────────────────────────────────────────────

McpServer::McpServer(std::string_view name, std::string_view version)
    : name_(name), version_(version) {}

// ──────────────────────────────────────────────────────────────────
//  Tool registration (Builder + Void API)
// ──────────────────────────────────────────────────────────────────

ToolDesc& McpServer::tool(std::string_view name,
                           std::string_view description) {
    auto desc = std::make_unique<ToolDesc>();
    desc->name        = std::string(name);
    desc->description = std::string(description);
    size_t idx = tools_.size();
    tools_.push_back(std::move(desc));
    tool_index_[std::string(name)] = idx;
    return *tools_[idx];
}

ToolDesc& McpServer::tool_void(std::string_view name,
                                std::string_view description) {
    auto desc = std::make_unique<ToolDesc>();
    desc->name        = std::string(name);
    desc->description = std::string(description);
    desc->kind        = ToolDesc::HandlerKind::Void;
    desc->input_schema = {{"type", "object"}, {"properties", json::object()}};
    size_t idx = tools_.size();
    tools_.push_back(std::move(desc));
    tool_index_[std::string(name)] = idx;
    return *tools_[idx];
}

// ──────────────────────────────────────────────────────────────────
//  Instructions
// ──────────────────────────────────────────────────────────────────

void McpServer::set_instructions(std::string_view text) {
    instructions_ = std::string(text);
}

void McpServer::set_instructions_file(std::string_view path) {
    std::ifstream ifs{std::string(path)};
    if (!ifs.is_open()) {
        // If file not found, leave instructions empty — not a fatal error
        return;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    instructions_ = oss.str();
}

// ──────────────────────────────────────────────────────────────────
//  Protocol: initialize
// ──────────────────────────────────────────────────────────────────

json McpServer::handle_initialize() const {
    return {
        {"protocolVersion", "2025-03-26"},
        {"capabilities", {
            {"tools", {{"listChanged", false}}}
        }},
        {"serverInfo", {
            {"name", name_},
            {"version", version_}
        }},
        {"instructions", instructions_}
    };
}

// ──────────────────────────────────────────────────────────────────
//  Protocol: tools/list
// ──────────────────────────────────────────────────────────────────

json McpServer::handle_tools_list() const {
    json arr = json::array();
    for (const auto& t : tools_) {
        arr.push_back(t->to_tool_json());
    }
    return {{"tools", arr}};
}

// ──────────────────────────────────────────────────────────────────
//  Protocol: tools/call  (exception-driven dispatch)
// ──────────────────────────────────────────────────────────────────
//
//  This is the core of the SDK.  All handler errors are exceptions:
//    ToolError      → user-visible  (isError=true)
//    ProtocolError  → user-invisible (JSON-RPC error)
//    std::exception → user-invisible (InternalError)
//
json McpServer::handle_tools_call(std::string_view tool_name,
                                   const json& args) const {
    auto it = tool_index_.find(std::string(tool_name));
    if (it == tool_index_.end()) {
        return {
            {"isError", true},
            {"content", json::array({
                {{"type", "text"},
                 {"text",  "Unknown tool: " + std::string(tool_name)}}
            })}
        };
    }

    const auto& desc = *tools_[it->second];

    try {
        ToolResult result;

        // Static empty object — avoids dangling reference when args is not
        // an object (ternary would create a temporary json that is destroyed
        // at the semicolon, leaving Params with a dangling reference).
        static const json empty_args = json::object();

        switch (desc.kind) {
        case ToolDesc::HandlerKind::Builder: {
            const json& args_ref = args.is_object() ? args : empty_args;
            Params p(args_ref, &desc.param_descs());
            result = desc.builder_handler(p);
            break;
        }
        case ToolDesc::HandlerKind::Struct: {
            const json& args_ref = args.is_object() ? args : empty_args;
            result = desc.struct_handler(args_ref);
            break;
        }
        case ToolDesc::HandlerKind::Void: {
            result = desc.void_handler();
            break;
        }
        }

        return result.to_json();

    } catch (const ToolError& e) {
        // Tool-level error — user-visible
        return ToolResult::error(e.what()).to_json();

    } catch (const ProtocolError& e) {
        // Protocol-level error — user-invisible
        // Caller (handle_request) will wrap as JSON-RPC error
        throw;

    } catch (const std::exception& e) {
        // Uncaught exception — treat as internal error
        return ToolResult::error(
            std::string("Internal error: ") + e.what()).to_json();
    }
}

// ──────────────────────────────────────────────────────────────────
//  Protocol: ping
// ──────────────────────────────────────────────────────────────────

json McpServer::handle_ping() const {
    return json::object();  // empty result
}

// ──────────────────────────────────────────────────────────────────
//  Unified entry point
// ──────────────────────────────────────────────────────────────────

json McpServer::handle_request(std::string_view method,
                                const json& params,
                                int id) const {
    try {
        if (method == "initialize") {
            return make_success(handle_initialize(), id);
        }
        if (method == "tools/list") {
            return make_success(handle_tools_list(), id);
        }
        if (method == "tools/call") {
            std::string name = params.value("name", "");
            json args = params.value("arguments", json::object());
            json result = handle_tools_call(name, args);
            // tools/call result is the CallToolResult (content + isError),
            // wrapped in a JSON-RPC success response.
            return make_success(result, id);
        }
        if (method == "ping") {
            return make_success(handle_ping(), id);
        }

        // Unknown method
        return make_error(
            static_cast<int>(RpcErrorCode::MethodNotFound),
            "Unknown method: " + std::string(method), id);

    } catch (const ProtocolError& e) {
        return make_error(static_cast<int>(e.code()), e.what(), id);
    } catch (const std::exception& e) {
        return make_error(
            static_cast<int>(RpcErrorCode::InternalError),
            e.what(), id);
    }
}

// ──────────────────────────────────────────────────────────────────
//  Queries
// ──────────────────────────────────────────────────────────────────

const ToolDesc* McpServer::find_tool(std::string_view name) const {
    auto it = tool_index_.find(std::string(name));
    if (it == tool_index_.end())
        return nullptr;
    return tools_[it->second].get();
}

std::vector<std::string> McpServer::tool_names() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& t : tools_)
        names.push_back(t->name);
    return names;
}

// ──────────────────────────────────────────────────────────────────
//  Private: JSON-RPC response builders
// ──────────────────────────────────────────────────────────────────

json McpServer::make_error(int code, const std::string& message, int id) const {
    return {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"error",   {{"code", code}, {"message", message}}}
    };
}

json McpServer::make_success(const json& result, int id) const {
    return {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"result",  result}
    };
}

// ──────────────────────────────────────────────────────────────────
//  Session guard implementation
// ──────────────────────────────────────────────────────────────────

pv::api::ISessionService*
require_session_impl(pv::api::IAppService* app_svc) {
    if (!app_svc)
        return nullptr;
    return app_svc->get_active_session();
}

// ──────────────────────────────────────────────────────────────────
//  Mode guard implementation
// ──────────────────────────────────────────────────────────────────

void require_mode_impl(pv::api::ISessionService* session,
                        pv::api::WorkMode expected,
                        const std::string& tool_name) {
    if (!session)
        throw ToolError("No active session");

    auto current = session->get_work_mode();
    if (current == expected)
        return;

    const char* mode_names[] = {"Logic", "Analog", "DSO", "MSO", "Unknown"};
    int expected_idx = static_cast<int>(expected);
    int current_idx  = static_cast<int>(current);
    const char* expected_name = (expected_idx >= 0 && expected_idx <= 3)
                                ? mode_names[expected_idx] : "Unknown";
    const char* current_name  = (current_idx  >= 0 && current_idx  <= 3)
                                ? mode_names[current_idx]  : "Unknown";

    throw ToolError(tool_name + " requires " + expected_name
                    + " mode. Current: " + current_name
                    + ". Call switch_work_mode("
                    + std::to_string(static_cast<int>(expected))
                    + ").");
}

// ──────────────────────────────────────────────────────────────────
//  Utility: base64 encoding (for binary sample data)
// ──────────────────────────────────────────────────────────────────

namespace {

const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // anonymous namespace

std::string base64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = (static_cast<uint32_t>(data[i])     << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8)  |
                      static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Table[(n >> 18) & 0x3F]);
        out.push_back(kBase64Table[(n >> 12) & 0x3F]);
        out.push_back(kBase64Table[(n >>  6) & 0x3F]);
        out.push_back(kBase64Table[ n        & 0x3F]);
    }
    if (i < data.size()) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size())
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kBase64Table[(n >> 18) & 0x3F]);
        out.push_back(kBase64Table[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size())
                        ? kBase64Table[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// ──────────────────────────────────────────────────────────────────
//  Result<T> wrapper implementations
// ──────────────────────────────────────────────────────────────────

void check(const pv::api::Error& err) {
    throw ToolError(err.message);
}

void check_void(const pv::api::Result<void>& r) {
    if (!r) throw ToolError(r.error().message);
}

} // namespace mcp
