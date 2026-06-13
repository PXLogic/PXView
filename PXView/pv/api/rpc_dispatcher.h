#pragma once

#include "transport.h"
#include "iapp_service.h"
#include <nlohmann/json.hpp>

namespace pv::api {

class RpcDispatcher : public IJsonRpcHandler {
public:
    explicit RpcDispatcher(IAppService* app_svc);

    JsonRpcResponse handle_request(const JsonRpcRequest& req) override;

    // MCP tool schema definitions
    static nlohmann::json get_tool_schemas();

private:
    IAppService* app_svc_;

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

    // ---- MCP protocol handlers ----
    JsonRpcResponse on_initialize(int id);
    JsonRpcResponse on_tools_list(int id);
    JsonRpcResponse on_ping(int id);

    // ---- MCP tool call handlers (mapped from MCP tool names) ----
    // These dispatch to the internal on_* handlers below
    JsonRpcResponse dispatch_mcp_tool(int id, const std::string& tool_name, const nlohmann::json& args);

    // ---- Internal tool handlers ----
    JsonRpcResponse on_get_devices(int id, const nlohmann::json& params);
    JsonRpcResponse on_start_capture(int id, const nlohmann::json& params);
    JsonRpcResponse on_stop_capture(int id, const nlohmann::json& params);
    JsonRpcResponse on_wait_capture(int id, const nlohmann::json& params);
    JsonRpcResponse on_load_capture(int id, const nlohmann::json& params);
    JsonRpcResponse on_save_capture(int id, const nlohmann::json& params);
    JsonRpcResponse on_close_capture(int id, const nlohmann::json& params);
    JsonRpcResponse on_add_analyzer(int id, const nlohmann::json& params);
    JsonRpcResponse on_remove_analyzer(int id, const nlohmann::json& params);
    JsonRpcResponse on_list_analyzers(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_analyzer_options(int id, const nlohmann::json& params);
    JsonRpcResponse on_export_raw_data_csv(int id, const nlohmann::json& params);
    JsonRpcResponse on_export_raw_data_binary(int id, const nlohmann::json& params);
    JsonRpcResponse on_export_data_table_csv(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_capture_status(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_channels(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_analyzer_results(int id, const nlohmann::json& params);

    // ---- Legacy handlers (still used by WebSocket transport) ----
    JsonRpcResponse on_get_sample_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_sample_rate(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_sample_limit(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_collect_mode(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_logic_waveform(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_analog_waveform(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_dso_waveform(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_available_decoders(int id, const nlohmann::json& params);
    JsonRpcResponse on_add_decoder(int id, const nlohmann::json& params);
    JsonRpcResponse on_remove_decoder(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_decoder_annotations(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_measurements(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_cursors(int id, const nlohmann::json& params);
    JsonRpcResponse on_add_cursor(int id, const nlohmann::json& params);
    JsonRpcResponse on_remove_cursor(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_glitch_filter(int id, const nlohmann::json& params);
    JsonRpcResponse on_clear_glitch_filter(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_signal_invert(int id, const nlohmann::json& params);
    JsonRpcResponse on_clear_signal_invert(int id, const nlohmann::json& params);
    JsonRpcResponse on_save_file(int id, const nlohmann::json& params);
    JsonRpcResponse on_load_file(int id, const nlohmann::json& params);
    JsonRpcResponse on_export_data(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_time_info(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_disk_cache_info(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_device_info(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_work_mode(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_signal_list(int id, const nlohmann::json& params);
    JsonRpcResponse on_find_next_edge(int id, const nlohmann::json& params);
};

} // namespace pv::api
