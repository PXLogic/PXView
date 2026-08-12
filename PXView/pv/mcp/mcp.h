// mcp.h — PXView MCP SDK  (umbrella header)
//
// Include this single header to use the entire SDK.
//
// Quick start (Builder API):
// ┌──────────────────────────────────────────────────────────────┐
// │ #include "pv/mcp/mcp.h"                                      │
// │                                                              │
// │ mcp::McpServer server("PXView", "1.5.7");                    │
// │ server.set_instructions_file("pv/mcp/mcp_instructions.txt");│
// │                                                              │
// │ server.tool("get_samples", "Read raw samples")               │
// │     .param<int16_t>("channelIndex", "Channel index", mcp::Required) │
// │     .param<uint64_t>("startSample", "Start sample")         │
// │     .enum_param<std::string>("channelType",                 │
// │         {"logic", "analog", "dso"}, "Channel type", mcp::Required)│
// │     .on_call([&](const mcp::Params& p) -> mcp::ToolResult { │
// │         auto session = mcp::require_session(app_svc);        │
// │         auto ch = p.get<int16_t>("channelIndex");           │
// │         // ...                                               │
// │         return mcp::json_result({{"sample_count", 42}});    │
// │     });                                                      │
// │                                                              │
// │ // Struct API (complex tools):                               │
// │ server.tool<StartCaptureParams>("start_capture", "...")     │
// │     // handler set via set_struct_handler                    │
// │                                                              │
// │ // Void API (no params):                                     │
// │ server.tool_void("stop_capture", "Stop capture")             │
// │     .on_call([&]() { return mcp::text("stopped"); });       │
// │                                                              │
// │ // Dispatch:                                                 │
// │ json resp = server.handle_request(method, params, id);      │
// └──────────────────────────────────────────────────────────────┘
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

// Core types
#include "mcp_params.h"       // Params, ParamDesc, json_type_name<T>, Required, Default
#include "mcp_content.h"      // ContentBlock, ToolResult, text(), json_result()
#include "mcp_errors.h"       // ToolError, ProtocolError, require_session, require_mode

// Schema + tool registration
#include "mcp_schema.h"       // SchemaBuilder, schema_for<T>(), TypedSchemaBuilder
#include "mcp_tool.h"         // ToolDesc, param<T>(), enum_param<T>(), on_call()
#include "mcp_macros.h"       // MCP_SCHEMA / MCP_SCHEMA_END / MCP_TOOL macros

// Server
#include "mcp_server.h"       // McpServer — tool registration + dispatch
