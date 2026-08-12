// mcp_content.h — MCP content blocks and tool results
//
// Part of the PXView MCP SDK.
// Implements the MCP specification's CallToolResult structure:
//   - content: array of ContentBlock (text, image, or resource)
//   - isError: boolean flag
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <utility>

namespace mcp {

using json = nlohmann::json;

// ──────────────────────────────────────────────────────────────────
//  ContentBlock  —  one piece of tool output (text / image / resource)
// ──────────────────────────────────────────────────────────────────

struct ContentBlock {
    enum Type { Text, Image, Resource } type = Text;

    std::string text{};
    std::string image_data{};    // base64
    std::string mime_type{};
    std::string resource_uri{};
    json        resource_content{};

    // ── Constructors ──

    static ContentBlock text_content(std::string t) {
        return { Text, std::move(t), {}, {}, {}, {} };
    }

    static ContentBlock image_content(std::string base64, std::string mime) {
        return { Image, {}, std::move(base64), std::move(mime), {}, {} };
    }

    static ContentBlock resource_block(std::string uri, json content) {
        return { Resource, {}, {}, {}, std::move(uri), std::move(content) };
    }

    // ── Serialise to JSON (MCP wire format) ──

    json to_json() const {
        switch (type) {
        case Text:
            return {
                {"type", "text"},
                {"text", text}
            };
        case Image:
            return {
                {"type",  "image"},
                {"data",   image_data},
                {"mimeType", mime_type}
            };
        case Resource:
            return {
                {"type",     "resource"},
                {"resource", {
                    {"uri",     resource_uri},
                    {"content", resource_content}
                }}
            };
        }
        return {{"type", "text"}, {"text", ""}};
    }
};

// ──────────────────────────────────────────────────────────────────
//  ToolResult  —  what every tool handler returns (or throws)
// ──────────────────────────────────────────────────────────────────

class ToolResult {
public:
    // ── Success factories ──

    static ToolResult text(std::string msg) {
        ToolResult r;
        r.is_error_ = false;
        r.content_.push_back(ContentBlock::text_content(std::move(msg)));
        return r;
    }

    static ToolResult json_result(json j) {
        ToolResult r;
        r.is_error_ = false;
        r.content_.push_back(
            ContentBlock::text_content(j.is_string() ? j.get<std::string>()
                                                     : j.dump(2)));
        return r;
    }

    static ToolResult success(std::vector<ContentBlock> blocks) {
        ToolResult r;
        r.is_error_ = false;
        r.content_ = std::move(blocks);
        return r;
    }

    // ── Error factory (tool-level error — user-visible, isError=true) ──

    static ToolResult error(std::string msg) {
        ToolResult r;
        r.is_error_ = true;
        r.content_.push_back(ContentBlock::text_content(std::move(msg)));
        return r;
    }

    // ── Serialise to MCP tools/call result ──

    json to_json() const {
        json result;
        json arr = json::array();
        for (const auto& b : content_)
            arr.push_back(b.to_json());
        result["content"] = std::move(arr);
        result["isError"] = is_error_;
        return result;
    }

    bool is_error() const { return is_error_; }
    const std::vector<ContentBlock>& content() const { return content_; }

private:
    std::vector<ContentBlock> content_;
    bool is_error_ = false;
};

// ──────────────────────────────────────────────────────────────────
//  Convenience free functions
// ──────────────────────────────────────────────────────────────────

inline ToolResult text(std::string msg) {
    return ToolResult::text(std::move(msg));
}

inline ToolResult json_result(json j) {
    return ToolResult::json_result(std::move(j));
}

inline ToolResult error(std::string msg) {
    return ToolResult::error(std::move(msg));
}

} // namespace mcp
