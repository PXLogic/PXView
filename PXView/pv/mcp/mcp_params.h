// mcp_params.h — Type-safe parameter extraction for the MCP SDK
//
// Part of the PXView MCP SDK.  Provides:
//   - json_type_name<T>     compile-time C++ type → JSON Schema type mapping
//   - ParamDesc              runtime parameter descriptor (type, required, default, enum)
//   - mcp::Params           wrapper around nlohmann::json for safe extraction
//   - JSON Pointer support  nested access via "a/b/c" paths
//   - Required / Default / Enum modifiers
//   - Debug-build type_index verification (catches schema/handler drift)
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <typeinfo>
#include <typeindex>
#include <unordered_map>
#include <stdexcept>

// Error types (need full definition for throw expressions)
#include "mcp_errors.h"

namespace mcp {

using json = nlohmann::json;

// ──────────────────────────────────────────────────────────────────
//  Type-mapping traits  —  C++ type  →  JSON Schema type string
// ──────────────────────────────────────────────────────────────────

template <typename T>
struct json_type_name {
    static constexpr const char* value = "object";
    static constexpr const char* items  = "";
    static constexpr bool is_array     = false;
    static constexpr bool is_optional  = false;
};

template <> struct json_type_name<bool>           { static constexpr const char* value = "boolean"; static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<int16_t>        { static constexpr const char* value = "integer";  static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<int32_t>        { static constexpr const char* value = "integer";  static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<int64_t>        { static constexpr const char* value = "integer";  static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<uint16_t>       { static constexpr const char* value = "integer";  static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<uint32_t>       { static constexpr const char* value = "integer";  static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<uint64_t>       { static constexpr const char* value = "integer";  static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<double>         { static constexpr const char* value = "number";   static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<float>          { static constexpr const char* value = "number";   static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };
template <> struct json_type_name<std::string>    { static constexpr const char* value = "string";   static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };

// nlohmann::json itself → "object" (arbitrary JSON value)
template <> struct json_type_name<json>          { static constexpr const char* value = "object";   static constexpr const char* items = ""; static constexpr bool is_array = false; static constexpr bool is_optional = false; };

// optional<T> → T's type, marked optional
template <typename T>
struct json_type_name<std::optional<T>> {
    static constexpr const char* value    = json_type_name<T>::value;
    static constexpr const char* items    = json_type_name<T>::items;
    static constexpr bool is_array     = json_type_name<T>::is_array;
    static constexpr bool is_optional  = true;
};

// vector<T> → "array" with items = T's type
template <typename T>
struct json_type_name<std::vector<T>> {
    static constexpr const char* value = "array";
    static constexpr const char* items = json_type_name<T>::value;
    static constexpr bool is_array     = true;
    static constexpr bool is_optional  = false;
};

// ──────────────────────────────────────────────────────────────────
//  Modifier types
// ──────────────────────────────────────────────────────────────────

struct Required_tag {};
inline constexpr Required_tag Required{};

template <typename T>
struct Default_val {
    T value;
    explicit Default_val(T v) : value(std::move(v)) {}
};
template <typename T> Default_val(T) -> Default_val<T>;

template <typename T>
struct Enum_val {
    std::vector<T> values;
};

// ──────────────────────────────────────────────────────────────────
//  ParamDesc  —  runtime parameter descriptor
// ──────────────────────────────────────────────────────────────────
//
//  Filled in by ToolDesc::param<T>() template methods.
//  The cpp_type field stores std::type_index(typeid(T)) for
//  runtime type-safety checking in debug builds.
//
struct ParamDesc {
    std::string     name;
    std::string     json_type;       // "integer", "string", "boolean", "number", "array", "object"
    std::string     items_type;      // for arrays: type of elements
    std::string     description;
    bool            required    = false;
    bool            has_default = false;
    json            default_value;
    bool            has_enum    = false;
    std::vector<json> enum_values;
    std::type_index cpp_type   = std::type_index(typeid(void));
};

// ──────────────────────────────────────────────────────────────────
//  Params  —  type-safe parameter extraction wrapper
// ──────────────────────────────────────────────────────────────────
//
//  Wraps a const json& and provides template-based extraction
//  with automatic error reporting.  Supports JSON Pointer paths
//  for nested objects:  p.get<int>("logicDeviceConfiguration/digitalSampleRate")
//
//  When constructed with a ParamDesc list (from ToolDesc), debug builds
//  verify that the requested C++ type matches the registered type_index.
//  This catches schema/handler drift at runtime in debug mode.
//
class Params {
public:
    // Basic constructor — no type checking (backward compatible)
    explicit Params(const json& j)
        : data_(j), type_checks_(nullptr) {}

    // Constructor with type checking — pass &ToolDesc::param_descs()
    Params(const json& j, const std::vector<ParamDesc>* desc)
        : data_(j), type_checks_(desc) {}

    // Required parameter — throws ToolError if missing or wrong type
    template <typename T>
    T get(std::string_view key) const {
        check_type<T>(key);
        const json& val = resolve(key);
        try {
            return val.get<T>();
        } catch (const json::exception& e) {
            throw ToolError(std::string("Invalid type for '")
                + std::string(key) + "': " + e.what());
        }
    }

    // Optional parameter — returns default if missing
    template <typename T>
    T get_or(std::string_view key, T default_val) const {
        check_type<T>(key);
        const json* val = resolve_optional(key);
        if (!val) return default_val;
        try {
            return val->get<T>();
        } catch (const json::exception& e) {
            throw ToolError(std::string("Invalid type for '")
                + std::string(key) + "': " + e.what());
        }
    }

    // Optional parameter — returns std::nullopt if missing
    template <typename T>
    std::optional<T> get_opt(std::string_view key) const {
        check_type<T>(key);
        const json* val = resolve_optional(key);
        if (!val) return std::nullopt;
        try {
            return val->get<T>();
        } catch (const json::exception& e) {
            throw ToolError(std::string("Invalid type for '")
                + std::string(key) + "': " + e.what());
        }
    }

    // Array parameter — required
    template <typename T>
    std::vector<T> get_array(std::string_view key) const {
        return get<std::vector<T>>(key);
    }

    // Array parameter — optional with default
    template <typename T>
    std::vector<T> get_array_or(std::string_view key,
                                 std::vector<T> default_val) const {
        return get_or<std::vector<T>>(key, std::move(default_val));
    }

    // Check parameter presence
    bool has(std::string_view key) const {
        return resolve_optional(key) != nullptr;
    }

    // Raw JSON access
    const json& raw() const { return data_; }

    // Number of top-level keys
    size_t size() const { return data_.size(); }

private:
    const json& data_;
    const std::vector<ParamDesc>* type_checks_;

    // Debug-build type verification
    // If type_checks_ is set, look up the param by name and verify
    // that the requested type_index matches the registered one.
    // This catches the case where param<int16_t>("x") was declared
    // in the schema but the handler calls p.get<std::string>("x").
    template <typename T>
    void check_type(std::string_view key) const {
        if (!type_checks_) return;

        // Only check top-level keys (no '/' in key)
        if (key.find('/') != std::string_view::npos) return;

        std::string key_str(key);
        for (const auto& desc : *type_checks_) {
            if (desc.name == key_str) {
                auto requested = std::type_index(typeid(T));
                if (desc.cpp_type != requested) {
                    // Type mismatch detected!
                    throw ToolError(
                        std::string("Type mismatch for '") + key_str
                        + "': schema registered "
                        + desc.cpp_type.name()
                        + " but handler requests "
                        + requested.name()
                    );
                }
                return;
            }
        }
        // Key not in the registered params — could be a nested path
        // or an unregistered param.  Don't fail, let the actual
        // json.get<T>() call handle it.
    }

    // Resolve a key that may be a JSON Pointer path (e.g. "a/b/c").
    // Returns nullptr if the path does not exist.
    const json* resolve_optional(std::string_view key) const {
        if (key.empty())
            return &data_;

        // Fast path: no '/' in key — direct lookup
        if (key.find('/') == std::string_view::npos) {
            auto it = data_.find(key);
            if (it == data_.end())
                return nullptr;
            return &(*it);
        }

        // Slow path: JSON Pointer-style path
        const json* cur = &data_;
        size_t start = 0;
        while (start <= key.size()) {
            auto pos = key.find('/', start);
            std::string_view segment;
            if (pos == std::string_view::npos) {
                segment = key.substr(start);
                start   = key.size() + 1;   // done
            } else {
                segment = key.substr(start, pos - start);
                start   = pos + 1;
            }
            if (segment.empty())
                continue;   // leading or trailing '/'

            if (!cur->is_object())
                return nullptr;
            auto it = cur->find(segment);
            if (it == cur->end())
                return nullptr;
            cur = &(*it);
        }
        return cur;
    }

    // Resolve a required key — throws ToolError if missing
    const json& resolve(std::string_view key) const {
        const json* val = resolve_optional(key);
        if (!val)
            throw ToolError(std::string("Missing required parameter: ")
                             + std::string(key));
        return *val;
    }
};

} // namespace mcp
