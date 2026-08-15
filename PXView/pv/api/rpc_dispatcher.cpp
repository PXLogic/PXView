#include "pv/api/rpc_dispatcher.h"
#include "pv/mcp/mcp.h"

// MCP tool registry — creates and configures the McpServer with all tools
namespace mcp {
std::unique_ptr<McpServer> create_mcp_server(pv::api::IAppService* app_svc);
}

namespace pv::api {

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
    // Shared mapping (types.h) keeps RPC + MCP serializers in sync.
    const char* state_str = capture_state_to_str(s.state);
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

    // ---- Legacy JSON-RPC routing — REMOVED ----
    // All tool dispatch goes through the MCP SDK path above.
    // Legacy WebSocket clients must use the MCP protocol
    // (initialize → tools/list → tools/call).
    return error_resp(req.id, static_cast<int>(ErrorCode::InvalidRequest),
                      "Legacy JSON-RPC methods are no longer supported. "
                      "Use MCP protocol: initialize → tools/list → tools/call.");
}

// ---- MCP Tool Implementations ----
// ---- Batch A MCP Tool Implementations ----
// ---- Batch B MCP Tool Implementations ----
// ---- Legacy Method Implementations (WebSocket transport) ----
// ============================================================================
// P1-3: Batch call — execute multiple JSON-RPC calls in one request
// ============================================================================
// ============================================================================
// P0-3: Binary viewport data — return waveform data as a binary frame
// ============================================================================
} // namespace pv::api
