// mcp_schema.h — JSON Schema generation for the MCP SDK
//
// Part of the PXView MCP SDK.
//
// Provides:
//   ParamDesc         runtime parameter descriptor (Builder API)
//   SchemaBuilder     accumulates ParamDesc, generates inputSchema JSON
//   schema_for<T>()   template specialisation point for Struct API
//   get_cached_schema<T>()  cached schema accessor
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

// ParamDesc is now defined in mcp_params.h (so Params can access it
// for debug-build type_index verification).
#include "mcp_params.h"

#include <string>
#include <string_view>
#include <vector>
#include <typeindex>
#include <typeinfo>

namespace mcp {

using json = nlohmann::json;

// ──────────────────────────────────────────────────────────────────
//  SchemaBuilder  —  collects ParamDesc, builds inputSchema
// ──────────────────────────────────────────────────────────────────

class SchemaBuilder {
public:
    SchemaBuilder& add_param(ParamDesc desc) {
        params_.push_back(std::move(desc));
        return *this;
    }

    json build() const {
        json properties = json::object();
        json required   = json::array();

        for (const auto& p : params_) {
            json prop = json::object();
            // Only emit "type" when json_type is non-empty.
            // An empty json_type means "accept any type" (valid JSON Schema:
            // omitting the type keyword makes the schema permissive).
            if (!p.json_type.empty())
                prop["type"] = p.json_type;
            prop["description"] = p.description;

            if (p.json_type == "array" && !p.items_type.empty()) {
                prop["items"] = {{"type", p.items_type}};
            }
            if (p.has_default) {
                prop["default"] = p.default_value;
            }
            if (p.has_enum) {
                prop["enum"] = p.enum_values;
            }

            properties[p.name] = prop;
            if (p.required) {
                required.push_back(p.name);
            }
        }

        return {
            {"type",       "object"},
            {"properties", properties},
            {"required",   required}
        };
    }

    const std::vector<ParamDesc>& params() const { return params_; }
    bool empty() const { return params_.empty(); }

private:
    std::vector<ParamDesc> params_;
};

// ──────────────────────────────────────────────────────────────────
//  schema_for<T>()  —  Struct API specialisation point
// ──────────────────────────────────────────────────────────────────
//
//  Default returns an empty-object schema.
//  User code specialises this via the MCP_SCHEMA macro:
//
//    MCP_SCHEMA(MyParams)
//        .req("a", &MyParams::a, "First operand")
//        .opt("b", &MyParams::b, "Second operand");
//    MCP_SCHEMA_END(MyParams)
//
//  Or directly:
//
//    template<>
//    inline json mcp::schema_for<MyParams>() {
//        SchemaBuilder b;
//        b.add_param({.name="a", .json_type="integer", ...});
//        return b.build();
//    }
//
template <typename T>
inline json schema_for() {
    // Default: empty object schema
    return {
        {"type",       "object"},
        {"properties", json::object()}
    };
}

// ──────────────────────────────────────────────────────────────────
//  Cached schema accessor  —  generates once, caches as static
// ──────────────────────────────────────────────────────────────────

template <typename T>
const json& get_cached_schema() {
    static json schema = schema_for<T>();
    return schema;
}

// ──────────────────────────────────────────────────────────────────
//  TypedSchemaBuilder  —  member-pointer-bound schema builder
//  (used by MCP_SCHEMA macro for the Struct API)
// ──────────────────────────────────────────────────────────────────
//
//  Binding via member pointer means:
//    .req("deviceId", &StartCaptureParams::deviceId, "Device ID")
//  If the struct member is renamed, the schema won't compile.
//
template <typename S>
class TypedSchemaBuilder {
public:
    TypedSchemaBuilder() = default;

    // Required field — member pointer overload
    template <typename M>
    TypedSchemaBuilder& req(std::string_view name,
                             M S::* member_ptr,
                             std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type    = cpp_type_to_json<M>();
        p.description = std::string(desc);
        p.required    = true;
        p.cpp_type    = std::type_index(typeid(M));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Optional field
    template <typename M>
    TypedSchemaBuilder& opt(std::string_view name,
                             M S::* member_ptr,
                             std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = cpp_type_to_json<M>();
        p.description = std::string(desc);
        p.required    = false;
        p.cpp_type    = std::type_index(typeid(M));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Optional with default
    template <typename M>
    TypedSchemaBuilder& opt_def(std::string_view name,
                                  M S::* member_ptr,
                                  std::string_view desc,
                                  M default_val) {
        ParamDesc p;
        p.name         = std::string(name);
        p.json_type    = cpp_type_to_json<M>();
        p.description  = std::string(desc);
        p.required     = false;
        p.has_default  = true;
        p.default_value = default_val;
        p.cpp_type     = std::type_index(typeid(M));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Required enum field
    template <typename M>
    TypedSchemaBuilder& req_enum(std::string_view name,
                                   M S::* member_ptr,
                                   std::vector<std::string> values,
                                   std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = cpp_type_to_json<M>();
        p.description = std::string(desc);
        p.required    = true;
        p.has_enum    = true;
        for (auto& v : values) p.enum_values.push_back(v);
        p.cpp_type    = std::type_index(typeid(M));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Optional enum field
    template <typename M>
    TypedSchemaBuilder& opt_enum(std::string_view name,
                                   M S::* member_ptr,
                                   std::vector<std::string> values,
                                   std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = cpp_type_to_json<M>();
        p.description = std::string(desc);
        p.required    = false;
        p.has_enum    = true;
        for (auto& v : values) p.enum_values.push_back(v);
        p.cpp_type    = std::type_index(typeid(M));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Optional array field
    template <typename E>
    TypedSchemaBuilder& opt_arr(std::string_view name,
                                  std::vector<E> S::* member_ptr,
                                  std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = "array";
        p.items_type  = cpp_type_to_json<E>();
        p.description = std::string(desc);
        p.required    = false;
        p.cpp_type    = std::type_index(typeid(std::vector<E>));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Required array field
    template <typename E>
    TypedSchemaBuilder& req_arr(std::string_view name,
                                  std::vector<E> S::* member_ptr,
                                  std::string_view desc) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = "array";
        p.items_type  = cpp_type_to_json<E>();
        p.description = std::string(desc);
        p.required    = true;
        p.cpp_type    = std::type_index(typeid(std::vector<E>));
        builder_.add_param(std::move(p));
        return *this;
    }

    // Optional nested object field
    template <typename M>
    TypedSchemaBuilder& opt_obj(std::string_view name,
                                   M S::* member_ptr,
                                   std::string_view desc,
                                   const json& nested_schema) {
        ParamDesc p;
        p.name        = std::string(name);
        p.json_type   = "object";
        p.description = std::string(desc);
        p.required    = false;
        p.cpp_type    = std::type_index(typeid(M));
        (void)nested_schema;  // nested schema stored separately if needed
        builder_.add_param(std::move(p));
        return *this;
    }

    json build() { return builder_.build(); }

private:
    SchemaBuilder builder_;

    // Helper: C++ type → JSON Schema type name
    template <typename M>
    static std::string cpp_type_to_json() {
        if constexpr (std::is_same_v<M, bool>)
            return "boolean";
        else if constexpr (std::is_integral_v<M>)
            return "integer";
        else if constexpr (std::is_floating_point_v<M>)
            return "number";
        else if constexpr (std::is_same_v<M, std::string>)
            return "string";
        else if constexpr (std::is_same_v<M, nlohmann::json>)
            return "object";
        else
            return "object";
    }
};

} // namespace mcp
