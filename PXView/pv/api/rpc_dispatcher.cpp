#include "pv/api/rpc_dispatcher.h"
#include "pv/api/binary_codec.h"
#include "pv/api/session_service.h"
#include "pv/mcp/mcp.h"
#include <algorithm>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QDateTime>
#include "PXView/config.h"

// MCP tool registry — creates and configures the McpServer with all tools
namespace mcp {
std::unique_ptr<McpServer> create_mcp_server(pv::api::IAppService* app_svc);
}

namespace pv::api {

// Debug log helper - writes to temp file since GUI app stderr is unreliable
static void mcp_dbg_log(const char* msg) {
    static QFile dbg_file;
    if (!dbg_file.isOpen()) {
        dbg_file.setFileName(QDir::tempPath() + "/pxview_automation_debug.log");
        (void)dbg_file.open(QIODevice::WriteOnly | QIODevice::Append);
    }
    if (dbg_file.isOpen()) {
        dbg_file.write(msg);
        dbg_file.write("\n");
        dbg_file.flush();
    }
}

using json = nlohmann::json;

// ---- Base64 encoding ----

static const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string RpcDispatcher::base64_encode(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                      static_cast<uint32_t>(data[i + 2]);
        out.push_back(kBase64Table[(n >> 18) & 0x3F]);
        out.push_back(kBase64Table[(n >> 12) & 0x3F]);
        out.push_back(kBase64Table[(n >> 6) & 0x3F]);
        out.push_back(kBase64Table[n & 0x3F]);
    }
    if (i < data.size()) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < data.size())
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(kBase64Table[(n >> 18) & 0x3F]);
        out.push_back(kBase64Table[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < data.size()) ? kBase64Table[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

// ---- Response helpers ----

JsonRpcResponse RpcDispatcher::success_resp(int id, const json& result) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = true;
    resp.result_json = result.dump();
    return resp;
}

JsonRpcResponse RpcDispatcher::error_resp(int id, int code, const std::string& message) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = false;
    json err = {{"code", code}, {"message", message}};
    resp.error_json = err.dump();
    return resp;
}

// ---- JSON serialization ----

json RpcDispatcher::to_json(const DeviceInfo& d) {
    return json{
        {"id",                d.id},
        {"driver_name",       d.driver_name},
        {"display_name",      d.display_name},
        {"path",              d.path},
        {"is_hardware",       d.is_hardware},
        {"is_demo",           d.is_demo},
        {"is_file",           d.is_file},
        {"is_virtual",        d.is_virtual},
        {"is_hardware_logic", d.is_hardware_logic},
        {"is_hardware_dso",   d.is_hardware_dso},
        {"is_dsl_device",     d.is_dsl_device},
        {"is_compat_device",  d.is_compat_device},
        {"usb_speed",         d.usb_speed}
    };
}

json RpcDispatcher::to_json(const ChannelInfo& c) {
    return json{
        {"index",           c.index},
        {"name",            c.name},
        {"type",            static_cast<int>(c.type)},
        {"enabled",         c.enabled},
        {"enabled_default", c.enabled_default}
    };
}

json RpcDispatcher::to_json(const SampleConfig& s) {
    return json{
        {"sample_rate",         s.sample_rate},
        {"sample_limit",        s.sample_limit},
        {"time_base",           s.time_base},
        {"collect_mode",        static_cast<int>(s.collect_mode)},
        {"stream_mode",         s.stream_mode},
        {"rle_enabled",         s.rle_enabled},
        {"repeat_interval",     s.repeat_interval},
        {"repeat_hold_percent", s.repeat_hold_percent}
    };
}

json RpcDispatcher::to_json(const CaptureStatus& s) {
    // Map numeric CaptureState enum to human-readable string.
    // Tests and MCP clients expect string values: idle, capturing, completed, etc.
    const char* state_str = "idle";
    switch (s.state) {
        case CaptureState::Empty:    state_str = "idle";       break;
        case CaptureState::Starting: state_str = "capturing";  break;
        case CaptureState::Recording:state_str = "capturing";  break;
        case CaptureState::Stopping: state_str = "capturing";  break;
        case CaptureState::Stopped:  state_str = "completed";  break;
        case CaptureState::Paused:   state_str = "paused";     break;
        case CaptureState::Error:    state_str = "error";      break;
    }
    return json{
        {"state",                   state_str},
        {"state_code",              static_cast<int>(s.state)},
        {"is_instant",              s.is_instant},
        {"is_saving",               s.is_saving},
        {"have_view_data",          s.have_view_data},
        {"have_hardware_data",      s.have_hardware_data},
        {"have_decoded_result",     s.have_decoded_result},
        {"is_copy_in_progress",     s.is_copy_in_progress},
        {"is_glitch_filter_active", s.is_glitch_filter_active},
        {"is_signal_invert_active", s.is_signal_invert_active},
        {"progress",                s.progress},
        {"triggered",               s.triggered}
    };
}

json RpcDispatcher::to_json(const TimeInfo& t) {
    return json{
        {"session_start_ms",   t.session_start_ms},
        {"trigger_pos",        t.trigger_pos},
        {"trigger_time_ms",    t.trigger_time_ms},
        {"is_triggered",       t.is_triggered},
        {"session_duration_sec", t.session_duration_sec},
        {"view_time_sec",      t.view_time_sec},
        {"sample_time_sec",    t.sample_time_sec}
    };
}

json RpcDispatcher::to_json(const DiskCacheInfo& d) {
    return json{
        {"enabled",            d.enabled},
        {"write_speed_mbps",   d.write_speed_mbps},
        {"write_queue_depth",  d.write_queue_depth},
        {"is_disk_full",       d.is_disk_full}
    };
}

json RpcDispatcher::to_json(const DecoderDescriptor& d) {
    json ch_arr = json::array();
    for (const auto& ch : d.channel_info) {
        ch_arr.push_back(json{
            {"id",          ch.id},
            {"name",        ch.name},
            {"desc",        ch.desc},
            {"order",       ch.order},
            {"is_optional", ch.is_optional}
        });
    }
    return json{
        {"id",                d.id},
        {"name",              d.name},
        {"long_name",         d.long_name},
        {"channels",          d.channels},
        {"optional_channels", d.optional_channels},
        {"channel_info",      ch_arr}
    };
}

json RpcDispatcher::to_json(const DecoderAnnotation& a) {
    return json{
        {"start_sample", a.start_sample},
        {"end_sample",   a.end_sample},
        {"ann_class",    a.ann_class},
        {"texts",        a.texts}
    };
}

json RpcDispatcher::to_json(const MeasurementValue& m) {
    return json{
        {"type",  m.type},
        {"value", m.value},
        {"unit",  m.unit},
        {"valid", m.valid}
    };
}

json RpcDispatcher::to_json(const CursorInfo& c) {
    return json{
        {"index",      c.index},
        {"sample_pos", c.sample_pos},
        {"time_sec",   c.time_sec}
    };
}

json RpcDispatcher::to_json(const SignalInfo& s) {
    return json{
        {"index",   s.index},
        {"name",    s.name},
        {"type",    static_cast<int>(s.type)},
        {"enabled", s.enabled},
        {"color",   s.color}
    };
}

json RpcDispatcher::to_json(const LogicTriggerConfig& c) {
    return json{
        {"stage_count", c.stage_count},
        {"config_json", c.config_json}
    };
}

json RpcDispatcher::to_json(const DsoTriggerConfig& c) {
    return json{
        {"source",    static_cast<int>(c.source)},
        {"slope",     static_cast<int>(c.slope)},
        {"horiz_pos", c.horiz_pos},
        {"holdoff",   c.holdoff},
        {"margin",    c.margin},
        {"channel",   c.channel}
    };
}

json RpcDispatcher::to_json(const ProbeConfig& p) {
    return json{
        {"vdiv",        p.vdiv},
        {"coupling",    static_cast<int>(p.coupling)},
        {"vfactor",     p.vfactor},
        {"map_default", p.map_default}
    };
}

json RpcDispatcher::to_json(const DecoderInstance& d) {
    return json{
        {"instance_id",  d.instance_id},
        {"decoder_id",   d.decoder_id},
        {"display_name", d.display_name},
        {"row_index",    d.row_index},
        {"is_running",   d.is_running},
        {"progress",     d.progress}
    };
}

json RpcDispatcher::to_json(const GlitchFilterConfig& c) {
    json ch_arr = json::array();
    json th_arr = json::array();
    json md_arr = json::array();
    for (size_t i = 0; i < c.channels.size(); i++) {
        ch_arr.push_back(c.channels[i]);
        if (i < c.thresholds.size())
            th_arr.push_back(c.thresholds[i]);
        if (i < c.modes.size())
            md_arr.push_back(static_cast<int>(c.modes[i]));
    }
    return json{
        {"channels",   ch_arr},
        {"thresholds", th_arr},
        {"modes",      md_arr}
    };
}

json RpcDispatcher::to_json(const SignalInvertConfig& c) {
    json ch_arr = json::array();
    json st_arr = json::array();
    for (size_t i = 0; i < c.channels.size(); i++) {
        ch_arr.push_back(c.channels[i]);
        if (i < c.invert_states.size())
            st_arr.push_back(c.invert_states[i]);
    }
    return json{
        {"channels",      ch_arr},
        {"invert_states", st_arr}
    };
}

// ---- Batch B result struct serialization ----

json RpcDispatcher::to_json(const ErrorState& e) {
    return json{
        {"has_error",     e.has_error},
        {"error_code",    e.error_code},
        {"error_pattern", e.error_pattern},
        {"error_message", e.error_message}
    };
}

json RpcDispatcher::to_json(const MathResult& m) {
    return json{
        {"is_enabled", m.is_enabled},
        {"ch1_index",  m.ch1_index},
        {"ch2_index",  m.ch2_index},
        {"math_type",  m.math_type},
        {"sample_num", m.sample_num},
        {"samples",    m.samples}
    };
}

json RpcDispatcher::to_json(const SpectrumResult& s) {
    return json{
        {"is_enabled",      s.is_enabled},
        {"channel_index",   s.channel_index},
        {"sample_num",      s.sample_num},
        {"windows_index",   s.windows_index},
        {"dc_ignored",      s.dc_ignored},
        {"sample_interval", s.sample_interval},
        {"spectrum",        s.spectrum}
    };
}

json RpcDispatcher::to_json(const LissajousResult& l) {
    return json{
        {"is_enabled", l.is_enabled},
        {"x_index",    l.x_index},
        {"y_index",    l.y_index},
        {"percent",    l.percent}
    };
}

json RpcDispatcher::to_json(const DecoderClassInfo& d) {
    return json{
        {"class_id",   d.class_id},
        {"class_name", d.class_name}
    };
}

// ---- Constructor ----

RpcDispatcher::RpcDispatcher(IAppService* app_svc)
    : app_svc_(app_svc)
    , mcp_server_(mcp::create_mcp_server(app_svc)) {}

RpcDispatcher::~RpcDispatcher() = default;

// ---- MCP Protocol Handlers ----
// These delegate to the mcp::McpServer SDK, which handles initialize,
// tools/list, and tools/call with auto-generated schemas and
// exception-driven dispatch.

JsonRpcResponse RpcDispatcher::on_initialize(int id) {
    // Delegate to MCP SDK — includes instructions field for AI agent guidance
    json params = json::object();
    json resp = mcp_server_->handle_request("initialize", params, id);
    JsonRpcResponse out;
    out.id = id;
    if (resp.contains("result")) {
        out.success = true;
        out.is_mcp_direct = true;
        out.result_json = resp["result"].dump();
    } else if (resp.contains("error")) {
        out.success = false;
        out.error_json = resp["error"].dump();
    }
    return out;
}

JsonRpcResponse RpcDispatcher::on_tools_list(int id) {
    // Delegate to MCP SDK — returns auto-generated schemas for all 49 tools
    json params = json::object();
    json resp = mcp_server_->handle_request("tools/list", params, id);
    JsonRpcResponse out;
    out.id = id;
    out.success = true;
    out.is_mcp_direct = true;
    if (resp.contains("result"))
        out.result_json = resp["result"].dump();
    else
        out.result_json = resp.dump();
    return out;
}

JsonRpcResponse RpcDispatcher::on_ping(int id) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = true;
    resp.result_json = json::object().dump();
    return resp;
}

// ---- MCP Tool Dispatch ----
// Delegates to the mcp::McpServer SDK.  All 49 consolidated tools are
// registered in mcp_tool_registry.cpp and dispatched via exception-driven
// handle_tools_call().  Old tool names are NOT retained — callers must
// use the new consolidated names.

JsonRpcResponse RpcDispatcher::dispatch_mcp_tool(int id, const std::string& tool_name, const json& args) {
    // Check if the tool is registered in the SDK
    if (mcp_server_->find_tool(tool_name)) {
        // Delegate to MCP SDK — exception-driven dispatch
        json result = mcp_server_->handle_tools_call(tool_name, args);

        JsonRpcResponse resp;
        resp.id = id;
        resp.success = true;
        resp.is_mcp_direct = true;
        resp.result_json = result.dump();
        return resp;
    }

    // Unknown tool — return MCP error
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = false;
    resp.is_mcp_error = true;
    json error_content = {
        {"content", json::array({{{"type", "text"}, {"text", "[MethodNotFound] Unknown tool: " + tool_name}}})},
        {"isError", true}
    };
    resp.error_json = error_content.dump();
    return resp;
}

// ---- Main dispatch ----

JsonRpcResponse RpcDispatcher::handle_request(const JsonRpcRequest& req) {
    // ---- MCP protocol routing ----
    if (req.is_mcp) {
        if (req.method == "initialize")    return on_initialize(req.id);
        if (req.method == "tools/list")    return on_tools_list(req.id);
        if (req.method == "tools/call") {
            json args;
            if (!req.mcp_tool_args.empty()) {
                try {
                    args = json::parse(req.mcp_tool_args);
                } catch (const json::parse_error&) {
                    JsonRpcResponse resp;
                    resp.id = req.id;
                    resp.success = false;
                    resp.is_mcp_error = true;
                    json error_content = {
                        {"content", json::array({{{"type", "text"}, {"text", "[InvalidParams] Invalid arguments JSON"}}})},
                        {"isError", true}
                    };
                    resp.error_json = error_content.dump();
                    return resp;
                }
            }
            return dispatch_mcp_tool(req.id, req.mcp_tool_name, args);
        }
        if (req.method == "ping")          return on_ping(req.id);
        if (req.method.rfind("notifications/", 0) == 0) {
            // Notifications are handled at transport level, should not reach here
            JsonRpcResponse resp;
            resp.id = req.id;
            resp.success = true;
            resp.result_json = json::object().dump();
            return resp;
        }

        // Unknown MCP method
        JsonRpcResponse resp;
        resp.id = req.id;
        resp.success = false;
        resp.is_mcp_error = true;
        json error_content = {
            {"content", json::array({{{"type", "text"}, {"text", "[MethodNotFound] Unknown MCP method: " + req.method}}})},
            {"isError", true}
        };
        resp.error_json = error_content.dump();
        return resp;
    }

    // ---- Legacy JSON-RPC routing (WebSocket transport) ----
    json params;
    if (!req.params_json.empty()) {
        try {
            params = json::parse(req.params_json);
        } catch (const json::parse_error&) {
            return error_resp(req.id, static_cast<int>(ErrorCode::InvalidRequest),
                              "Invalid params JSON");
        }
    }

    // Methods that do NOT require an active session
    if (req.method == "get_devices")       return on_get_devices(req.id, params);
    if (req.method == "create_session") {
        std::string device_id = params.value("device_id", "");
        std::string file_path = params.value("file_path", "");
        auto r = app_svc_->create_session(device_id, file_path);
        return wrap_result(req.id, r);
    }

    // All remaining methods require an active session
    ISessionService* session = app_svc_->get_active_session();
    if (!session) {
        return error_resp(req.id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    }

    if (req.method == "get_capture_status")      return on_get_capture_status(req.id, params);
    if (req.method == "start_capture")            return on_start_capture(req.id, params);
    if (req.method == "stop_capture")             return on_stop_capture(req.id, params);
    if (req.method == "get_channels")             return on_get_channels(req.id, params);
    if (req.method == "get_sample_config")        return on_get_sample_config(req.id, params);
    if (req.method == "set_sample_rate")          return on_set_sample_rate(req.id, params);
    if (req.method == "set_sample_limit")         return on_set_sample_limit(req.id, params);
    if (req.method == "set_collect_mode")         return on_set_collect_mode(req.id, params);
    if (req.method == "get_logic_waveform")       return on_get_logic_waveform(req.id, params);
    if (req.method == "get_analog_waveform")      return on_get_analog_waveform(req.id, params);
    if (req.method == "get_dso_waveform")         return on_get_dso_waveform(req.id, params);
    if (req.method == "get_available_decoders")   return on_get_available_decoders(req.id, params);
    if (req.method == "add_decoder")              return on_add_decoder(req.id, params);
    if (req.method == "remove_decoder")           return on_remove_decoder(req.id, params);
    if (req.method == "get_decoder_annotations")  return on_get_decoder_annotations(req.id, params);
    if (req.method == "get_measurements")         return on_get_measurements(req.id, params);
    if (req.method == "get_cursors")              return on_get_cursors(req.id, params);
    if (req.method == "add_cursor")               return on_add_cursor(req.id, params);
    if (req.method == "remove_cursor")            return on_remove_cursor(req.id, params);
if (req.method == "clear_cursors")            return on_clear_cursors(req.id, params);
    if (req.method == "set_glitch_filter")        return on_set_glitch_filter(req.id, params);
    if (req.method == "clear_glitch_filter")      return on_clear_glitch_filter(req.id, params);
    if (req.method == "set_signal_invert")        return on_set_signal_invert(req.id, params);
    if (req.method == "clear_signal_invert")      return on_clear_signal_invert(req.id, params);
    if (req.method == "save_file")                return on_save_file(req.id, params);
    if (req.method == "load_file")                return on_load_file(req.id, params);
    if (req.method == "export_data")              return on_export_data(req.id, params);
    if (req.method == "get_time_info")            return on_get_time_info(req.id, params);
    if (req.method == "get_disk_cache_info")      return on_get_disk_cache_info(req.id, params);
    if (req.method == "get_device_info")          return on_get_device_info(req.id, params);
    if (req.method == "get_work_mode")            return on_get_work_mode(req.id, params);
    if (req.method == "get_signal_list")          return on_get_signal_list(req.id, params);
    if (req.method == "find_next_edge")           return on_find_next_edge(req.id, params);

    // ---- P1-3: Batch operations ----
    if (req.method == "batch_call")               return on_batch_call(req.id, params);

    // ---- P0-3: Binary viewport data ----
    if (req.method == "get_viewport_binary")      return on_get_viewport_binary(req.id, params);

    return error_resp(req.id, static_cast<int>(ErrorCode::InvalidRequest),
                      "Unknown method: " + req.method);
}

// ---- MCP Tool Implementations ----

JsonRpcResponse RpcDispatcher::on_get_devices(int id, const json& params) {
    (void)params;
    auto devices = app_svc_->get_device_list();
    auto session = app_svc_->get_active_session();
    std::string active_id = "";
    DeviceInfo dinfo;
    if (session) {
        dinfo = session->get_device_info();
        active_id = dinfo.id;
    }

    json arr = json::array();
    for (const auto& d : devices) {
        json j;
        if (d.id == active_id) {
            j = to_json(dinfo);
        } else {
            j = to_json(d);
        }
        j["is_active"] = (d.id == active_id);
        arr.push_back(j);
    }
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_start_capture(int id, const json& params) {
    mcp_dbg_log("on_start_capture: ENTER");
    auto session = app_svc_->get_active_session();

    // Ensure device is connected when deviceId is provided.
    // AppService::initialize() pre-creates a SessionService WITHOUT any device
    // connected (especially in headless mode), so even when an active session
    // exists we must still call create_session(deviceId) to trigger set_device()
    // when no device is currently active. create_session() is a no-op if the
    // requested device is already the active one (it checks device_already_active).
    if (params.contains("deviceId")) {
        std::string device_id = params.value("deviceId", "");
        if (!device_id.empty()) {
            mcp_dbg_log(QString("on_start_capture: ensuring device %1 is connected").arg(QString::fromStdString(device_id)).toUtf8().constData());
            auto r = app_svc_->create_session(device_id, "");
            if (!r.ok())
                return error_resp(id, static_cast<int>(r.error().code),
                                  "Failed to create session: " + r.error().message);
            session = app_svc_->get_active_session();
            mcp_dbg_log("on_start_capture: session ready, processing events");

            // If create_session() called set_device() (device was not already active),
            // it triggered CurrentDeviceChanged which causes massive UI rebuilds.
            // In GUI mode, process pending events so configure_and_start() doesn't
            // conflict with the ongoing UI rebuild. In headless mode, skip — there's
            // no UI to rebuild.
            // Phase 2: reduced from double processEvents() to single + gui guard.
            if (SessionService::is_gui_mode())
                QCoreApplication::processEvents();
        }
    }

    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session. Provide deviceId to auto-create one.");

    // Support MCP-style parameters (logicDeviceConfiguration, captureConfiguration)
    if (params.contains("logicDeviceConfiguration") || params.contains("captureConfiguration")) {
        // Extract configuration from MCP-style params
        json logic_config = params.value("logicDeviceConfiguration", json::object());
        json capture_config = params.value("captureConfiguration", json::object());

        std::vector<int16_t> digital_channels;
        std::vector<int16_t> analog_channels;
        uint64_t digital_sample_rate = 0;
        uint64_t analog_sample_rate = 0;
        double digital_threshold_volts = 0.0;
        std::vector<std::pair<int16_t, double>> glitch_filters;
        std::string capture_mode = "manual";
        double duration_seconds = 0.0;
        uint64_t sample_count = 0;

        if (logic_config.contains("digitalChannels"))
            for (auto& ch : logic_config["digitalChannels"])
                digital_channels.push_back(ch.get<int16_t>());
        if (logic_config.contains("analogChannels"))
            for (auto& ch : logic_config["analogChannels"])
                analog_channels.push_back(ch.get<int16_t>());
        if (logic_config.contains("digitalSampleRate"))
            digital_sample_rate = logic_config["digitalSampleRate"].get<uint64_t>();
        if (logic_config.contains("analogSampleRate"))
            analog_sample_rate = logic_config["analogSampleRate"].get<uint64_t>();
        if (logic_config.contains("digitalThresholdVolts"))
            digital_threshold_volts = logic_config["digitalThresholdVolts"].get<double>();

        if (capture_config.contains("timedCaptureMode")) {
            capture_mode = "timed";
            duration_seconds = capture_config["timedCaptureMode"].value("durationSeconds", 0.0);
        } else if (capture_config.contains("manualCaptureMode")) {
            capture_mode = "manual";
            sample_count = capture_config["manualCaptureMode"].value("sampleCount", (uint64_t)0);
        }

        // Parse digitalCaptureMode
        int trigger_channel_index = -1;
        std::string trigger_type;
        double after_trigger_seconds = 0.0;
        double min_pulse_width_seconds = 0.0;
        double max_pulse_width_seconds = 0.0;
        std::vector<std::pair<int16_t, std::string>> linked_channels;

        if (capture_config.contains("digitalCaptureMode") && capture_config["digitalCaptureMode"].is_object()) {
            auto& dtm = capture_config["digitalCaptureMode"];
            trigger_channel_index = dtm.value("triggerChannelIndex", -1);
            trigger_type = dtm.value("triggerType", "");
            after_trigger_seconds = dtm.value("afterTriggerSeconds", 0.0);
            min_pulse_width_seconds = dtm.value("minPulseWidthSeconds", 0.0);
            max_pulse_width_seconds = dtm.value("maxPulseWidthSeconds", 0.0);
            if (dtm.contains("linkedChannels") && dtm["linkedChannels"].is_array()) {
                for (auto& lc : dtm["linkedChannels"]) {
                    linked_channels.push_back({
                        lc.value("channelIndex", (int16_t)-1),
                        lc.value("state", "")
                    });
                }
            }
        }

        // Parse channelMode from logicDeviceConfiguration
        std::string channel_mode;
        if (logic_config.contains("channelMode"))
            channel_mode = logic_config["channelMode"].get<std::string>();

        // Parse new logicDeviceConfiguration parameters
        bool rle_enabled = logic_config.value("rleEnabled", false);
        double stream_buffer_size_gb = logic_config.value("streamBufferSizeGB", 0.0);
        double stream_mem_buffer_size_gb = logic_config.value("streamMemBufferSizeGB", 0.0);
        bool disk_cache_enabled = logic_config.value("diskCacheEnabled", false);
        std::string disk_cache_path = logic_config.value("diskCachePath", "");
        std::string threshold_preset = logic_config.value("thresholdPreset", "");
        std::string operation_mode = logic_config.value("operationMode", "");
        std::string buffer_options = logic_config.value("bufferOptions", "");
        std::string digital_filter = logic_config.value("digitalFilter", "");
        std::string pattern = logic_config.value("pattern", "");

        // Parse captureRatio and repeatIntervalSeconds
        int capture_ratio = capture_config.value("captureRatio", -1);
        double repeat_interval_seconds = capture_config.value("repeatIntervalSeconds", 0.0);

        mcp_dbg_log("on_start_capture: calling configure_and_start");
        auto r = session->configure_and_start(
            digital_channels, analog_channels,
            digital_sample_rate, analog_sample_rate,
            digital_threshold_volts, glitch_filters,
            capture_mode, duration_seconds, false,
            trigger_channel_index, trigger_type,
            after_trigger_seconds, min_pulse_width_seconds,
            max_pulse_width_seconds, linked_channels,
            channel_mode,
            rle_enabled,
            stream_buffer_size_gb, stream_mem_buffer_size_gb,
            disk_cache_enabled, disk_cache_path,
            threshold_preset,
            operation_mode, buffer_options, digital_filter, pattern,
            capture_ratio, repeat_interval_seconds,
            sample_count);
        mcp_dbg_log("on_start_capture: configure_and_start returned");
        return wrap_result(id, r);
    }

    // Legacy simple start
    bool instant = params.value("instant", false);
    return wrap_void(id, session->start_capture(instant));
}

JsonRpcResponse RpcDispatcher::on_stop_capture(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    return wrap_void(id, session->stop_capture());
}

JsonRpcResponse RpcDispatcher::on_wait_capture(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    double timeout_seconds = params.value("timeoutSeconds", 300.0);
    uint64_t timeout_ms = static_cast<uint64_t>(timeout_seconds * 1000.0);
    return wrap_void(id, session->wait_capture_complete(timeout_ms));
}

JsonRpcResponse RpcDispatcher::on_load_capture(int id, const json& params) {
    if (!params.contains("filepath"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'filepath' parameter");
    std::string path = params["filepath"].get<std::string>();

    auto session = app_svc_->get_active_session();

    // Auto-create session with file path (Logic 2 behavior)
    if (!session) {
        auto r = app_svc_->create_session("", path);
        if (!r.ok())
            return error_resp(id, static_cast<int>(r.error().code),
                              "Failed to create session: " + r.error().message);
        session = app_svc_->get_active_session();
    } else {
        auto res = session->load_file(path);
        if (!res.ok())
            return error_resp(id, static_cast<int>(res.error().code), res.error().message);
    }

    return success_resp(id, json{{"status", "loaded"}});
}

JsonRpcResponse RpcDispatcher::on_save_capture(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    if (!params.contains("filepath"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'filepath' parameter");
    std::string path = params["filepath"].get<std::string>();
    return wrap_void(id, session->save_file(path));
}

JsonRpcResponse RpcDispatcher::on_close_capture(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    return wrap_void(id, session->close_capture());
}

JsonRpcResponse RpcDispatcher::on_add_analyzer(int id, const json& params) {
    mcp_dbg_log("on_add_analyzer: ENTER");
    auto session = app_svc_->get_active_session();

    // Ensure a device is connected. AppService::initialize() pre-creates a
    // SessionService WITHOUT any device connected (especially in headless
    // mode), so even when an active session exists we must call
    // create_session(deviceId) to trigger set_device() when no device is
    // currently active. Same pattern as on_start_capture.
    if (params.contains("deviceId")) {
        std::string device_id = params["deviceId"].get<std::string>();
        if (!device_id.empty()) {
            mcp_dbg_log(QString("on_add_analyzer: ensuring device %1 is connected")
                        .arg(QString::fromStdString(device_id))
                        .toUtf8().constData());
            auto r = app_svc_->create_session(device_id, "");
            if (!r.ok()) {
                mcp_dbg_log("on_add_analyzer: create_session FAILED");
                return error_resp(id, static_cast<int>(r.error().code),
                                  "Failed to create session: " + r.error().message);
            }
            session = app_svc_->get_active_session();
            mcp_dbg_log("on_add_analyzer: session ready, processing events");
            // Phase 2: guard with is_gui_mode() — headless mode has no UI rebuild.
            if (SessionService::is_gui_mode())
                QCoreApplication::processEvents();
        }
    } else if (!session) {
        // No deviceId provided and no active session — try to auto-create
        // one with the first available (non-demo) device so that add_analyzer
        // can still be called before start_capture (recommended MCP workflow).
        mcp_dbg_log("on_add_analyzer: no active session, auto-creating one");
        auto devices = app_svc_->get_device_list();
        std::string device_id;
        for (const auto& d : devices) {
            if (!d.is_demo) {
                device_id = d.id;
                break;
            }
        }
        if (device_id.empty() && !devices.empty())
            device_id = devices[0].id;

        if (!device_id.empty()) {
            mcp_dbg_log("on_add_analyzer: calling create_session");
            auto r = app_svc_->create_session(device_id, "");
            if (!r.ok()) {
                mcp_dbg_log("on_add_analyzer: create_session FAILED");
                return error_resp(id, static_cast<int>(r.error().code),
                                  "Failed to create session: " + r.error().message);
            }
            session = app_svc_->get_active_session();
            mcp_dbg_log("on_add_analyzer: session created, processing events");
            // Phase 2: guard with is_gui_mode() — headless mode has no UI rebuild.
            if (SessionService::is_gui_mode())
                QCoreApplication::processEvents();
        }
    }

    if (!session) {
        mcp_dbg_log("on_add_analyzer: no session available");
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    }

    // MCP uses "analyzerName", internal uses "id"
    std::string decoder_id;
    if (params.contains("analyzerName"))
        decoder_id = params["analyzerName"].get<std::string>();
    else if (params.contains("id"))
        decoder_id = params["id"].get<std::string>();
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerName' parameter");

    std::map<std::string, std::string> options;
    std::map<std::string, int16_t> channel_map;

    // MCP uses "settings" object for options and channel map
    if (params.contains("settings") && params["settings"].is_object()) {
        auto& settings = params["settings"];
        // Support both camelCase and snake_case keys
        auto get_nested = [&](const std::string& key1, const std::string& key2) -> const json* {
            if (settings.contains(key1) && settings[key1].is_object()) return &settings[key1];
            if (settings.contains(key2) && settings[key2].is_object()) return &settings[key2];
            return nullptr;
        };

        if (auto* opts = get_nested("options", "options")) {
            for (auto& [k, v] : opts->items()) {
                if (v.is_string())
                    options[k] = v.get<std::string>();
                else if (v.is_number())
                    options[k] = v.dump();
            }
        }
        if (auto* cmap = get_nested("channelMap", "channel_map")) {
            for (auto& [k, v] : cmap->items()) {
                if (v.is_number_integer())
                    channel_map[k] = v.get<int16_t>();
            }
        }
    }

    // Legacy format: "options" and "channel_map" at top level
    if (params.contains("options") && params["options"].is_object()) {
        for (auto& [k, v] : params["options"].items())
            options[k] = v.get<std::string>();
    }
    if (params.contains("channel_map") && params["channel_map"].is_object()) {
        for (auto& [k, v] : params["channel_map"].items())
            channel_map[k] = v.get<int16_t>();
    }

    // Extract analyzerLabel
    std::string label;
    if (params.contains("analyzerLabel"))
        label = params["analyzerLabel"].get<std::string>();

    // Extract stackOnAnalyzerId
    std::string stack_on_id;
    if (params.contains("stackOnAnalyzerId"))
        stack_on_id = params["stackOnAnalyzerId"].get<std::string>();

    mcp_dbg_log(QString("on_add_analyzer: calling add_decoder(%1)").arg(QString::fromStdString(decoder_id)).toUtf8().constData());
    auto r = session->add_decoder(decoder_id, options, channel_map, label, false, stack_on_id);
    mcp_dbg_log("on_add_analyzer: add_decoder returned");
    return wrap_result(id, r);
}

JsonRpcResponse RpcDispatcher::on_remove_analyzer(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    // MCP uses "analyzerId", internal uses "instance_id"
    std::string instance_id;
    if (params.contains("analyzerId"))
        instance_id = params["analyzerId"].get<std::string>();
    else if (params.contains("instance_id"))
        instance_id = params["instance_id"].get<std::string>();
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerId' parameter");

    return wrap_void(id, session->remove_decoder(instance_id));
}

JsonRpcResponse RpcDispatcher::on_list_analyzers(int id, const json& params) {
    (void)params;
    auto session = app_svc_->get_active_session();
    if (!session) {
        // Auto-create session with first available device
        auto devices = app_svc_->get_device_list();
        if (!devices.empty()) {
            app_svc_->create_session(devices[0].id, "");
            session = app_svc_->get_active_session();
        }
    }

    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No session available");

    auto decoders = session->get_available_decoders();

    json result = json::array();
    for (const auto& d : decoders) {
        result.push_back(to_json(d));
    }

    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_analyzer_options(int id, const json& params) {
    // No active session required - this queries decoder metadata
    if (!params.contains("analyzerName"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerName' parameter");

    std::string analyzer_name = params["analyzerName"].get<std::string>();

    // Create a temporary session to access the decoder list
    auto session = app_svc_->get_active_session();
    if (!session) {
        // Auto-create session with first available device
        auto devices = app_svc_->get_device_list();
        if (!devices.empty()) {
            app_svc_->create_session(devices[0].id, "");
            session = app_svc_->get_active_session();
        }
    }

    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No session available");

    auto result = session->get_decoder_options(analyzer_name);
    if (!result.ok())
        return error_resp(id, static_cast<int>(result.error().code),
                          result.error().message);

    return success_resp(id, result.value());
}

JsonRpcResponse RpcDispatcher::on_export_raw_data_csv(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    if (!params.contains("directory"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'directory' parameter");

    std::string directory = params["directory"].get<std::string>();
    std::vector<int32_t> digital_channels;
    std::vector<int32_t> analog_channels;
    int analog_downsample_ratio = params.value("analogDownsampleRatio", 1);
    bool iso8601_timestamp = params.value("iso8601Timestamp", false);

    if (params.contains("digitalChannels"))
        for (auto& ch : params["digitalChannels"])
            digital_channels.push_back(ch.get<int32_t>());
    if (params.contains("analogChannels"))
        for (auto& ch : params["analogChannels"])
            analog_channels.push_back(ch.get<int32_t>());

    return wrap_void(id, session->export_raw_data_csv(
        directory, digital_channels, analog_channels,
        analog_downsample_ratio, iso8601_timestamp));
}

JsonRpcResponse RpcDispatcher::on_export_raw_data_binary(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    if (!params.contains("directory"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'directory' parameter");

    std::string directory = params["directory"].get<std::string>();
    std::vector<int32_t> digital_channels;
    std::vector<int32_t> analog_channels;
    int analog_downsample_ratio = params.value("analogDownsampleRatio", 1);

    if (params.contains("digitalChannels"))
        for (auto& ch : params["digitalChannels"])
            digital_channels.push_back(ch.get<int32_t>());
    if (params.contains("analogChannels"))
        for (auto& ch : params["analogChannels"])
            analog_channels.push_back(ch.get<int32_t>());

    return wrap_void(id, session->export_raw_data_binary(
        directory, digital_channels, analog_channels,
        analog_downsample_ratio));
}

JsonRpcResponse RpcDispatcher::on_export_raw_data(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    if (!params.contains("format"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'format' parameter (csv/binary/vcd/hex/bits)");
    if (!params.contains("directory"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'directory' parameter");

    std::string format = params["format"].get<std::string>();
    std::string directory = params["directory"].get<std::string>();
    std::vector<int32_t> digital_channels;
    std::vector<int32_t> analog_channels;
    int analog_downsample_ratio = params.value("analogDownsampleRatio", 1);
    bool iso8601_timestamp = params.value("iso8601Timestamp", false);

    if (params.contains("digitalChannels"))
        for (auto& ch : params["digitalChannels"])
            digital_channels.push_back(ch.get<int32_t>());
    if (params.contains("analogChannels"))
        for (auto& ch : params["analogChannels"])
            analog_channels.push_back(ch.get<int32_t>());

    return wrap_void(id, session->export_raw_data(
        format, directory, digital_channels, analog_channels,
        analog_downsample_ratio, iso8601_timestamp));
}

JsonRpcResponse RpcDispatcher::on_export_data_table_csv(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    if (!params.contains("filepath"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'filepath' parameter");

    std::string filepath = params["filepath"].get<std::string>();
    bool iso8601_timestamp = params.value("iso8601Timestamp", false);

    // Support both single analyzerId and analyzers array
    std::string analyzer_id;
    int radix_type = 0;

    if (params.contains("analyzers") && params["analyzers"].is_array() && !params["analyzers"].empty()) {
        // Use first analyzer from array
        auto& first = params["analyzers"][0];
        if (first.contains("analyzerId"))
            analyzer_id = first["analyzerId"].get<std::string>();
        if (first.contains("radixType"))
            radix_type = first["radixType"].get<int>();
    }

    return wrap_void(id, session->export_data_table_csv(
        filepath, analyzer_id, radix_type, iso8601_timestamp));
}

JsonRpcResponse RpcDispatcher::on_get_capture_status(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    return success_resp(id, to_json(session->get_capture_status()));
}

JsonRpcResponse RpcDispatcher::on_get_channels(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto channels = session->get_channels();
    json arr = json::array();
    for (const auto& c : channels)
        arr.push_back(to_json(c));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_get_analyzer_results(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    // MCP uses "analyzerId", internal uses "instance_id"
    std::string instance_id;
    if (params.contains("analyzerId"))
        instance_id = params["analyzerId"].get<std::string>();
    else if (params.contains("instance_id"))
        instance_id = params["instance_id"].get<std::string>();
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerId' parameter");

    uint64_t start = params.value("startSample", uint64_t(0));
    uint64_t end = params.value("endSample", UINT64_MAX);
    int max_count = params.value("maxCount", 1000);

    auto r = session->get_decoder_annotations(instance_id, start, end, max_count);
    if (!r) {
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }
    json arr = json::array();
    for (const auto& a : r.value())
        arr.push_back(to_json(a));
    return success_resp(id, arr);
}

// ---- Batch A MCP Tool Implementations ----

JsonRpcResponse RpcDispatcher::on_get_trigger_config(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    std::string mode = params.value("mode", "");
    if (mode.empty()) {
        // Infer from active work mode
        auto work_mode = session->get_work_mode();
        mode = (work_mode == WorkMode::Dso) ? "dso" : "logic";
    }

    if (mode == "dso") {
        auto cfg = session->get_dso_trigger_config();
        json result = to_json(cfg);
        result["mode"] = "dso";
        return success_resp(id, result);
    }
    // Default: logic
    auto cfg = session->get_logic_trigger_config();
    json result = to_json(cfg);
    result["mode"] = "logic";
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_set_trigger_config(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    if (!params.contains("mode"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'mode' parameter");
    std::string mode = params["mode"].get<std::string>();

    if (mode == "dso") {
        DsoTriggerConfig cfg;
        if (params.contains("source"))
            cfg.source = static_cast<TriggerSource>(params["source"].get<int>());
        if (params.contains("slope"))
            cfg.slope = static_cast<TriggerSlope>(params["slope"].get<int>());
        if (params.contains("horizPos"))
            cfg.horiz_pos = params["horizPos"].get<double>();
        if (params.contains("holdoff"))
            cfg.holdoff = params["holdoff"].get<double>();
        if (params.contains("margin"))
            cfg.margin = params["margin"].get<double>();
        if (params.contains("channel"))
            cfg.channel = params["channel"].get<int32_t>();
        return wrap_void(id, session->set_dso_trigger_config(cfg));
    }

    if (mode == "logic") {
        LogicTriggerConfig cfg;
        if (params.contains("stageCount"))
            cfg.stage_count = params["stageCount"].get<int32_t>();
        if (params.contains("configJson"))
            cfg.config_json = params["configJson"].get<std::string>();
        return wrap_void(id, session->set_logic_trigger_config(cfg));
    }

    return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                      "Invalid 'mode' (must be 'logic' or 'dso')");
}

JsonRpcResponse RpcDispatcher::on_get_probe_config(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' parameter");
    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    auto cfg = session->get_probe_config(ch);
    json result = to_json(cfg);
    result["channel_index"] = ch;
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_set_probe_config(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' parameter");
    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());

    // Read current config first so omitted fields retain their values
    ProbeConfig cfg = session->get_probe_config(ch);
    if (params.contains("vdiv"))
        cfg.vdiv = params["vdiv"].get<double>();
    if (params.contains("coupling"))
        cfg.coupling = static_cast<Coupling>(params["coupling"].get<int>());
    if (params.contains("vfactor"))
        cfg.vfactor = params["vfactor"].get<double>();
    if (params.contains("mapDefault"))
        cfg.map_default = params["mapDefault"].get<bool>();
    return wrap_void(id, session->set_probe_config(ch, cfg));
}

JsonRpcResponse RpcDispatcher::on_set_channel_enabled_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex") || !params.contains("enabled"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' or 'enabled' parameter");
    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    bool enabled = params["enabled"].get<bool>();
    return wrap_void(id, session->set_channel_enabled(ch, enabled));
}

JsonRpcResponse RpcDispatcher::on_set_channel_name_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex") || !params.contains("name"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' or 'name' parameter");
    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    std::string name = params["name"].get<std::string>();
    return wrap_void(id, session->set_channel_name(ch, name));
}

JsonRpcResponse RpcDispatcher::on_set_time_base(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("timeBase"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'timeBase' parameter");
    uint64_t tb = params["timeBase"].get<uint64_t>();
    return wrap_void(id, session->set_time_base(tb));
}

JsonRpcResponse RpcDispatcher::on_set_collect_mode_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("mode"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'mode' parameter");
    std::string mode_str = params["mode"].get<std::string>();
    CollectMode mode;
    if (mode_str == "single")
        mode = CollectMode::Single;
    else if (mode_str == "repetitive" || mode_str == "repeat")
        mode = CollectMode::Repeat;
    else if (mode_str == "loop")
        mode = CollectMode::Loop;
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Invalid 'mode' (must be 'single', 'repetitive', or 'loop')");
    return wrap_void(id, session->set_collect_mode(mode));
}

JsonRpcResponse RpcDispatcher::on_set_repeat_interval(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("intervalMs"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'intervalMs' parameter");
    int interval_ms = params["intervalMs"].get<int>();
    double seconds = static_cast<double>(interval_ms) / 1000.0;
    return wrap_void(id, session->set_repeat_interval(seconds));
}

JsonRpcResponse RpcDispatcher::on_get_logic_samples_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' parameter");

    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    uint64_t start = params.value("startSample", uint64_t(0));
    // endSample defaults to UINT64_MAX when omitted or -1
    uint64_t end;
    if (params.contains("endSample")) {
        auto& ev = params["endSample"];
        if (ev.is_number_integer() && ev.get<int64_t>() == -1)
            end = UINT64_MAX;
        else
            end = ev.get<uint64_t>();
    } else {
        end = UINT64_MAX;
    }

    std::vector<int16_t> channels = {ch};
    std::vector<uint8_t> out_data;
    auto r = session->get_logic_samples(start, end, channels, out_data);
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    json result = {
        {"sample_count", r.value()},
        {"data", base64_encode(out_data)}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_analog_samples_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' parameter");

    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    uint64_t start = params.value("startSample", uint64_t(0));
    uint64_t end;
    if (params.contains("endSample")) {
        auto& ev = params["endSample"];
        if (ev.is_number_integer() && ev.get<int64_t>() == -1)
            end = UINT64_MAX;
        else
            end = ev.get<uint64_t>();
    } else {
        end = UINT64_MAX;
    }

    std::vector<float> out_data;
    auto r = session->get_analog_samples(start, end, ch, out_data);
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    json result = {
        {"sample_count", r.value()},
        {"data", out_data}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_dso_samples_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' parameter");

    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    uint64_t start = params.value("startSample", uint64_t(0));
    uint64_t end;
    if (params.contains("endSample")) {
        auto& ev = params["endSample"];
        if (ev.is_number_integer() && ev.get<int64_t>() == -1)
            end = UINT64_MAX;
        else
            end = ev.get<uint64_t>();
    } else {
        end = UINT64_MAX;
    }

    std::vector<float> out_data;
    auto r = session->get_dso_samples(start, end, ch, out_data);
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    json result = {
        {"sample_count", r.value()},
        {"data", out_data}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_find_next_edge_mcp(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channelIndex") || !params.contains("startSample"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channelIndex' or 'startSample' parameter");

    int16_t ch = static_cast<int16_t>(params["channelIndex"].get<int32_t>());
    uint64_t from = params["startSample"].get<uint64_t>();
    std::string direction = params.value("direction", "forward");
    bool rising = (direction != "falling");
    auto r = session->find_next_edge(from, ch, rising);
    return wrap_result(id, r);
}

JsonRpcResponse RpcDispatcher::on_find_pattern(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("channels") || !params.contains("pattern") || !params.contains("startSample"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'channels', 'pattern', or 'startSample' parameter");

    auto channels = params["channels"].get<std::vector<int32_t>>();
    if (channels.empty())
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "'channels' array must not be empty");
    // NOTE: SessionService::find_pattern only supports a single channel.
    // Use the first channel from the array (multi-channel search is a TODO).
    int16_t ch = static_cast<int16_t>(channels[0]);
    std::string pattern = params["pattern"].get<std::string>();
    uint64_t from = params["startSample"].get<uint64_t>();
    auto r = session->find_pattern(from, ch, pattern);
    return wrap_result(id, r);
}

JsonRpcResponse RpcDispatcher::on_get_active_decoders(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto decoders = session->get_active_decoders();
    json arr = json::array();
    for (const auto& d : decoders)
        arr.push_back(to_json(d));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_clear_all_decoders(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    return wrap_void(id, session->clear_all_decoders());
}

JsonRpcResponse RpcDispatcher::on_list_sessions(int id, const json& /*params*/) {
    auto ids = app_svc_->get_session_ids();
    int active_id = app_svc_->get_active_session_id();
    json arr = json::array();
    for (int sid : ids) {
        json s;
        s["session_id"] = sid;
        s["is_active"] = (sid == active_id);
        auto* sess = app_svc_->get_session(sid);
        if (sess) {
            auto dev = sess->get_device_info();
            s["device_id"] = dev.id;
            s["device_name"] = dev.display_name;
            s["driver_name"] = dev.driver_name;
            s["is_file"] = dev.is_file;
            s["file_path"] = dev.path;
        }
        arr.push_back(s);
    }
    json result = {
        {"sessions", arr},
        {"active_session_id", active_id},
        {"count", static_cast<int>(ids.size())}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_create_session_mcp(int id, const json& params) {
    std::string device_id = params.value("deviceId", "");
    std::string file_path = params.value("filePath", "");
    auto r = app_svc_->create_session(device_id, file_path);
    return wrap_result(id, r);
}

JsonRpcResponse RpcDispatcher::on_destroy_session_mcp(int id, const json& params) {
    if (!params.contains("sessionId"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'sessionId' parameter");
    int sid = params["sessionId"].get<int>();
    return wrap_void(id, app_svc_->destroy_session(sid));
}

JsonRpcResponse RpcDispatcher::on_set_active_session_mcp(int id, const json& params) {
    if (!params.contains("sessionId"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'sessionId' parameter");
    int sid = params["sessionId"].get<int>();
    return wrap_void(id, app_svc_->set_active_session(sid));
}

JsonRpcResponse RpcDispatcher::on_get_session_count(int id, const json& /*params*/) {
    int count = app_svc_->get_session_count();
    return success_resp(id, json{{"count", count}});
}

JsonRpcResponse RpcDispatcher::on_connect_device(int id, const json& params) {
    if (!params.contains("deviceId"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'deviceId' parameter");
    std::string device_id = params["deviceId"].get<std::string>();
    // Reuse create_session(deviceId) which triggers set_device() internally
    // when no device is currently active — same pattern as on_start_capture.
    auto r = app_svc_->create_session(device_id, "");
    if (!r.ok())
        return error_resp(id, static_cast<int>(r.error().code),
                          "Failed to connect device: " + r.error().message);
    json result = {
        {"success", true},
        {"session_id", r.value()}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_disconnect_device(int id, const json& params) {
    std::string device_id;
    if (params.contains("deviceId")) {
        device_id = params["deviceId"].get<std::string>();
    } else {
        // Use the active device
        auto r = app_svc_->get_active_device();
        if (!r.ok())
            return error_resp(id, static_cast<int>(r.error().code),
                              "No active device to disconnect");
        device_id = r.value().id;
    }
    return wrap_void(id, app_svc_->disconnect_device(device_id));
}

JsonRpcResponse RpcDispatcher::on_get_config(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("key") || !params.contains("type"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'key' or 'type' parameter");
    int key = params["key"].get<int>();
    std::string type = params["type"].get<std::string>();

    if (type == "string") {
        auto r = session->get_config_string(key);
        return wrap_result(id, r);
    }
    if (type == "bool") {
        auto r = session->get_config_bool(key);
        return wrap_result(id, r);
    }
    if (type == "uint64") {
        auto r = session->get_config_uint64(key);
        return wrap_result(id, r);
    }
    if (type == "int" || type == "int64") {
        // NOTE: 'int64' currently maps to int32 because SessionService
        // does not expose a dedicated int64 getter (TODO for batch B).
        auto r = session->get_config_int32(key);
        return wrap_result(id, r);
    }
    if (type == "double") {
        auto r = session->get_config_double(key);
        return wrap_result(id, r);
    }
    return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                      "Unsupported 'type': " + type);
}

JsonRpcResponse RpcDispatcher::on_set_config(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("key") || !params.contains("type") || !params.contains("value"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'key', 'type', or 'value' parameter");
    int key = params["key"].get<int>();
    std::string type = params["type"].get<std::string>();
    const json& value = params["value"];

    if (type == "string") {
        std::string v = value.get<std::string>();
        return wrap_result(id, session->set_config_string(key, v));
    }
    if (type == "bool") {
        bool v = value.get<bool>();
        return wrap_result(id, session->set_config_bool(key, v));
    }
    if (type == "uint64") {
        uint64_t v = value.get<uint64_t>();
        return wrap_result(id, session->set_config_uint64(key, v));
    }
    if (type == "int" || type == "int64") {
        // NOTE: 'int64' currently maps to int32 (TODO for batch B).
        int32_t v = value.get<int32_t>();
        return wrap_result(id, session->set_config_int32(key, v));
    }
    if (type == "double") {
        double v = value.get<double>();
        return wrap_result(id, session->set_config_double(key, v));
    }
    return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                      "Unsupported 'type': " + type);
}

JsonRpcResponse RpcDispatcher::on_get_glitch_filter_config(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto cfg = session->get_glitch_filter_config();
    json result = to_json(cfg);
    result["is_active"] = !cfg.channels.empty();
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_signal_invert_config(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto cfg = session->get_signal_invert_config();
    json result = to_json(cfg);
    result["is_active"] = !cfg.channels.empty();
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_repeat_status(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto cfg = session->get_sample_config();
    json result = {
        {"is_single_mode",   cfg.collect_mode == CollectMode::Single},
        {"is_repeat_mode",   cfg.collect_mode == CollectMode::Repeat},
        {"is_loop_mode",     cfg.collect_mode == CollectMode::Loop},
        {"collect_mode",     static_cast<int>(cfg.collect_mode)},
        {"repeat_interval",  cfg.repeat_interval},
        {"repeat_hold_percent", cfg.repeat_hold_percent}
    };
    return success_resp(id, result);
}

// ---- Batch B MCP Tool Implementations ----

JsonRpcResponse RpcDispatcher::on_refresh_device_list(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    auto r = session->refresh_device_list();
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);

    // Mirror on_get_devices: tag the active device so MCP clients can
    // immediately see which entry corresponds to the current session.
    DeviceInfo dinfo = session->get_device_info();
    const std::string& active_id = dinfo.id;
    json arr = json::array();
    for (const auto& d : r.value()) {
        json j = (d.id == active_id) ? to_json(dinfo) : to_json(d);
        j["is_active"] = (d.id == active_id);
        arr.push_back(j);
    }
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_set_save_range(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    if (!params.contains("startSample") || !params.contains("endSample"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'startSample' or 'endSample' parameter");
    uint64_t start = params["startSample"].get<uint64_t>();
    uint64_t end   = params["endSample"].get<uint64_t>();
    return wrap_void(id, session->set_save_range(start, end));
}

JsonRpcResponse RpcDispatcher::on_reconfigure_decoder(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    std::string instance_id;
    if (params.contains("analyzerId"))
        instance_id = params["analyzerId"].get<std::string>();
    else if (params.contains("instance_id"))
        instance_id = params["instance_id"].get<std::string>();
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerId' parameter");

    std::map<std::string, std::string> options;
    if (params.contains("options") && params["options"].is_object()) {
        for (auto& [k, v] : params["options"].items()) {
            if (v.is_string())
                options[k] = v.get<std::string>();
            else if (v.is_number())
                options[k] = v.dump();
        }
    }

    std::map<std::string, int> channel_map;
    if (params.contains("channelMap") && params["channelMap"].is_object()) {
        for (auto& [k, v] : params["channelMap"].items()) {
            if (v.is_number_integer())
                channel_map[k] = v.get<int>();
        }
    } else if (params.contains("channel_map") && params["channel_map"].is_object()) {
        // snake_case fallback
        for (auto& [k, v] : params["channel_map"].items()) {
            if (v.is_number_integer())
                channel_map[k] = v.get<int>();
        }
    }

    return wrap_void(id, session->reconfigure_decoder(instance_id, options, channel_map));
}

JsonRpcResponse RpcDispatcher::on_get_decoder_class_names(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    std::string decoder_id;
    if (params.contains("analyzerName"))
        decoder_id = params["analyzerName"].get<std::string>();
    else if (params.contains("decoderId"))
        decoder_id = params["decoderId"].get<std::string>();
    else if (params.contains("decoder_id"))
        decoder_id = params["decoder_id"].get<std::string>();
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerName' parameter");

    auto r = session->get_decoder_class_names(decoder_id);
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    json arr = json::array();
    for (const auto& c : r.value())
        arr.push_back(to_json(c));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_get_decoder_binary_output(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    std::string instance_id;
    if (params.contains("analyzerId"))
        instance_id = params["analyzerId"].get<std::string>();
    else if (params.contains("instance_id"))
        instance_id = params["instance_id"].get<std::string>();
    else
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'analyzerId' parameter");

    if (!params.contains("outputId"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'outputId' parameter");
    int output_id = params["outputId"].get<int>();

    auto r = session->get_decoder_binary_output(instance_id, output_id);
    if (!r) {
        // ConfigNotSupported (and any other failure) is propagated as a
        // JSON-RPC error response with code + message so MCP clients can
        // detect the limitation cleanly.
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }
    // Encode bytes as a JSON array of integers (0-255) per the MCP contract.
    json arr = json::array();
    for (uint8_t b : r.value())
        arr.push_back(static_cast<int>(b));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_get_math_results(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto r = session->get_math_results();
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    return success_resp(id, to_json(r.value()));
}

JsonRpcResponse RpcDispatcher::on_get_spectrum_results(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto r = session->get_spectrum_results();
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    return success_resp(id, to_json(r.value()));
}

JsonRpcResponse RpcDispatcher::on_get_lissajous_results(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto r = session->get_lissajous_results();
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    return success_resp(id, to_json(r.value()));
}

JsonRpcResponse RpcDispatcher::on_get_error_state(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    auto r = session->get_error_state();
    if (!r)
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    return success_resp(id, to_json(r.value()));
}

JsonRpcResponse RpcDispatcher::on_clear_error_state(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    return wrap_void(id, session->clear_error_state());
}

// ---- Legacy Method Implementations (WebSocket transport) ----

JsonRpcResponse RpcDispatcher::on_get_sample_config(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return success_resp(id, to_json(session->get_sample_config()));
}

JsonRpcResponse RpcDispatcher::on_set_sample_rate(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("rate"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'rate' parameter");
    uint64_t rate = params["rate"].get<uint64_t>();
    return wrap_void(id, session->set_sample_rate(rate));
}

JsonRpcResponse RpcDispatcher::on_set_sample_limit(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("limit"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'limit' parameter");
    uint64_t limit = params["limit"].get<uint64_t>();
    return wrap_void(id, session->set_sample_limit(limit));
}

JsonRpcResponse RpcDispatcher::on_set_collect_mode(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("mode"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'mode' parameter");
    CollectMode mode = static_cast<CollectMode>(params["mode"].get<int>());
    return wrap_void(id, session->set_collect_mode(mode));
}

JsonRpcResponse RpcDispatcher::on_get_logic_waveform(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("start") || !params.contains("end") || !params.contains("channels"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'start', 'end', or 'channels' parameter");

    uint64_t start = params["start"].get<uint64_t>();
    uint64_t end = params["end"].get<uint64_t>();
    std::vector<int16_t> channels = params["channels"].get<std::vector<int16_t>>();
    std::vector<uint8_t> out_data;
    auto r = session->get_logic_samples(start, end, channels, out_data);
    if (!r) {
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }
    json result = {
        {"sample_count", r.value()},
        {"data", base64_encode(out_data)}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_analog_waveform(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("start") || !params.contains("end") || !params.contains("channel"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'start', 'end', or 'channel' parameter");

    uint64_t start = params["start"].get<uint64_t>();
    uint64_t end = params["end"].get<uint64_t>();
    int16_t channel = params["channel"].get<int16_t>();
    std::vector<float> out_data;
    auto r = session->get_analog_samples(start, end, channel, out_data);
    if (!r) {
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }
    json result = {
        {"sample_count", r.value()},
        {"data", out_data}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_dso_waveform(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("start") || !params.contains("end") || !params.contains("channel"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'start', 'end', or 'channel' parameter");

    uint64_t start = params["start"].get<uint64_t>();
    uint64_t end = params["end"].get<uint64_t>();
    int16_t channel = params["channel"].get<int16_t>();
    std::vector<float> out_data;
    auto r = session->get_dso_samples(start, end, channel, out_data);
    if (!r) {
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }
    json result = {
        {"sample_count", r.value()},
        {"data", out_data}
    };
    return success_resp(id, result);
}

JsonRpcResponse RpcDispatcher::on_get_available_decoders(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    auto decoders = session->get_available_decoders();
    json arr = json::array();
    for (const auto& d : decoders)
        arr.push_back(to_json(d));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_add_decoder(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("id"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'id' parameter");

    std::string decoder_id = params["id"].get<std::string>();
    std::map<std::string, std::string> options;
    if (params.contains("options") && params["options"].is_object()) {
        for (auto& [k, v] : params["options"].items())
            options[k] = v.get<std::string>();
    }
    std::map<std::string, int16_t> channel_map;
    if (params.contains("channel_map") && params["channel_map"].is_object()) {
        for (auto& [k, v] : params["channel_map"].items())
            channel_map[k] = v.get<int16_t>();
    }
    auto r = session->add_decoder(decoder_id, options, channel_map, "", false);
    return wrap_result(id, r);
}

JsonRpcResponse RpcDispatcher::on_remove_decoder(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("instance_id"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'instance_id' parameter");
    std::string instance_id = params["instance_id"].get<std::string>();
    return wrap_void(id, session->remove_decoder(instance_id));
}

JsonRpcResponse RpcDispatcher::on_get_decoder_annotations(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("instance_id"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'instance_id' parameter");

    std::string instance_id = params["instance_id"].get<std::string>();
    uint64_t start = params.value("start", uint64_t(0));
    uint64_t end = params.value("end", UINT64_MAX);
    int max_count = params.value("max_count", 1000);

    auto r = session->get_decoder_annotations(instance_id, start, end, max_count);
    if (!r) {
        return error_resp(id, static_cast<int>(r.error().code), r.error().message);
    }
    json arr = json::array();
    for (const auto& a : r.value())
        arr.push_back(to_json(a));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_get_measurements(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    auto measurements = session->get_measurements();
    json arr = json::array();
    for (const auto& m : measurements)
        arr.push_back(to_json(m));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_get_cursors(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    auto cursors = session->get_cursors();
    json arr = json::array();
    for (const auto& c : cursors)
        arr.push_back(to_json(c));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_add_cursor(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("sample_pos"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'sample_pos' parameter");
    uint64_t sample_pos = params["sample_pos"].get<uint64_t>();
    return wrap_void(id, session->add_cursor(sample_pos));
}

JsonRpcResponse RpcDispatcher::on_remove_cursor(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("index"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'index' parameter");
    int index = params["index"].get<int>();
    return wrap_void(id, session->remove_cursor(index));
}

JsonRpcResponse RpcDispatcher::on_clear_cursors(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");
    return wrap_void(id, session->clear_cursors());
}

JsonRpcResponse RpcDispatcher::on_set_glitch_filter(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    GlitchFilterConfig config;
    if (params.contains("channels"))
        config.channels = params["channels"].get<std::vector<int32_t>>();
    if (params.contains("thresholds"))
        config.thresholds = params["thresholds"].get<std::vector<int32_t>>();
    else if (params.contains("threshold")) {
        // MCP singular form: apply same threshold to all channels
        int32_t th = params["threshold"].get<int32_t>();
        config.thresholds.resize(config.channels.size(), th);
    }
    if (params.contains("modes")) {
        auto modes_int = params["modes"].get<std::vector<int>>();
        config.modes.reserve(modes_int.size());
        for (int m : modes_int)
            config.modes.push_back(static_cast<GlitchFilterMode>(m));
    }
    return wrap_void(id, session->set_glitch_filter(config));
}

JsonRpcResponse RpcDispatcher::on_clear_glitch_filter(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return wrap_void(id, session->clear_glitch_filter());
}

JsonRpcResponse RpcDispatcher::on_set_signal_invert(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    SignalInvertConfig config;
    if (params.contains("channels"))
        config.channels = params["channels"].get<std::vector<int32_t>>();
    if (params.contains("invert_states"))
        config.invert_states = params["invert_states"].get<std::vector<bool>>();
    else
        // MCP form: only 'channels' provided — default to inverting all listed channels
        config.invert_states.resize(config.channels.size(), true);
    return wrap_void(id, session->set_signal_invert(config));
}

JsonRpcResponse RpcDispatcher::on_clear_signal_invert(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return wrap_void(id, session->clear_signal_invert());
}

JsonRpcResponse RpcDispatcher::on_save_file(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("path"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'path' parameter");
    std::string path = params["path"].get<std::string>();
    return wrap_void(id, session->save_file(path));
}

JsonRpcResponse RpcDispatcher::on_load_file(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("path"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest), "Missing 'path' parameter");
    std::string path = params["path"].get<std::string>();
    return wrap_void(id, session->load_file(path));
}

JsonRpcResponse RpcDispatcher::on_export_data(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    ExportConfig config;
    if (params.contains("output_path"))
        config.output_path = params["output_path"].get<std::string>();
    if (params.contains("channels"))
        config.channels = params["channels"].get<std::vector<int32_t>>();
    if (params.contains("start_sample"))
        config.start_sample = params["start_sample"].get<uint64_t>();
    if (params.contains("end_sample"))
        config.end_sample = params["end_sample"].get<uint64_t>();
    if (params.contains("is_logic"))
        config.is_logic = params["is_logic"].get<bool>();
    if (params.contains("include_headers"))
        config.include_headers = params["include_headers"].get<bool>();
    return wrap_void(id, session->export_data(config));
}

JsonRpcResponse RpcDispatcher::on_get_time_info(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return success_resp(id, to_json(session->get_time_info()));
}

JsonRpcResponse RpcDispatcher::on_get_disk_cache_info(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return success_resp(id, to_json(session->get_disk_cache_info()));
}

JsonRpcResponse RpcDispatcher::on_get_device_info(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return success_resp(id, to_json(session->get_device_info()));
}

JsonRpcResponse RpcDispatcher::on_get_work_mode(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    return success_resp(id, static_cast<int>(session->get_work_mode()));
}

JsonRpcResponse RpcDispatcher::on_get_signal_list(int id, const json& /*params*/) {
    auto session = app_svc_->get_active_session();
    auto sig_list = session->get_signal_list();
    json arr = json::array();
    for (const auto& s : sig_list)
        arr.push_back(to_json(s));
    return success_resp(id, arr);
}

JsonRpcResponse RpcDispatcher::on_find_next_edge(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!params.contains("from") || !params.contains("channel"))
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'from' or 'channel' parameter");

    uint64_t from = params["from"].get<uint64_t>();
    int16_t channel = params["channel"].get<int16_t>();
    bool rising = params.value("rising", true);
    auto r = session->find_next_edge(from, channel, rising);
    return wrap_result(id, r);
}

// ============================================================================
// P1-3: Batch call — execute multiple JSON-RPC calls in one request
// ============================================================================

JsonRpcResponse RpcDispatcher::on_batch_call(int id, const json& params) {
    if (!params.contains("calls") || !params["calls"].is_array())
        return error_resp(id, static_cast<int>(ErrorCode::InvalidRequest),
                          "Missing 'calls' array parameter");

    json results = json::array();

    for (const auto& call : params["calls"]) {
        std::string method = call.value("method", "");
        json call_params = call.value("params", json::object());

        // Build a sub-request and dispatch through ourselves
        JsonRpcRequest sub_req;
        sub_req.method = method;
        sub_req.params_json = call_params.dump();
        sub_req.id = id;  // Use parent id; the result is extracted inline

        JsonRpcResponse sub_resp = handle_request(sub_req);

        json result_entry;
        if (sub_resp.success) {
            result_entry["success"] = true;
            if (!sub_resp.result_json.empty()) {
                result_entry["result"] = json::parse(sub_resp.result_json);
            } else {
                result_entry["result"] = nullptr;
            }
        } else {
            result_entry["success"] = false;
            if (!sub_resp.error_json.empty()) {
                result_entry["error"] = json::parse(sub_resp.error_json);
            } else {
                result_entry["error"] = {{"code", -1}, {"message", "Unknown error"}};
            }
        }
        results.push_back(result_entry);
    }

    return success_resp(id, results);
}

// ============================================================================
// P0-3: Binary viewport data — return waveform data as a binary frame
// ============================================================================

JsonRpcResponse RpcDispatcher::on_get_viewport_binary(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    if (!session)
        return error_resp(id, static_cast<int>(ErrorCode::MissingDevice),
                          "No active session");

    // Parse parameters
    uint64_t start_sample = params.value("start_sample", uint64_t(0));
    uint64_t end_sample = params.value("end_sample", uint64_t(0));
    int32_t width_px = params.value("width", 1920);
    std::string data_type = params.value("type", std::string("logic"));

    // Channel list
    std::vector<int16_t> channels;
    if (params.contains("channels") && params["channels"].is_array()) {
        for (const auto& ch : params["channels"]) {
            channels.push_back(ch.get<int16_t>());
        }
    }
    if (channels.empty()) {
        // Default to channel 0
        channels.push_back(0);
    }

    // Build signal mask
    uint16_t signal_mask = 0;
    for (int16_t ch : channels) {
        if (ch >= 0 && ch < 16)
            signal_mask |= (1 << ch);
    }

    uint32_t timestamp_ms = static_cast<uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

    JsonRpcResponse resp;
    resp.id = id;
    resp.success = true;
    resp.is_binary = true;
    resp.binary_content_type = "application/octet-stream";

    if (data_type == "logic") {
        // Fetch logic samples and encode as edge data
        std::vector<std::vector<std::pair<uint64_t, uint8_t>>> edges_per_channel(16);

        for (int16_t ch : channels) {
            if (ch < 0 || ch >= 16)
                continue;

            std::vector<uint8_t> raw_data;
            std::vector<int16_t> ch_vec = {ch};
            auto r = session->get_logic_samples(start_sample, end_sample, ch_vec, raw_data);

            if (r && !raw_data.empty()) {
                // Convert raw samples to edges (transition detection)
                uint8_t prev = raw_data[0] ? 1 : 0;
                edges_per_channel[ch].push_back({start_sample, prev});

                for (size_t i = 1; i < raw_data.size(); ++i) {
                    uint8_t cur = raw_data[i] ? 1 : 0;
                    if (cur != prev) {
                        edges_per_channel[ch].push_back(
                            {start_sample + i, cur});
                        prev = cur;
                    }
                }
            }
        }

        resp.binary_payload = BinaryCodec::encode_logic_edges(
            timestamp_ms, start_sample, edges_per_channel, signal_mask);

        json result_header = {
            {"binary", true},
            {"frame_type", "logic_edges"},
            {"start_sample", start_sample},
            {"signal_mask", signal_mask},
            {"size", resp.binary_payload.size()}
        };
        resp.result_json = result_header.dump();
    } else {
        // Analog/DSO envelope data
        std::vector<std::vector<float>> envelopes_per_channel(16);

        for (int16_t ch : channels) {
            if (ch < 0 || ch >= 16)
                continue;

            std::vector<float> raw_data;
            if (data_type == "analog") {
                auto r = session->get_analog_samples(start_sample, end_sample, ch, raw_data);
                (void)r;
            } else {
                // DSO
                auto r = session->get_dso_samples(start_sample, end_sample, ch, raw_data);
                (void)r;
            }

            if (!raw_data.empty()) {
                // Simple envelope: compute min/max pairs at a downsample rate
                uint32_t total_samples = static_cast<uint32_t>(raw_data.size());
                uint32_t target_pairs = static_cast<uint32_t>(width_px);
                if (target_pairs == 0) target_pairs = 1920;
                uint32_t scale = (total_samples + target_pairs - 1) / target_pairs;
                if (scale == 0) scale = 1;

                for (uint32_t i = 0; i < total_samples; i += scale) {
                    float mn = raw_data[i];
                    float mx = raw_data[i];
                    uint32_t end_j = std::min(i + scale, total_samples);
                    for (uint32_t j = i + 1; j < end_j; ++j) {
                        if (raw_data[j] < mn) mn = raw_data[j];
                        if (raw_data[j] > mx) mx = raw_data[j];
                    }
                    envelopes_per_channel[ch].push_back(mn);
                    envelopes_per_channel[ch].push_back(mx);
                }
            }
        }

        // Use a scale of 1 for now (the real scale depends on sample rate / width)
        resp.binary_payload = BinaryCodec::encode_analog_envelope(
            timestamp_ms, start_sample, 1, envelopes_per_channel, signal_mask);

        json result_header = {
            {"binary", true},
            {"frame_type", "analog_envelope"},
            {"start_sample", start_sample},
            {"signal_mask", signal_mask},
            {"size", resp.binary_payload.size()}
        };
        resp.result_json = result_header.dump();
    }

    return resp;
}

} // namespace pv::api
