#pragma once

#include "pv/api/transport.h"
#include "pv/api/iapp_service.h"
#include <nlohmann/json.hpp>
#include <memory>

// Forward declare the MCP SDK server (owned by RpcDispatcher)
namespace mcp { class McpServer; }

namespace pv::api {

class RpcDispatcher : public IJsonRpcHandler {
public:
    explicit RpcDispatcher(IAppService* app_svc);
    ~RpcDispatcher();

    JsonRpcResponse handle_request(const JsonRpcRequest& req) override;

private:
    IAppService* app_svc_;

    // MCP SDK server — owns all tool registrations, schema generation,
    // and exception-driven dispatch.
    std::unique_ptr<mcp::McpServer> mcp_server_;

    // Helper: build success / error responses
    static JsonRpcResponse success_resp(int id, const nlohmann::json& result);
    static JsonRpcResponse error_resp(int id, int code, const std::string& message);

    // Helper: wrap Result<T> into JsonRpcResponse
    template<typename T>
    static JsonRpcResponse wrap_result(int id, const Result<T>& r) {
        if (r) return success_resp(id, r.value());
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }

    // Helper: wrap Result<void> into JsonRpcResponse
    static JsonRpcResponse wrap_void(int id, const Result<void>& r) {
        if (r) return success_resp(id, nullptr);
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }

    // Helper: base64 encode raw bytes
    static std::string base64_encode(const std::vector<uint8_t>& data);

    // JSON serialization for data types
    static nlohmann::json to_json(const DeviceInfo& d);
    static nlohmann::json to_json(const ChannelInfo& c);
    static nlohmann::json to_json(const SampleConfig& s);
    static nlohmann::json to_json(const CaptureStatus& s);
    static nlohmann::json to_json(const TimeInfo& t);
    static nlohmann::json to_json(const DiskCacheInfo& d);
    static nlohmann::json to_json(const DecoderDescriptor& d);
    static nlohmann::json to_json(const DecoderAnnotation& a);
    static nlohmann::json to_json(const MeasurementValue& m);
    static nlohmann::json to_json(const CursorInfo& c);
    static nlohmann::json to_json(const SignalInfo& s);
    static nlohmann::json to_json(const LogicTriggerConfig& c);
    static nlohmann::json to_json(const DsoTriggerConfig& c);
    static nlohmann::json to_json(const ProbeConfig& p);
    static nlohmann::json to_json(const DecoderInstance& d);
    static nlohmann::json to_json(const GlitchFilterConfig& c);
    static nlohmann::json to_json(const SignalInvertConfig& c);
    // ---- Batch B result struct serialization ----
    static nlohmann::json to_json(const ErrorState& e);
    static nlohmann::json to_json(const MathResult& m);
    static nlohmann::json to_json(const SpectrumResult& s);
    static nlohmann::json to_json(const LissajousResult& l);
    static nlohmann::json to_json(const DecoderClassInfo& d);

    // ---- MCP protocol handlers ----
    JsonRpcResponse on_initialize(int id);
    JsonRpcResponse on_tools_list(int id);
    JsonRpcResponse on_ping(int id);

    // ---- MCP tool call handlers (mapped from MCP tool names) ----
    // These dispatch to the internal on_* handlers below
    JsonRpcResponse dispatch_mcp_tool(int id, const std::string& tool_name, const nlohmann::json& args);

};

} // namespace pv::api
