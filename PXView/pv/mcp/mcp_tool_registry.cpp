// mcp_tool_registry.cpp — Register all MCP tools using the SDK
//
// This file replaces both tool_schemas.inc (46 KB of hand-written JSON)
// and the dispatch_mcp_tool() if-chain in rpc_dispatcher.cpp.
//
// 49 consolidated tools (down from 65 originals):
//   Tier 0: Mode management (3)     — switch/get_work_mode, get_supported_work_modes
//   Tier 1: Core workflow (18)      — devices, capture, analyzers, channels, export
//   Tier 2: Configuration (12)      — sample config, channel, trigger, probe, glitch, invert, config
//   Tier 3: Advanced features (16)  — samples, edges, decoders, sessions, math/spectrum, cursors
//
// Licensed under GPL v2 or (at your option) any later version.

#include "pv/mcp/mcp.h"
#include "pv/api/iapp_service.h"
#include "pv/api/isession_service.h"
#include "pv/api/types.h"
#include "PXView/config.h"

#include <algorithm>
#include <cstring>

namespace mcp {

using json = nlohmann::json;
using namespace pv::api;

// ──────────────────────────────────────────────────────────────────
//  Local serialization helpers (mirror rpc_dispatcher::to_json)
// ──────────────────────────────────────────────────────────────────

namespace {

json device_to_json(const DeviceInfo& d) {
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

json channel_to_json(const ChannelInfo& c) {
    return json{
        {"index",           c.index},
        {"name",            c.name},
        {"type",            static_cast<int>(c.type)},
        {"enabled",         c.enabled},
        {"enabled_default", c.enabled_default}
    };
}

json sample_config_to_json(const SampleConfig& s) {
    return json{
        {"sample_rate",          s.sample_rate},
        {"sample_limit",         s.sample_limit},
        {"time_base",            s.time_base},
        {"collect_mode",         static_cast<int>(s.collect_mode)},
        {"stream_mode",          s.stream_mode},
        {"rle_enabled",          s.rle_enabled},
        {"repeat_interval",      s.repeat_interval},
        {"repeat_hold_percent",  s.repeat_hold_percent}
    };
}

json capture_status_to_json(const CaptureStatus& s) {
    return json{
        {"state",                  static_cast<int>(s.state)},
        {"is_instant",             s.is_instant},
        {"is_saving",              s.is_saving},
        {"have_view_data",         s.have_view_data},
        {"have_hardware_data",     s.have_hardware_data},
        {"have_decoded_result",    s.have_decoded_result},
        {"is_glitch_filter_active", s.is_glitch_filter_active},
        {"is_signal_invert_active", s.is_signal_invert_active},
        {"progress",               s.progress},
        {"triggered",              s.triggered}
    };
}

json glitch_config_to_json(const GlitchFilterConfig& c) {
    json j = {
        {"channels",  c.channels},
        {"thresholds", c.thresholds}
    };
    json modes = json::array();
    for (auto m : c.modes) modes.push_back(static_cast<int>(m));
    j["modes"] = modes;
    return j;
}

json signal_invert_to_json(const SignalInvertConfig& c) {
    return json{
        {"channels",     c.channels},
        {"invert_states", c.invert_states}
    };
}

json logic_trigger_to_json(const LogicTriggerConfig& c) {
    return json{
        {"stage_count", c.stage_count},
        {"config_json", c.config_json}
    };
}

json dso_trigger_to_json(const DsoTriggerConfig& c) {
    return json{
        {"source",    static_cast<int>(c.source)},
        {"slope",     static_cast<int>(c.slope)},
        {"horiz_pos", c.horiz_pos},
        {"holdoff",   c.holdoff},
        {"margin",    c.margin},
        {"channel",   c.channel}
    };
}

json probe_to_json(const ProbeConfig& p) {
    return json{
        {"vdiv",        p.vdiv},
        {"coupling",    static_cast<int>(p.coupling)},
        {"vfactor",     p.vfactor},
        {"map_default", p.map_default}
    };
}

json disk_cache_to_json(const DiskCacheInfo& d) {
    return json{
        {"enabled",           d.enabled},
        {"write_speed_mbps",  d.write_speed_mbps},
        {"write_queue_depth", d.write_queue_depth},
        {"is_disk_full",      d.is_disk_full}
    };
}

json decoder_desc_to_json(const DecoderDescriptor& d) {
    json ch = json::array();
    for (const auto& c : d.channel_info) {
        ch.push_back({
            {"id", c.id}, {"name", c.name},
            {"desc", c.desc}, {"order", c.order},
            {"is_optional", c.is_optional}
        });
    }
    return json{
        {"id",               d.id},
        {"name",             d.name},
        {"long_name",        d.long_name},
        {"channels",         d.channels},
        {"optional_channels", d.optional_channels},
        {"channel_info",     ch}
    };
}

json decoder_inst_to_json(const DecoderInstance& d) {
    return json{
        {"instance_id",  d.instance_id},
        {"decoder_id",   d.decoder_id},
        {"display_name", d.display_name},
        {"row_index",    d.row_index},
        {"is_running",   d.is_running},
        {"progress",     d.progress}
    };
}

json decoder_ann_to_json(const DecoderAnnotation& a) {
    return json{
        {"start_sample", a.start_sample},
        {"end_sample",   a.end_sample},
        {"ann_class",    a.ann_class},
        {"texts",        a.texts}
    };
}

json error_state_to_json(const ErrorState& e) {
    return json{
        {"has_error",     e.has_error},
        {"error_code",    e.error_code},
        {"error_pattern", e.error_pattern},
        {"error_message", e.error_message}
    };
}

json math_result_to_json(const MathResult& m) {
    return json{
        {"is_enabled", m.is_enabled},
        {"ch1_index",  m.ch1_index},
        {"ch2_index",  m.ch2_index},
        {"math_type",  m.math_type},
        {"sample_num", m.sample_num},
        {"samples",    m.samples}
    };
}

json spectrum_result_to_json(const SpectrumResult& s) {
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

json lissajous_result_to_json(const LissajousResult& l) {
    return json{
        {"is_enabled", l.is_enabled},
        {"x_index",    l.x_index},
        {"y_index",    l.y_index},
        {"percent",    l.percent}
    };
}

json cursor_to_json(const CursorInfo& c) {
    return json{
        {"index",      c.index},
        {"sample_pos", c.sample_pos}
    };
}

json decoder_class_to_json(const DecoderClassInfo& d) {
    return json{
        {"class_id",   d.class_id},
        {"class_name", d.class_name}
    };
}

// Helper: get devices list with active flag
json get_devices_json(IAppService* app_svc) {
    auto devices = app_svc->get_device_list();
    auto* session = app_svc->get_active_session();
    std::string active_id;
    DeviceInfo dinfo;
    if (session) {
        dinfo = session->get_device_info();
        active_id = dinfo.id;
    }
    json arr = json::array();
    for (const auto& d : devices) {
        json j = (d.id == active_id) ? device_to_json(dinfo) : device_to_json(d);
        j["is_active"] = (d.id == active_id);
        arr.push_back(j);
    }
    return arr;
}

} // anonymous namespace

// ──────────────────────────────────────────────────────────────────
//  create_mcp_server — build and return a fully configured McpServer
// ──────────────────────────────────────────────────────────────────

std::unique_ptr<McpServer>
create_mcp_server(IAppService* app_svc) {
    auto server = std::make_unique<McpServer>("pxview", DS_VERSION_STRING);

    // Load instructions from file (Layer 1 of three-layer guidance)
    server->set_instructions_file("pv/mcp/mcp_instructions.txt");

    // ════════════════════════════════════════════════════════════════
    //  Tier 0: Mode Management (3 tools)
    // ════════════════════════════════════════════════════════════════

    // switch_work_mode — switch the device work mode
    server->tool("switch_work_mode",
        "Switch the device work mode. MUST be called before configuring "
        "channels/triggers/probes. Modes: 0=Logic (digital channels, logic "
        "triggers, glitch filter, RLE), 1=DSO (oscilloscope, DSO triggers, "
        "probe config, math/spectrum), 2=Analog (analog/DAQ channels), "
        "3=MSO (mixed signal). Use get_supported_work_modes to check "
        "which modes the device supports.")
        .param<int8_t>("mode", "Work mode: 0=Logic, 1=DSO, 2=Analog, 3=MSO",
                        Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto mode = static_cast<WorkMode>(p.get<int8_t>("mode"));
            if (static_cast<int>(mode) < 0 || static_cast<int>(mode) > 3)
                throw ToolError("Invalid mode value. Use 0=Logic, 1=DSO, "
                                "2=Analog, 3=MSO.");
            auto r = session->switch_work_mode(mode);
            check_void(r);
            return json_result({{"success", true},
                {"mode", static_cast<int>(mode)}});
        });

    // get_work_mode — query current work mode
    server->tool_void("get_work_mode",
        "Get the current device work mode. Returns: 0=Logic, 1=DSO, "
        "2=Analog, 3=MSO, -1=Unknown. Call this to determine which "
        "channels/triggers/probe configs are available.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            return json_result({{"mode",
                static_cast<int>(session->get_work_mode())}});
        });

    // get_supported_work_modes — query device capabilities
    server->tool_void("get_supported_work_modes",
        "Get the work modes supported by the current device. Returns an "
        "array of mode integers (0=Logic, 1=DSO, 2=Analog, 3=MSO). Call "
        "this before switch_work_mode to check device capabilities.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto r = session->get_supported_work_modes();
            if (!r) throw ToolError(r.error().message);
            json arr = json::array();
            for (auto m : r.value())
                arr.push_back(static_cast<int>(m));
            return json_result({{"modes", arr}});
        });

    // ════════════════════════════════════════════════════════════════
    //  Tier 1: Core Workflow (18 tools)
    // ════════════════════════════════════════════════════════════════

    // get_devices
    server->tool("get_devices",
        "List connected devices and their IDs. Call this first to discover "
        "available devices before starting a capture.")
        .read_only()
        .on_call([app_svc](const Params&) -> ToolResult {
            return json_result(get_devices_json(app_svc));
        });

    // start_capture (simplified: trigger config removed — use configure_trigger)
    server->tool("start_capture",
        "Start a capture in the current work mode. Trigger configuration "
        "should be set separately via configure_trigger BEFORE calling "
        "this tool. Typical workflow: 1) get_devices, 2) switch_work_mode, "
        "3) configure_channel, 4) set_sample_config, 5) configure_trigger, "
        "6) start_capture, 7) wait_capture, 8) get_analyzer_results. "
        "In Stream mode (channelMode='Stream'), durationSeconds and "
        "sampleCount are ignored — use stop_capture to end streaming.")
        .param<std::string>("deviceId", "Device ID (optional, uses active device if omitted)")
        .array_param<int16_t>("digitalChannels", "Digital channel indices (Logic/MSO mode)")
        .array_param<int16_t>("analogChannels", "Analog channel indices (Analog/MSO mode)")
        .param<uint64_t>("digitalSampleRate", "Digital sample rate in Hz")
        .param<uint64_t>("analogSampleRate", "Analog sample rate in Hz")
        .param<double>("digitalThresholdVolts", "Digital threshold voltage (e.g. 1.8)")
        .param<std::string>("captureMode", "Capture mode: 'timed', 'manual', 'stream'")
        .param<double>("durationSeconds", "Duration in seconds (timed mode, ignored in Stream mode)")
        .param<std::string>("channelMode", "Buffer/Stream mode")
        .param<bool>("rleEnabled", "Enable RLE compression (Logic mode)")
        .param<uint64_t>("sampleCount", "Sample count limit (ignored in Stream mode)")
        .param<double>("repeatIntervalSeconds", "Repeat interval in seconds")
        .param<bool>("instant", "Start instantly without buffering")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);

            // Auto-connect device if specified
            auto device_id = p.get_or<std::string>("deviceId", "");
            if (!device_id.empty()) {
                auto r = app_svc->create_session(device_id, "");
                if (!r.ok())
                    throw ToolError("Failed to create session: " +
                                    r.error().message);
                session = app_svc->get_active_session();
            }
            if (!session)
                throw ToolError("No active session. Provide deviceId to "
                                "auto-create one.");

            auto digital_channels = p.get_array_or<int16_t>(
                "digitalChannels", {});
            auto analog_channels = p.get_array_or<int16_t>(
                "analogChannels", {});
            auto digital_rate = p.get_or<uint64_t>("digitalSampleRate", 0);
            auto analog_rate = p.get_or<uint64_t>("analogSampleRate", 0);
            auto threshold = p.get_or<double>("digitalThresholdVolts", 0.0);
            auto capture_mode = p.get_or<std::string>("captureMode", "manual");
            auto duration = p.get_or<double>("durationSeconds", 0.0);
            auto channel_mode = p.get_or<std::string>("channelMode", "");
            auto rle = p.get_or<bool>("rleEnabled", false);
            auto sample_count = p.get_or<uint64_t>("sampleCount", 0);
            auto repeat_int = p.get_or<double>("repeatIntervalSeconds", 0.0);
            auto instant = p.get_or<bool>("instant", false);

            // Trigger config is now set separately via configure_trigger.
            // Pass trigger defaults (-1, "") to indicate no inline trigger.
            auto r = session->configure_and_start(
                digital_channels, analog_channels,
                digital_rate, analog_rate, threshold,
                {}, capture_mode, duration, instant,
                -1, "", 0.0, 0.0, 0.0, {},
                channel_mode, rle, 0.0, 0.0, false, "",
                "", "", "", "", "", -1, repeat_int, sample_count);
            if (!r)
                throw ToolError(r.error().message);
            return json_result({{"started", true},
                {"capture_id", r.value()}});
        });

    // stop_capture
    server->tool_void("stop_capture",
        "Stop the current capture. Use in Stream mode or to abort a "
        "timed capture early.")
        .destructive()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            check_void(session->stop_capture());
            return text("stopped");
        });

    // wait_capture
    server->tool("wait_capture",
        "Wait for the current capture to complete. Returns when the "
        "capture is done or timeout is reached.")
        .param<uint64_t>("timeoutMs", "Timeout in milliseconds (default 300000)")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto timeout = p.get_or<uint64_t>("timeoutMs", 300000);
            auto r = session->wait_capture_complete(timeout);
            if (!r)
                throw ToolError(r.error().message);
            return json_result({{"completed", true}});
        });

    // get_capture_status
    server->tool_void("get_capture_status",
        "Get the current capture status (state, sample count, sample rate).")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            return json_result(capture_status_to_json(
                session->get_capture_status()));
        });

    // load_capture
    server->tool("load_capture",
        "Load a previously saved capture from a file.")
        .param<std::string>("filePath", "Path to the capture file", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto path = p.get<std::string>("filePath");
            auto r = session->load_file(path);
            check_void(r);
            return text("loaded");
        });

    // save_capture
    server->tool("save_capture",
        "Save the current capture to a file.")
        .param<std::string>("filePath", "Output file path", Required)
        .param<uint64_t>("startSample", "Start sample for partial save")
        .param<uint64_t>("endSample", "End sample for partial save (0 = all)")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto path = p.get<std::string>("filePath");
            auto start = p.get_or<uint64_t>("startSample", 0);
            auto end = p.get_or<uint64_t>("endSample", 0);
            if (start > 0 || end > 0)
                check_void(session->set_save_range(start, end));
            auto r = session->save_file(path);
            check_void(r);
            return text("saved");
        });

    // close_capture
    server->tool_void("close_capture",
        "Close the current capture and release its data.")
        .destructive()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            check_void(session->close_capture());
            return text("closed");
        });

    // add_analyzer
    server->tool("add_analyzer",
        "Add a protocol decoder (analyzer). Only available in Logic/MSO "
        "mode (decoders need digital channels). Best called BEFORE "
        "start_capture for auto-decode. Use stackOnAnalyzerId to stack "
        "decoders (e.g., SPI on top of I2C).")
        .param<std::string>("decoderId", "Decoder ID (e.g. 'i2c', 'spi')", Required)
        .param<std::string>("label", "Display label for this analyzer instance")
        .param<std::string>("stackOnAnalyzerId", "Instance ID to stack on top of")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto decoder_id = p.get<std::string>("decoderId");
            auto label = p.get_or<std::string>("label", "");
            auto stack_on = p.get_or<std::string>("stackOnAnalyzerId", "");

            // Parse options and channelMap from raw JSON
            std::map<std::string, std::string> options;
            std::map<std::string, int16_t> channel_map;
            const auto& raw = p.raw();
            if (raw.contains("options") && raw["options"].is_object()) {
                for (auto& [k, v] : raw["options"].items()) {
                    if (v.is_string())
                        options[k] = v.get<std::string>();
                    else
                        options[k] = v.dump();
                }
            }
            if (raw.contains("channelMap") && raw["channelMap"].is_object()) {
                for (auto& [k, v] : raw["channelMap"].items()) {
                    if (v.is_number_integer())
                        channel_map[k] = v.get<int16_t>();
                }
            }

            auto r = session->add_decoder(
                decoder_id, options, channel_map, label,
                false, stack_on);
            if (!r)
                throw ToolError(r.error().message);
            return json_result({{"analyzerId", r.value()},
                {"success", true}});
        });

    // remove_analyzer
    server->tool("remove_analyzer",
        "Remove a previously added protocol decoder.")
        .param<std::string>("analyzerId", "Analyzer instance ID", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto id = p.get<std::string>("analyzerId");
            check_void(session->remove_decoder(id));
            return text("removed");
        });

    // list_analyzers
    server->tool_void("list_analyzers",
        "List all available protocol decoders and their channel "
        "requirements.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto decoders = session->get_available_decoders();
            json arr = json::array();
            for (const auto& d : decoders)
                arr.push_back(decoder_desc_to_json(d));
            return json_result(arr);
        });

    // get_analyzer_options
    server->tool("get_analyzer_options",
        "Get the configuration options for a specific protocol decoder.")
        .param<std::string>("decoderId", "Decoder ID", Required)
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto id = p.get<std::string>("decoderId");
            auto r = session->get_decoder_options(id);
            if (!r)
                throw ToolError(r.error().message);
            return json_result(r.value());
        });

    // get_analyzer_results
    server->tool("get_analyzer_results",
        "Read decoded protocol data from a previously added analyzer. "
        "Returns annotation array with sample ranges and text.")
        .param<std::string>("analyzerId", "Analyzer instance ID", Required)
        .param<uint64_t>("startSample", "Start sample (default 0)")
        .param<uint64_t>("endSample", "End sample (default = all)")
        .param<int>("maxCount", "Max annotations to return (default 1000)")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto id = p.get<std::string>("analyzerId");
            auto start = p.get_or<uint64_t>("startSample", 0);
            auto end = p.get_or<uint64_t>("endSample", UINT64_MAX);
            auto max_count = p.get_or<int>("maxCount", 1000);
            auto r = session->get_decoder_annotations(id, start, end, max_count);
            if (!r)
                throw ToolError(r.error().message);
            json arr = json::array();
            for (const auto& a : r.value())
                arr.push_back(decoder_ann_to_json(a));
            return json_result(arr);
        });

    // export_raw_data (merged: csv + binary + vcd + hex + bits)
    server->tool("export_raw_data",
        "Export raw sample data to files. Supports formats: 'csv', "
        "'binary', 'vcd', 'hex', 'bits'. Only available after capture "
        "is complete. Each channel is written to its own file "
        "(channel_N.<ext> / analog_N.<ext>).")
        .param<std::string>("format", "Output format: csv | binary | vcd | hex | bits (default: csv)")
        .param<std::string>("directory", "Output directory path", Required)
        .array_param<int32_t>("digitalChannels", "Digital channel indices")
        .array_param<int32_t>("analogChannels", "Analog channel indices")
        .param<int>("analogDownsampleRatio", "Analog downsample ratio (default 1)")
        .param<bool>("iso8601Timestamp", "Use ISO8601 timestamp in filename")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto format = p.get_or<std::string>("format", "csv");
            auto dir = p.get<std::string>("directory");
            auto dig_ch = p.get_array_or<int32_t>("digitalChannels", {});
            auto ana_ch = p.get_array_or<int32_t>("analogChannels", {});
            auto ratio = p.get_or<int>("analogDownsampleRatio", 1);
            auto iso = p.get_or<bool>("iso8601Timestamp", false);

            auto r = session->export_raw_data(
                format, dir, dig_ch, ana_ch, ratio, iso);
            check_void(r);
            return text("exported");
        });

    // export_data_table_csv
    server->tool("export_data_table_csv",
        "Export decoded analyzer data as a CSV table.")
        .param<std::string>("filePath", "Output CSV file path", Required)
        .param<std::string>("analyzerId", "Analyzer instance ID", Required)
        .param<int>("radixType", "Radix type: 1=Binary, 2=Decimal, 3=Hex, 4=Ascii")
        .param<bool>("iso8601Timestamp", "Use ISO8601 timestamp")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto path = p.get<std::string>("filePath");
            auto id = p.get<std::string>("analyzerId");
            auto radix = p.get_or<int>("radixType", 0);
            auto iso = p.get_or<bool>("iso8601Timestamp", false);
            auto r = session->export_data_table_csv(path, id, radix, iso);
            check_void(r);
            return text("exported");
        });

    // get_channels (enhanced: supports optional mode pre-query)
    server->tool("get_channels",
        "Get channel list for the current or specified work mode. If mode "
        "is specified and differs from the current mode, returns channel "
        "counts for that mode without switching (useful for exploring "
        "device capabilities before switch_work_mode). If mode matches the "
        "current mode or is omitted, returns full channel details for the "
        "current active mode.")
        .param<int8_t>("mode", "Optional: query channels for a specific mode "
                       "without switching (0=Logic, 1=DSO, 2=Analog, 3=MSO)")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto current_mode = session->get_work_mode();

            // If mode is specified and differs from current mode, return
            // channel counts for the requested mode without switching.
            if (p.has("mode")) {
                auto req_mode = static_cast<WorkMode>(
                    p.get<int8_t>("mode"));
                if (req_mode != current_mode) {
                    json result;
                    result["requested_mode"] =
                        static_cast<int>(req_mode);
                    result["current_mode"] =
                        static_cast<int>(current_mode);
                    result["note"] = "Channel details require switching. "
                        "Call switch_work_mode(" +
                        std::to_string(static_cast<int>(req_mode)) +
                        ") to switch to this mode, then call get_channels "
                        "again for full channel details.";
                    json counts = json::array();
                    switch (req_mode) {
                    case WorkMode::Logic:
                        counts.push_back({
                            {"type", "logic"},
                            {"count", session->get_channel_count(
                                ChannelType::Logic)}});
                        break;
                    case WorkMode::Dso:
                        counts.push_back({
                            {"type", "dso"},
                            {"count", session->get_channel_count(
                                ChannelType::Dso)}});
                        break;
                    case WorkMode::Analog:
                        counts.push_back({
                            {"type", "analog"},
                            {"count", session->get_channel_count(
                                ChannelType::Analog)}});
                        break;
                    case WorkMode::Mso:
                        counts.push_back({
                            {"type", "logic"},
                            {"count", session->get_channel_count(
                                ChannelType::Logic)}});
                        counts.push_back({
                            {"type", "analog"},
                            {"count", session->get_channel_count(
                                ChannelType::Analog)}});
                        break;
                    default:
                        break;
                    }
                    result["channel_counts"] = counts;
                    return json_result(result);
                }
            }

            // mode omitted or matches current mode: return full details
            auto channels = session->get_channels();
            json arr = json::array();
            for (const auto& c : channels)
                arr.push_back(channel_to_json(c));
            return json_result(arr);
        });

    // get_sample_config
    server->tool_void("get_sample_config",
        "Get the current sample configuration (sample rate, limit, "
        "time base, collect mode, repeat interval, etc.).")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            return json_result(sample_config_to_json(
                session->get_sample_config()));
        });

    // refresh_device_list
    server->tool_void("refresh_device_list",
        "Trigger a hot-plug rescan of all device drivers and return the "
        "updated device list. Use this when a USB device is connected or "
        "disconnected after startup.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto r = session->refresh_device_list();
            if (!r)
                throw ToolError(r.error().message);
            DeviceInfo dinfo = session->get_device_info();
            const std::string& active_id = dinfo.id;
            json arr = json::array();
            for (const auto& d : r.value()) {
                json j = (d.id == active_id) ? device_to_json(dinfo)
                                             : device_to_json(d);
                j["is_active"] = (d.id == active_id);
                arr.push_back(j);
            }
            return json_result(arr);
        });

    // ════════════════════════════════════════════════════════════════
    //  Tier 2: Configuration (10 tools)
    // ════════════════════════════════════════════════════════════════

    // set_sample_config (merged: rate + limit + timeBase + collectMode + repeat)
    server->tool("set_sample_config",
        "Set sample configuration parameters. All params optional — only "
        "provided params are updated. Sample rate type (digital/analog/dso) "
        "is determined by current work mode. Call get_sample_config to "
        "read current values.")
        .param<uint64_t>("sampleRate", "Sample rate in Hz (applies to current mode)")
        .param<uint64_t>("sampleLimit", "Sample count limit")
        .param<uint64_t>("timeBase", "Time base in samples")
        .param<int8_t>("collectMode", "0=Single, 1=Repeat, 2=Loop")
        .param<double>("repeatInterval", "Repeat interval in seconds")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            if (p.has("sampleRate"))
                check_void(session->set_sample_rate(
                    p.get<uint64_t>("sampleRate")));
            if (p.has("sampleLimit"))
                check_void(session->set_sample_limit(
                    p.get<uint64_t>("sampleLimit")));
            if (p.has("timeBase"))
                check_void(session->set_time_base(
                    p.get<uint64_t>("timeBase")));
            if (p.has("collectMode"))
                check_void(session->set_collect_mode(
                    static_cast<CollectMode>(
                        p.get<int8_t>("collectMode"))));
            if (p.has("repeatInterval"))
                check_void(session->set_repeat_interval(
                    p.get<double>("repeatInterval")));
            return json_result({{"success", true}});
        });

    // configure_channel (merged: set_channel_enabled + set_channel_name)
    server->tool("configure_channel",
        "Configure a channel: enable/disable and/or set display name. "
        "All params except channelIndex are optional — only provided "
        "params are updated.")
        .param<int16_t>("channelIndex", "Channel index", Required)
        .param<bool>("enabled", "Enable/disable the channel")
        .param<std::string>("name", "Display name for the channel")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto idx = p.get<int16_t>("channelIndex");
            if (p.has("enabled"))
                check_void(session->set_channel_enabled(
                    idx, p.get<bool>("enabled")));
            if (p.has("name"))
                check_void(session->set_channel_name(
                    idx, p.get<std::string>("name")));
            return json_result({{"success", true}});
        });

    // configure_trigger (merged: get + set, mode-aware)
    server->tool("configure_trigger",
        "Get or set trigger configuration. Automatically uses LogicTrigger "
        "or DsoTrigger based on current work mode. Call with no args to "
        "get current config. Include trigger params to set. "
        "Logic mode: stageCount + configJson. "
        "DSO mode: source, slope, horizPos, holdoff, margin, channel.")
        .param<int32_t>("stageCount", "Logic trigger: stage count")
        .param<std::string>("configJson", "Logic trigger: config JSON")
        .param<int32_t>("source", "DSO trigger: source (0=Auto, 1=CH0, 2=CH1)")
        .param<int32_t>("slope", "DSO trigger: slope (0=Rising, 1=Falling)")
        .param<double>("horizPos", "DSO trigger: horizontal position")
        .param<double>("holdoff", "DSO trigger: holdoff time")
        .param<double>("margin", "DSO trigger: margin")
        .param<int32_t>("channel", "DSO trigger: channel index")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto mode = session->get_work_mode();

            if (mode == WorkMode::Logic || mode == WorkMode::Mso) {
                // Logic trigger
                if (!p.has("stageCount") && !p.has("configJson")) {
                    // GET
                    return json_result(logic_trigger_to_json(
                        session->get_logic_trigger_config()));
                }
                // SET
                auto cfg = session->get_logic_trigger_config();
                if (p.has("stageCount"))
                    cfg.stage_count = p.get<int32_t>("stageCount");
                if (p.has("configJson"))
                    cfg.config_json = p.get<std::string>("configJson");
                check_void(session->set_logic_trigger_config(cfg));
                return json_result({{"success", true}});
            }

            if (mode == WorkMode::Dso || mode == WorkMode::Analog) {
                // DSO trigger
                if (!p.has("source") && !p.has("slope") &&
                    !p.has("horizPos") && !p.has("holdoff") &&
                    !p.has("margin") && !p.has("channel")) {
                    // GET
                    return json_result(dso_trigger_to_json(
                        session->get_dso_trigger_config()));
                }
                // SET
                auto cfg = session->get_dso_trigger_config();
                if (p.has("source"))
                    cfg.source = static_cast<TriggerSource>(
                        p.get<int32_t>("source"));
                if (p.has("slope"))
                    cfg.slope = static_cast<TriggerSlope>(
                        p.get<int32_t>("slope"));
                if (p.has("horizPos"))
                    cfg.horiz_pos = p.get<double>("horizPos");
                if (p.has("holdoff"))
                    cfg.holdoff = p.get<double>("holdoff");
                if (p.has("margin"))
                    cfg.margin = p.get<double>("margin");
                if (p.has("channel"))
                    cfg.channel = p.get<int32_t>("channel");
                check_void(session->set_dso_trigger_config(cfg));
                return json_result({{"success", true}});
            }

            throw ToolError("Trigger configuration is not available in "
                "the current work mode. Call switch_work_mode first.");
        });

    // configure_probe (merged: get + set)
    server->tool("configure_probe",
        "Get or set probe configuration (vdiv/coupling/vfactor). Only "
        "available in DSO/Analog/MSO mode — returns error in Logic mode. "
        "Call with just channelIndex to get current config.")
        .param<int16_t>("channelIndex", "Channel index", Required)
        .param<double>("vdiv", "Volts per division")
        .param<int8_t>("coupling", "Coupling: 0=DC, 1=AC")
        .param<double>("vfactor", "Voltage factor (probe attenuation)")
        .param<bool>("mapDefault", "Use default probe mapping")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto mode = session->get_work_mode();
            if (mode == WorkMode::Logic)
                throw ToolError("Probe configuration is not available in "
                    "Logic mode (current mode: 0). "
                    "Call switch_work_mode(1) for DSO or "
                    "switch_work_mode(2) for Analog.");

            auto idx = p.get<int16_t>("channelIndex");
            if (!p.has("vdiv") && !p.has("coupling") &&
                !p.has("vfactor") && !p.has("mapDefault")) {
                // GET
                return json_result(probe_to_json(
                    session->get_probe_config(idx)));
            }
            // SET
            auto cfg = session->get_probe_config(idx);
            if (p.has("vdiv"))
                cfg.vdiv = p.get<double>("vdiv");
            if (p.has("coupling"))
                cfg.coupling = static_cast<Coupling>(
                    p.get<int8_t>("coupling"));
            if (p.has("vfactor"))
                cfg.vfactor = p.get<double>("vfactor");
            if (p.has("mapDefault"))
                cfg.map_default = p.get<bool>("mapDefault");
            check_void(session->set_probe_config(idx, cfg));
            return json_result({{"success", true}});
        });

    // configure_glitch_filter (merged: get + set + clear)
    server->tool("configure_glitch_filter",
        "Get, set, or clear glitch filter (Logic/MSO mode only). "
        "Call with no args to get current config. "
        "Set channels+thresholds to enable. "
        "Set channels to [] to clear. "
        "Returns error in DSO/Analog mode.")
        .array_param<int32_t>("channels", "Channel indices. Empty array = clear.")
        .array_param<int32_t>("thresholds", "Min pulse width in samples per channel")
        .array_param<int32_t>("modes", "Filter mode per channel")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto mode = session->get_work_mode();
            if (mode == WorkMode::Dso || mode == WorkMode::Analog)
                throw ToolError("Glitch filter is not available in "
                    "DSO/Analog mode (current mode: " +
                    std::to_string(static_cast<int>(mode)) + "). "
                    "Call switch_work_mode(0) for Logic mode.");

            if (!p.has("channels")) {
                // GET
                auto cfg = session->get_glitch_filter_config();
                json j = glitch_config_to_json(cfg);
                j["is_active"] = !cfg.channels.empty();
                return json_result(j);
            }

            auto channels = p.get_array_or<int32_t>("channels", {});
            if (channels.empty()) {
                // CLEAR
                check_void(session->clear_glitch_filter());
                return json_result({{"cleared", true}});
            }
            // SET
            GlitchFilterConfig cfg;
            cfg.channels = channels;
            cfg.thresholds = p.get_array_or<int32_t>("thresholds", {});
            auto modes_arr = p.get_array_or<int32_t>("modes", {});
            for (auto m : modes_arr)
                cfg.modes.push_back(static_cast<pv::api::GlitchFilterMode>(m));
            check_void(session->set_glitch_filter(cfg));
            return json_result({{"success", true}});
        });

    // configure_signal_invert (merged: get + set + clear)
    server->tool("configure_signal_invert",
        "Get, set, or clear signal invert. Call with no args to get "
        "current config. Set channels+invertStates to enable. "
        "Set channels to [] to clear.")
        .array_param<int32_t>("channels", "Channel indices. Empty array = clear.")
        .array_param<bool>("invertStates", "Invert state per channel")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);

            if (!p.has("channels")) {
                // GET
                auto cfg = session->get_signal_invert_config();
                json j = signal_invert_to_json(cfg);
                j["is_active"] = !cfg.channels.empty();
                return json_result(j);
            }

            auto channels = p.get_array_or<int32_t>("channels", {});
            if (channels.empty()) {
                // CLEAR
                check_void(session->clear_signal_invert());
                return json_result({{"cleared", true}});
            }
            // SET
            SignalInvertConfig cfg;
            cfg.channels = channels;
            cfg.invert_states = p.get_array_or<bool>("invertStates", {});
            check_void(session->set_signal_invert(cfg));
            return json_result({{"success", true}});
        });

    // get_config — generic SR_CONF_* config reader
    server->tool("get_config",
        "Read a generic SR_CONF_* config value by key. The 'type' field "
        "selects how to interpret the value. NOTE: 'int64' currently maps "
        "to int32 (no int64 getter exists yet). Use this to access device "
        "options not covered by dedicated tools (e.g. PWM, VTH, Filter, "
        "ClockType, TriggerOut, RLE, BandwidthLimit, OperationMode, etc.).")
        .param<int32_t>("key", "SR_CONF_* config key (numeric)", Required)
        .param<std::string>("type", "Value type: bool, int, int64, string, double", Required)
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto key = p.get<int32_t>("key");
            auto type = p.get<std::string>("type");

            if (type == "string") {
                auto r = session->get_config_string(key);
                if (!r) throw ToolError(r.error().message);
                return json_result({{"value", r.value()}});
            }
            if (type == "bool") {
                auto r = session->get_config_bool(key);
                if (!r) throw ToolError(r.error().message);
                return json_result({{"value", r.value()}});
            }
            if (type == "uint64") {
                auto r = session->get_config_uint64(key);
                if (!r) throw ToolError(r.error().message);
                return json_result({{"value", r.value()}});
            }
            if (type == "int" || type == "int64") {
                auto r = session->get_config_int32(key);
                if (!r) throw ToolError(r.error().message);
                return json_result({{"value", r.value()}});
            }
            if (type == "double") {
                auto r = session->get_config_double(key);
                if (!r) throw ToolError(r.error().message);
                return json_result({{"value", r.value()}});
            }
            throw ToolError("Unsupported 'type': " + type +
                           ". Use: bool, int, int64, string, double, uint64.");
        });

    // set_config — generic SR_CONF_* config writer
    server->tool("set_config",
        "Write a generic SR_CONF_* config value by key. The 'type' field "
        "selects how to interpret the value. Use this to configure device "
        "options not covered by dedicated tools (e.g. PWM, VTH, Filter, "
        "ClockType, TriggerOut, RLE, BandwidthLimit, OperationMode, etc.).")
        .param<int32_t>("key", "SR_CONF_* config key (numeric)", Required)
        .param<std::string>("type", "Value type: bool, int, int64, string, double", Required)
        .param_desc({"value", "", "", "Value to set (type depends on 'type' field)",
                     true, false, {}, false, {}, std::type_index(typeid(void))})
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto key = p.get<int32_t>("key");
            auto type = p.get<std::string>("type");

            if (!p.has("value"))
                throw ToolError("Missing 'value' parameter");
            const auto& value = p.raw().at("value");

            if (type == "string") {
                auto r = session->set_config_string(key, value.get<std::string>());
                if (!r) throw ToolError(r.error().message);
                return json_result({{"success", true}});
            }
            if (type == "bool") {
                auto r = session->set_config_bool(key, value.get<bool>());
                if (!r) throw ToolError(r.error().message);
                return json_result({{"success", true}});
            }
            if (type == "uint64") {
                auto r = session->set_config_uint64(key, value.get<uint64_t>());
                if (!r) throw ToolError(r.error().message);
                return json_result({{"success", true}});
            }
            if (type == "int" || type == "int64") {
                auto r = session->set_config_int32(key, value.get<int32_t>());
                if (!r) throw ToolError(r.error().message);
                return json_result({{"success", true}});
            }
            if (type == "double") {
                auto r = session->set_config_double(key, value.get<double>());
                if (!r) throw ToolError(r.error().message);
                return json_result({{"success", true}});
            }
            throw ToolError("Unsupported 'type': " + type +
                           ". Use: bool, int, int64, string, double, uint64.");
        });

    // set_save_range
    server->tool("set_save_range",
        "Set the sample range for save/export operations.")
        .param<uint64_t>("startSample", "Start sample index", Required)
        .param<uint64_t>("endSample", "End sample index", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto start = p.get<uint64_t>("startSample");
            auto end = p.get<uint64_t>("endSample");
            check_void(session->set_save_range(start, end));
            return json_result({{"success", true}});
        });

    // connect_device
    server->tool("connect_device",
        "Connect to a specific device by ID. Creates a new session if "
        "needed. Use get_devices first to discover available device IDs.")
        .param<std::string>("deviceId", "Device ID to connect to", Required)
        .open_world()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto id = p.get<std::string>("deviceId");
            auto r = app_svc->create_session(id, "");
            if (!r.ok())
                throw ToolError("Failed to connect device: " +
                                r.error().message);
            return json_result({{"success", true},
                {"session_id", r.value()}});
        });

    // disconnect_device
    server->tool("disconnect_device",
        "Disconnect from a device. If no deviceId is provided, "
        "disconnects the active device.")
        .param<std::string>("deviceId", "Device ID to disconnect (optional)")
        .open_world()
        .on_call([app_svc](const Params& p) -> ToolResult {
            std::string id;
            if (p.has("deviceId")) {
                id = p.get<std::string>("deviceId");
            } else {
                auto r = app_svc->get_active_device();
                if (!r.ok())
                    throw ToolError("No active device to disconnect");
                id = r.value().id;
            }
            check_void(app_svc->disconnect_device(id));
            return json_result({{"success", true}});
        });

    // get_session_status (merged: get_repeat_status + get_disk_cache_info)
    server->tool_void("get_session_status",
        "Get session status including collect mode, repeat interval, "
        "and disk cache information.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto cfg = session->get_sample_config();
            auto cache = session->get_disk_cache_info();
            return json_result({
                {"collect_mode", static_cast<int>(cfg.collect_mode)},
                {"is_single_mode", cfg.collect_mode == CollectMode::Single},
                {"is_repeat_mode", cfg.collect_mode == CollectMode::Repeat},
                {"is_loop_mode", cfg.collect_mode == CollectMode::Loop},
                {"repeat_interval", cfg.repeat_interval},
                {"repeat_hold_percent", cfg.repeat_hold_percent},
                {"disk_cache", disk_cache_to_json(cache)}
            });
        });

    // ════════════════════════════════════════════════════════════════
    //  Tier 3: Advanced Features (15 tools)
    // ════════════════════════════════════════════════════════════════

    // get_samples (merged: get_logic + get_analog + get_dso samples)
    server->tool("get_samples",
        "Read raw samples from a channel. channelType must match the "
        "current work mode: 'logic' for Logic/MSO mode, 'analog' for "
        "Analog mode, 'dso' for DSO mode. Use get_work_mode to check "
        "current mode. Returns base64-encoded data for logic channels, "
        "float arrays for analog/DSO channels.")
        .param<int16_t>("channelIndex", "Channel index", Required)
        .enum_param<std::string>("channelType",
            {"logic", "analog", "dso"},
            "Channel type — must match current work mode", Required)
        .param<uint64_t>("startSample", "Start sample index (default 0)")
        .param<uint64_t>("endSample", "End sample index (default = all)")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto ch = p.get<int16_t>("channelIndex");
            auto type = p.get<std::string>("channelType");
            auto start = p.get_or<uint64_t>("startSample", 0);
            auto end = p.get_or<uint64_t>("endSample", UINT64_MAX);

            if (type == "logic") {
                std::vector<uint8_t> out_data;
                std::vector<int16_t> channels = {ch};
                auto r = session->get_logic_samples(
                    start, end, channels, out_data);
                if (!r)
                    throw ToolError(r.error().message);
                return json_result({
                    {"sample_count", r.value()},
                    {"data", base64_encode(out_data)},
                    {"encoding", "base64"}
                });
            }

            if (type == "analog") {
                std::vector<float> out_data;
                auto r = session->get_analog_samples(
                    start, end, ch, out_data);
                if (!r)
                    throw ToolError(r.error().message);
                return json_result({
                    {"sample_count", r.value()},
                    {"data", out_data},
                    {"encoding", "float32"}
                });
            }

            if (type == "dso") {
                std::vector<float> out_data;
                auto r = session->get_dso_samples(
                    start, end, ch, out_data);
                if (!r)
                    throw ToolError(r.error().message);
                return json_result({
                    {"sample_count", r.value()},
                    {"data", out_data},
                    {"encoding", "float32"}
                });
            }

            throw ToolError("Invalid channelType. Use 'logic', 'analog', "
                            "or 'dso'.");
        });

    // find_next_edge
    server->tool("find_next_edge",
        "Find the next signal edge (rising or falling) starting from "
        "a given sample position.")
        .param<uint64_t>("fromSample", "Start searching from this sample", Required)
        .param<int16_t>("channelIndex", "Channel index", Required)
        .param<bool>("risingEdge", "true=rising edge, false=falling edge")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto from = p.get<uint64_t>("fromSample");
            auto ch = p.get<int16_t>("channelIndex");
            auto rising = p.get_or<bool>("risingEdge", true);
            auto r = session->find_next_edge(from, ch, rising);
            if (!r)
                throw ToolError(r.error().message);
            return json_result({{"sample", r.value()}});
        });

    // find_pattern
    server->tool("find_pattern",
        "Find the next occurrence of a signal pattern on a channel starting "
        "from a given sample position. Returns the sample index of the next "
        "match.")
        .param<uint64_t>("fromSample", "Start searching from this sample", Required)
        .param<int16_t>("channelIndex", "Channel index", Required)
        .param<std::string>("pattern", "Pattern string to search for", Required)
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto from = p.get<uint64_t>("fromSample");
            auto ch = p.get<int16_t>("channelIndex");
            auto pattern = p.get<std::string>("pattern");
            auto r = session->find_pattern(from, ch, pattern);
            if (!r)
                throw ToolError(r.error().message);
            return json_result({{"sample", r.value()}});
        });

    // get_active_decoders
    server->tool_void("get_active_decoders",
        "Get the list of currently active (added) protocol decoders "
        "with their instance IDs and configurations.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto decoders = session->get_active_decoders();
            json arr = json::array();
            for (const auto& d : decoders)
                arr.push_back(decoder_inst_to_json(d));
            return json_result(arr);
        });

    // clear_all_decoders
    server->tool_void("clear_all_decoders",
        "Remove all active protocol decoders from the session.")
        .destructive()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            check_void(session->clear_all_decoders());
            return text("cleared");
        });

    // reconfigure_decoder
    server->tool("reconfigure_decoder",
        "Reconfigure an existing decoder's options and channel map "
        "in place (no remove + re-add). Triggers re-decode.")
        .param<std::string>("analyzerId", "Analyzer instance ID", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto id = p.get<std::string>("analyzerId");
            std::map<std::string, std::string> options;
            std::map<std::string, int> channel_map;
            const auto& raw = p.raw();
            if (raw.contains("options") && raw["options"].is_object()) {
                for (auto& [k, v] : raw["options"].items()) {
                    if (v.is_string())
                        options[k] = v.get<std::string>();
                    else
                        options[k] = v.dump();
                }
            }
            if (raw.contains("channelMap") && raw["channelMap"].is_object()) {
                for (auto& [k, v] : raw["channelMap"].items()) {
                    if (v.is_number_integer())
                        channel_map[k] = v.get<int>();
                }
            }
            check_void(session->reconfigure_decoder(id, options, channel_map));
            return json_result({{"success", true}});
        });

    // get_decoder_class_names
    server->tool("get_decoder_class_names",
        "Get the annotation class names declared by a decoder "
        "(the __annotations__ metadata).")
        .param<std::string>("analyzerName", "Decoder ID", Required)
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto id = p.get<std::string>("analyzerName");
            auto r = session->get_decoder_class_names(id);
            if (!r)
                throw ToolError(r.error().message);
            json arr = json::array();
            for (const auto& c : r.value())
                arr.push_back(decoder_class_to_json(c));
            return json_result(arr);
        });

    // list_sessions
    server->tool_void("list_sessions",
        "List all sessions with their device info and active status.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto ids = app_svc->get_session_ids();
            int active_id = app_svc->get_active_session_id();
            json arr = json::array();
            for (int sid : ids) {
                json s;
                s["session_id"] = sid;
                s["is_active"] = (sid == active_id);
                auto* sess = app_svc->get_session(sid);
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
            return json_result({
                {"sessions", arr},
                {"active_session_id", active_id},
                {"count", static_cast<int>(ids.size())}
            });
        });

    // create_session
    server->tool("create_session",
        "Create a new session. Optionally connect to a device or "
        "load a file.")
        .param<std::string>("deviceId", "Device ID to connect (optional)")
        .param<std::string>("filePath", "File path to load (optional)")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto device_id = p.get_or<std::string>("deviceId", "");
            auto file_path = p.get_or<std::string>("filePath", "");
            auto r = app_svc->create_session(device_id, file_path);
            if (!r.ok())
                throw ToolError(r.error().message);
            return json_result({{"session_id", r.value()},
                {"success", true}});
        });

    // destroy_session
    server->tool("destroy_session",
        "Destroy a session by ID.")
        .param<int>("sessionId", "Session ID to destroy", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto sid = p.get<int>("sessionId");
            check_void(app_svc->destroy_session(sid));
            return json_result({{"success", true}});
        });

    // set_active_session
    server->tool("set_active_session",
        "Switch the active session.")
        .param<int>("sessionId", "Session ID to make active", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto sid = p.get<int>("sessionId");
            check_void(app_svc->set_active_session(sid));
            return json_result({{"success", true}});
        });

    // get_math_results
    server->tool_void("get_math_results",
        "Get math operation results (ADD/SUB/MUL/DIV of two channels). "
        "Only available in DSO/Analog/MSO mode.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto r = session->get_math_results();
            if (!r)
                throw ToolError(r.error().message);
            return json_result(math_result_to_json(r.value()));
        });

    // get_spectrum_results
    server->tool_void("get_spectrum_results",
        "Get FFT spectrum analysis results. "
        "Only available in DSO/Analog/MSO mode.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto r = session->get_spectrum_results();
            if (!r)
                throw ToolError(r.error().message);
            return json_result(spectrum_result_to_json(r.value()));
        });

    // get_lissajous_results
    server->tool_void("get_lissajous_results",
        "Get Lissajous trace configuration. "
        "Only available in DSO/Analog/MSO mode.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            auto r = session->get_lissajous_results();
            if (!r)
                throw ToolError(r.error().message);
            return json_result(lissajous_result_to_json(r.value()));
        });

    // configure_error_state (merged: get + clear)
    server->tool("configure_error_state",
        "Get or clear the session error state. Call with no args or "
        "action='get' to read current errors. Use action='clear' to "
        "clear the error state.")
        .enum_param<std::string>("action", {"get", "clear"},
            "Action: 'get' (default) or 'clear'")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto action = p.get_or<std::string>("action", "get");
            if (action == "clear") {
                check_void(session->clear_error_state());
                return json_result({{"cleared", true}});
            }
            auto r = session->get_error_state();
            if (!r)
                throw ToolError(r.error().message);
            return json_result(error_state_to_json(r.value()));
        });

    // configure_cursors (merged: get + add + remove + clear)
    server->tool("configure_cursors",
        "Manage waveform cursors. Call with no args (or action='get') to "
        "list all cursors. Use action='add' with samplePos to add a cursor. "
        "Use action='remove' with index to remove a cursor by index. "
        "Use action='clear' to remove all cursors.")
        .enum_param<std::string>("action", {"get", "add", "remove", "clear"},
            "Action: 'get' (default), 'add', 'remove', or 'clear'")
        .param<uint64_t>("samplePos", "Sample position for new cursor (action='add')")
        .param<int32_t>("index", "Cursor index to remove (action='remove')")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto action = p.get_or<std::string>("action", "get");

            if (action == "get") {
                auto cursors = session->get_cursors();
                json arr = json::array();
                for (const auto& c : cursors)
                    arr.push_back(cursor_to_json(c));
                return json_result(arr);
            }

            if (action == "add") {
                if (!p.has("samplePos"))
                    throw ToolError("'samplePos' is required for action='add'.");
                auto pos = p.get<uint64_t>("samplePos");
                check_void(session->add_cursor(pos));
                return json_result({{"success", true}});
            }

            if (action == "remove") {
                if (!p.has("index"))
                    throw ToolError("'index' is required for action='remove'.");
                auto idx = p.get<int32_t>("index");
                check_void(session->remove_cursor(idx));
                return json_result({{"success", true}});
            }

            if (action == "clear") {
                check_void(session->clear_cursors());
                return json_result({{"cleared", true}});
            }

            throw ToolError("Invalid action: " + action +
                           ". Use 'get', 'add', 'remove', or 'clear'.");
        });

    return server;
}

} // namespace mcp
