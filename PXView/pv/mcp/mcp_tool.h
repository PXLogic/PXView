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
        return {
            {"name",        name},
            {"description", description},
            {"inputSchema", input_schema}
        };
    }

private:
    SchemaBuilder schema_builder_;
};

} // namespace mcp
