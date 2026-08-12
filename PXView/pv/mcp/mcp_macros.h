// mcp_macros.h — Macros for the Struct API
//
// Part of the PXView MCP SDK.
//
// Provides:
//   MCP_SCHEMA(StructName)    — begin a schema registration block
//   MCP_SCHEMA_END            — end the block
//
// Usage:
//   MCP_SCHEMA(GetSamplesParams)
//       .req("channelIndex", &GetSamplesParams::channelIndex, "Channel index")
//       .opt("startSample",  &GetSamplesParams::startSample,  "Start sample")
//       .req_enum("channelType", &GetSamplesParams::channelType,
//                 {"logic", "analog", "dso"}, "Channel type");
//   MCP_SCHEMA_END
//
// The macro generates a template<> specialisation of mcp::schema_for<StructName>()
// that builds and caches the JSON Schema from the TypedSchemaBuilder chain.
// Member-pointer binding ensures renaming a struct field without updating
// the schema causes a compile error.
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include "mcp_schema.h"

// ──────────────────────────────────────────────────────────────────
//  MCP_SCHEMA / MCP_SCHEMA_END
// ──────────────────────────────────────────────────────────────────
//
//  Generates:
//    template<>
//    inline nlohmann::json mcp::schema_for<StructName>() {
//        static mcp::TypedSchemaBuilder<StructName> builder;
//        static bool init = false;
//        if (!init) {
//            init = true;
//            builder.req(...).opt(...);
//        }
//        return builder.build();
//    }
//
//  The static + init-flag pattern ensures the schema is built once and
//  cached for subsequent calls (mirrors Rust rmcp's TypeId cache).
//
#define MCP_SCHEMA(StructName)                                          \
    template<>                                                           \
    inline nlohmann::json mcp::schema_for<StructName>() {               \
        static mcp::TypedSchemaBuilder<StructName> _mcp_builder;        \
        static bool _mcp_init = false;                                  \
        if (!_mcp_init) {                                               \
            _mcp_init = true;                                           \
            _mcp_builder

#define MCP_SCHEMA_END                                                  \
            ;                                                          \
        }                                                              \
        return _mcp_builder.build();                                   \
    }

// ──────────────────────────────────────────────────────────────────
//  Convenience: register a struct tool with handler in one statement
// ──────────────────────────────────────────────────────────────────
//
//  Usage:
//    MCP_TOOL(server, GetSamplesParams, "get_samples", "Read raw samples",
//        [&](const GetSamplesParams& p) -> mcp::ToolResult {
//            // handler
//        });
//
#define MCP_TOOL(server, ParamStruct, toolName, toolDesc, handler)      \
    do {                                                                 \
        (server).tool<ParamStruct>(toolName, toolDesc);                 \
        (server).set_struct_handler<ParamStruct>(toolName,              \
            handler);                                                   \
    } while (0)
