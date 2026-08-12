// mcp_tool.h — Tool descriptor and builder-method registration
//
// Part of the PXView MCP SDK.
//
// ToolDesc holds:
//   - name + description (tool metadata)
//   - input_schema (generated at registration time, cached)
//   - handler (std::function — Builder, Struct, or Void)
//   - SchemaBuilder (used by Builder API param<T>() calls)
//
// Template methods param<T>(), enum_param<T>(), array_param<T>()
// capture the C++ type at compile time, store type_index for
// runtime checking, and auto-generate the JSON Schema.
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include "mcp_schema.h"
#include "mcp_params.h"
#include "mcp_content.h"
#include "mcp_errors.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace mcp {

// ──────────────────────────────────────────────────────────────────
//  Handler function type aliases
// ──────────────────────────────────────────────────────────────────

// Builder API:    handler(const Params&)   -> ToolResult
using BuilderHandler = std::function<ToolResult(const Params&)>;

// Void API:       handler()                -> ToolResult
using VoidHandler = std::function<ToolResult()>;

// Struct API:     type-erased handler that takes raw json,
//                 internally does from_json<T> then calls the real handler
using StructHandler = std::function<ToolResult(const json&)>;

// ──────────────────────────────────────────────────────────────────
//  ToolDesc  —  one per registered tool
// ──────────────────────────────────────────────────────────────────

struct ToolDesc {
    std::string name{};
    std::string description{};
    json        input_schema{};
    json        output_schema{};      // MCP output schema (optional, empty = omitted)
    json        annotations{};       // MCP tool annotations (readOnlyHint, destructiveHint, etc.)

    // Handler variants (mutually exclusive — kind indicates which)
    BuilderHandler builder_handler{};
    StructHandler  struct_handler{};
    VoidHandler    void_handler{};

    enum class HandlerKind { Builder, Struct, Void };
    HandlerKind kind = HandlerKind::Void;

    // ── Builder API: parameter chain methods ──

    // Raw ParamDesc addition (internal use)
    ToolDesc& param_desc(ParamDesc desc) {
        schema_builder_.add_param(std::move(desc));
        return *this;
    }

    // Optional parameter:  param<T>("name", "desc")
    template <typename T>
    ToolDesc& param(std::string_view name, std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = json_type_name<T>::value;
        p.description = std::string(desc);
        p.required    = false;
        p.cpp_type    = std::type_index(typeid(T));
        return param_desc(std::move(p));
    }

    // Required parameter:  param<T>("name", "desc", Required)
    template <typename T>
    ToolDesc& param(std::string_view name, std::string_view desc,
                     Required_tag) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = json_type_name<T>::value;
        p.description = std::string(desc);
        p.required    = true;
        p.cpp_type    = std::type_index(typeid(T));
        return param_desc(std::move(p));
    }

    // Optional with default:  param<T>("name", "desc", Default_val(val))
    template <typename T>
    ToolDesc& param(std::string_view name, std::string_view desc,
                     Default_val<T> def) {
        ParamDesc p;
        p.name         = std::string(name);
        p.json_type     = json_type_name<T>::value;
        p.description  = std::string(desc);
        p.required     = false;
        p.has_default   = true;
        p.default_value = def.value;
        p.cpp_type     = std::type_index(typeid(T));
        return param_desc(std::move(p));
    }

    // Optional enum:  enum_param<T>("name", {"a","b"}, "desc")
    template <typename T>
    ToolDesc& enum_param(std::string_view name,
                          std::vector<std::string> values,
                          std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = json_type_name<T>::value;
        p.description = std::string(desc);
        p.required    = false;
        p.has_enum    = true;
        for (auto& v : values) p.enum_values.push_back(v);
        p.cpp_type    = std::type_index(typeid(T));
        return param_desc(std::move(p));
    }

    // Required enum:  enum_param<T>("name", {"a","b"}, "desc", Required)
    template <typename T>
    ToolDesc& enum_param(std::string_view name,
                          std::vector<std::string> values,
                          std::string_view desc,
                          Required_tag) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = json_type_name<T>::value;
        p.description = std::string(desc);
        p.required    = true;
        p.has_enum    = true;
        for (auto& v : values) p.enum_values.push_back(v);
        p.cpp_type    = std::type_index(typeid(T));
        return param_desc(std::move(p));
    }

    // Optional array:  array_param<T>("name", "desc")
    template <typename T>
    ToolDesc& array_param(std::string_view name,
                           std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = "array";
        p.items_type   = json_type_name<T>::value;
        p.description = std::string(desc);
        p.required    = false;
        p.cpp_type    = std::type_index(typeid(std::vector<T>));
        return param_desc(std::move(p));
    }

    // Required array:  array_param<T>("name", "desc", Required)
    template <typename T>
    ToolDesc& array_param(std::string_view name,
                           std::string_view desc,
                           Required_tag) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = "array";
        p.items_type   = json_type_name<T>::value;
        p.description = std::string(desc);
        p.required    = true;
        p.cpp_type    = std::type_index(typeid(std::vector<T>));
        return param_desc(std::move(p));
    }

    // ── Any-type parameters (accept any JSON value: object, array, etc.) ──
    // Replaces the typeid(void) hack for params like "analyzers", "value",
    // "channels" (multi-channel pattern), "types" (measurement types).
    //
    // The json_type parameter controls the schema's "type" field:
    //   "object" (default) — for dict/any-value params (options, channelMap, value)
    //   "array"            — for array params; pair with items_type for item schema
    //
    // is_any_type=true means the debug type-check is skipped (the handler
    // reads the value via p.raw() rather than p.get<T>()).

    // Optional any-type:  any_param("name", "desc")  →  type="object"
    // Optional any-type:  any_param("name", "desc", "array", "object")  →  type="array", items.type="object"
    ToolDesc& any_param(std::string_view name,
                         std::string_view desc,
                         std::string_view json_type = "object",
                         std::string_view items_type = "") {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = std::string(json_type);
        p.items_type  = std::string(items_type);
        p.description = std::string(desc);
        p.required    = false;
        p.is_any_type = true;
        return param_desc(std::move(p));
    }

    // Required any-type:  any_param("name", "desc", Required)
    // Required any-type:  any_param("name", "desc", Required, "array", "string")
    ToolDesc& any_param(std::string_view name,
                         std::string_view desc,
                         Required_tag,
                         std::string_view json_type = "object",
                         std::string_view items_type = "") {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = std::string(json_type);
        p.items_type  = std::string(items_type);
        p.description = std::string(desc);
        p.required    = true;
        p.is_any_type = true;
        return param_desc(std::move(p));
    }

    // ── Handler registration ──

    // Builder API handler
    ToolDesc& on_call(BuilderHandler h) {
        builder_handler = std::move(h);
        kind = HandlerKind::Builder;
        finalize_schema();
        return *this;
    }

    // Void handler (no params)
    ToolDesc& on_call(VoidHandler h) {
        void_handler = std::move(h);
        kind = HandlerKind::Void;
        // No schema for void tools — empty object
        input_schema = {{"type", "object"}, {"properties", json::object()}};
        return *this;
    }

    // Struct handler — set externally by McpServer::tool<P>()
    ToolDesc& on_call_struct(StructHandler h) {
        struct_handler = std::move(h);
        kind = HandlerKind::Struct;
        return *this;
    }

    // ── Output schema (optional, for structured output) ──
    // Sets the JSON Schema describing the tool's output structure.
    // When non-empty, it is included in the tool definition as
    // "outputSchema" (MCP 2025-03-26 spec).
    ToolDesc& set_output_schema(json schema) {
        output_schema = std::move(schema);
        return *this;
    }

    // ── Tool annotations (MCP spec hints for AI agents) ──
    // These help MCP clients decide whether to ask for user confirmation
    // before calling a tool, or whether to batch read-only calls.

    ToolDesc& read_only() {
        annotations = {{"readOnlyHint", true}};
        return *this;
    }

    ToolDesc& destructive() {
        annotations = {{"destructiveHint", true}, {"readOnlyHint", false}};
        return *this;
    }

    ToolDesc& idempotent() {
        annotations = {{"idempotentHint", true}};
        return *this;
    }

    ToolDesc& open_world() {
        annotations = {{"openWorldHint", true}};
        return *this;
    }

    // ── Schema finalisation ──

    void finalize_schema() {
        input_schema = schema_builder_.build();
    }

    // ── Param descriptors accessor ──
    // Used by McpServer to pass type info to Params for debug checks
    const std::vector<ParamDesc>& param_descs() const {
        return schema_builder_.params();
    }

    // ── Serialise to MCP tool definition (for tools/list) ──

    json to_tool_json() const {
        json j = {
            {"name",        name},
            {"description", description},
            {"inputSchema", input_schema}
        };
        if (!output_schema.is_null())
            j["outputSchema"] = output_schema;
        if (!annotations.is_null())
            j["annotations"] = annotations;
        return j;
    }

private:
    SchemaBuilder schema_builder_;
};

} // namespace mcp
