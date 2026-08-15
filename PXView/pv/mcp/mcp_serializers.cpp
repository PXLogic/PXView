// mcp_serializers.cpp — JSON serialization helpers for MCP tool results
//
// Part of the PXView MCP SDK.
//
// Implements all *_to_json() functions that convert pv::api data
// structs to nlohmann::json. Extracted from mcp_tool_registry.cpp
// to keep the registry focused on tool registration logic.
//
// Licensed under GPL v2 or (at your option) any later version.

#include "pv/mcp/mcp_serializers.h"

#include "pv/api/iapp_service.h"

namespace mcp {

// ──────────────────────────────────────────────────────────────────
//  Serialization helpers
// ──────────────────────────────────────────────────────────────────

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
        // String contract matches RpcDispatcher to_json(CaptureStatus):
        // idle/capturing/completed/paused/error. Numeric code exposed
        // separately as state_code for parity.
        {"state",                  capture_state_to_str(s.state)},
        {"state_code",             static_cast<int>(s.state)},
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

// ──────────────────────────────────────────────────────────────────
//  get_devices_json — device list with active flag
// ──────────────────────────────────────────────────────────────────

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

} // namespace mcp
