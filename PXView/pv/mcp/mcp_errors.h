// mcp_errors.h — Error types and session guards for the MCP SDK
//
// Part of the PXView MCP SDK.
//
// Two-tier error model (mirrors the Rust rmcp SDK):
//   ToolError      — tool-level error, user-visible (isError=true in the
//                    CallToolResult).  Use when the tool ran but failed.
//   ProtocolError  — protocol-level error, user-invisible (JSON-RPC error
//                    response).  Use when the request itself is malformed.
//
// Session guard:
//   require_session(app_svc)  — returns ISessionService*, throws ToolError
//                               if no active session.  Eliminates the 49
//                               manual "No active session" checks.
//   require_mode(session, mode)  — throws ToolError if the session is not
//                                   in the expected work mode.
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include <stdexcept>
#include <string>
#include <cstdint>
#include <functional>

// Forward declarations — avoids pulling in the full API headers
namespace pv::api {
class IAppService;
class ISessionService;
enum class WorkMode : int8_t;
enum class ErrorCode : int8_t;
struct Error;

template <typename T> class Result;
} // namespace pv::api

namespace mcp {

// ──────────────────────────────────────────────────────────────────
//  JSON-RPC error codes (subset relevant to the SDK)
// ──────────────────────────────────────────────────────────────────

enum class RpcErrorCode : int32_t {
    ParseError      = -32700,
    InvalidRequest  = -32600,
    MethodNotFound  = -32601,
    InvalidParams   = -32602,
    InternalError   = -32603,
    // PXView application-level codes (offset from -32000)
    ResourceNotFound  = -32002,
    MissingDevice     = -32001,
    ConfigNotSupported = -32003,
};

// ──────────────────────────────────────────────────────────────────
//  ToolError  —  thrown by handlers, caught by dispatch, user-visible
// ──────────────────────────────────────────────────────────────────

class ToolError : public std::runtime_error {
public:
    explicit ToolError(const std::string& msg)
        : std::runtime_error(msg) {}

    ToolError(const std::string& context, const std::string& detail)
        : std::runtime_error(context + ": " + detail) {}
};

// ──────────────────────────────────────────────────────────────────
//  ProtocolError  —  thrown by handlers, caught by dispatch,
//  produces a JSON-RPC error response (user-invisible)
// ──────────────────────────────────────────────────────────────────

class ProtocolError : public std::runtime_error {
public:
    ProtocolError(RpcErrorCode code, const std::string& msg)
        : std::runtime_error(msg), code_(code) {}

    RpcErrorCode code() const { return code_; }

private:
    RpcErrorCode code_;
};

// ──────────────────────────────────────────────────────────────────
//  Session guard  —  eliminates 49× "No active session" boilerplate
// ──────────────────────────────────────────────────────────────────
//
//  Usage:
//    auto session = mcp::require_session(app_svc);
//    // session is ISessionService* — guaranteed non-null
//    auto r = session->start_capture();
//    if (!r) throw mcp::ToolError(r.error().message);
//

// Implementation — defined in mcp_server.cpp (has access to IAppService)
pv::api::ISessionService* require_session_impl(pv::api::IAppService* app_svc);

inline pv::api::ISessionService*
require_session(pv::api::IAppService* app_svc) {
    auto* s = require_session_impl(app_svc);
    if (!s)
        throw ToolError("No active session. "
                        "Call create_session or connect_device first.");
    return s;
}

// ──────────────────────────────────────────────────────────────────
//  Mode guard  —  for mode-specific tools
// ──────────────────────────────────────────────────────────────────
//
//  Usage:
//    mcp::require_mode(session, pv::api::WorkMode::Logic);
//    // If session is in Analog mode, throws ToolError with guidance.
//

void require_mode_impl(pv::api::ISessionService* session,
                        pv::api::WorkMode expected,
                        const std::string& tool_name);

inline void
require_mode(pv::api::ISessionService* session,
             pv::api::WorkMode expected,
             const std::string& tool_name = "this tool") {
    require_mode_impl(session, expected, tool_name);
}

// ──────────────────────────────────────────────────────────────────
//  Result<T> wrappers  —  convert Result<T> to ToolResult or throw
//  Implementations in mcp_server.cpp (needs full pv::api::Error and
//  Result<void> definitions from types.h)
// ──────────────────────────────────────────────────────────────────

// Convert a Result<T> error branch into a ToolError throw
void check(const pv::api::Error& err);

// Wrap Result<void> — throws on failure, returns void on success
void check_void(const pv::api::Result<void>& r);

} // namespace mcp
