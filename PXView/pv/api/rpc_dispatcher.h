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
    JsonRpcResponse on_export_raw_data(int id, const nlohmann::json& params);
    JsonRpcResponse on_export_data_table_csv(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_capture_status(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_channels(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_analyzer_results(int id, const nlohmann::json& params);

    // ---- Batch A MCP tool handlers ----
    JsonRpcResponse on_get_trigger_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_trigger_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_probe_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_probe_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_channel_enabled_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_channel_name_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_time_base(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_collect_mode_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_repeat_interval(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_logic_samples_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_analog_samples_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_dso_samples_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_find_next_edge_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_find_pattern(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_active_decoders(int id, const nlohmann::json& params);
    JsonRpcResponse on_clear_all_decoders(int id, const nlohmann::json& params);
    JsonRpcResponse on_list_sessions(int id, const nlohmann::json& params);
    JsonRpcResponse on_create_session_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_destroy_session_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_active_session_mcp(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_session_count(int id, const nlohmann::json& params);
    JsonRpcResponse on_connect_device(int id, const nlohmann::json& params);
    JsonRpcResponse on_disconnect_device(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_glitch_filter_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_signal_invert_config(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_repeat_status(int id, const nlohmann::json& params);

    // ---- Batch B MCP tool handlers ----
    JsonRpcResponse on_refresh_device_list(int id, const nlohmann::json& params);
    JsonRpcResponse on_set_save_range(int id, const nlohmann::json& params);
    JsonRpcResponse on_reconfigure_decoder(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_decoder_class_names(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_decoder_binary_output(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_math_results(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_spectrum_results(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_lissajous_results(int id, const nlohmann::json& params);
    JsonRpcResponse on_get_error_state(int id, const nlohmann::json& params);
    JsonRpcResponse on_clear_error_state(int id, const nlohmann::json& params);

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
JsonRpcResponse on_clear_cursors(int id, const nlohmann::json& params);
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

    // ---- P1-3: Batch operation ----
    JsonRpcResponse on_batch_call(int id, const nlohmann::json& params);

    // ---- P0-3: Binary viewport data ----
    // Returns a binary frame containing logic edge or analog envelope data
    // for the specified viewport range. The response is sent as a JSON text
    // frame followed by a binary WebSocket frame.
    JsonRpcResponse on_get_viewport_binary(int id, const nlohmann::json& params);
};

} // namespace pv::api
