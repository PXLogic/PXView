// mcp_serializers.h — JSON serialization helpers for MCP tool results
//
// Part of the PXView MCP SDK.
//
// Provides:
//   - *_to_json() functions for all pv::api data structs
//   - get_devices_json() helper for device list with active flag
//   - StartCaptureParams struct + schema (Struct API demonstration)
//
// This file centralizes ~200 lines of serialization code that was
// previously inlined in mcp_tool_registry.cpp, keeping the registry
// focused on tool registration logic.
//
// Licensed under GPL v2 or (at your option) any later version.

#pragma once

#include "pv/mcp/mcp.h"           // McpServer, ToolDesc, json, etc.
#include "pv/api/types.h"
#include "pv/api/iapp_service.h"  // IAppService (includes isession_service.h)

#include <string>

namespace mcp {

using json = nlohmann::json;
using namespace pv::api;

// ──────────────────────────────────────────────────────────────────
//  Serialization helpers — convert pv::api structs to JSON
// ──────────────────────────────────────────────────────────────────

json device_to_json(const DeviceInfo& d);
json channel_to_json(const ChannelInfo& c);
json sample_config_to_json(const SampleConfig& s);
json capture_status_to_json(const CaptureStatus& s);
json glitch_config_to_json(const GlitchFilterConfig& c);
json signal_invert_to_json(const SignalInvertConfig& c);
json logic_trigger_to_json(const LogicTriggerConfig& c);
json dso_trigger_to_json(const DsoTriggerConfig& c);
json probe_to_json(const ProbeConfig& p);
json disk_cache_to_json(const DiskCacheInfo& d);
json decoder_desc_to_json(const DecoderDescriptor& d);
json decoder_inst_to_json(const DecoderInstance& d);
json decoder_ann_to_json(const DecoderAnnotation& a);
json error_state_to_json(const ErrorState& e);
json math_result_to_json(const MathResult& m);
json spectrum_result_to_json(const SpectrumResult& s);
json lissajous_result_to_json(const LissajousResult& l);
json cursor_to_json(const CursorInfo& c);
json decoder_class_to_json(const DecoderClassInfo& d);

// Helper: get devices list with active flag
json get_devices_json(IAppService* app_svc);

// ──────────────────────────────────────────────────────────────────
//  Struct API parameter types
// ──────────────────────────────────────────────────────────────────
//
// These structs replace the Builder API's string-based parameter
// chains for tools with many parameters. They provide:
//   - Compile-time field name safety (member-pointer in MCP_SCHEMA)
//   - Automatic JSON deserialization via nlohmann macros
//   - Reusable across multiple tools if needed
//

// StartCaptureParams — 12 parameters, the most complex tool.
// Replaces 12 .param<T>() calls + 12 p.get_or<T>() calls.
struct StartCaptureParams {
    std::string              deviceId;
    std::vector<int16_t>     digitalChannels;
    std::vector<int16_t>     analogChannels;
    uint64_t                 digitalSampleRate = 0;
    uint64_t                 analogSampleRate  = 0;
    double                   digitalThresholdVolts = 0.0;
    std::string              captureMode   = "manual";
    double                   durationSeconds   = 0.0;
    std::string              channelMode;
    bool                     rleEnabled    = false;
    uint64_t                 sampleCount   = 0;
    double                   repeatIntervalSeconds = 0.0;
    bool                     instant       = false;
    std::string              pattern;          // Demo pattern mode
};

// ──────────────────────────────────────────────────────────────────
//  Schema specializations (Struct API)
// ──────────────────────────────────────────────────────────────────

MCP_SCHEMA(StartCaptureParams)
    .opt("deviceId", &StartCaptureParams::deviceId,
         "Device ID (optional, uses active device if omitted)")
    .opt_arr("digitalChannels", &StartCaptureParams::digitalChannels,
             "Digital channel indices (Logic/MSO mode)")
    .opt_arr("analogChannels", &StartCaptureParams::analogChannels,
             "Analog channel indices (Analog/MSO mode)")
    .opt("digitalSampleRate", &StartCaptureParams::digitalSampleRate,
         "Digital sample rate in Hz")
    .opt("analogSampleRate", &StartCaptureParams::analogSampleRate,
         "Analog sample rate in Hz")
    .opt("digitalThresholdVolts", &StartCaptureParams::digitalThresholdVolts,
         "Digital threshold voltage (e.g. 1.8)")
    .opt("captureMode", &StartCaptureParams::captureMode,
         "Capture mode: 'timed', 'manual', 'stream'")
    .opt("durationSeconds", &StartCaptureParams::durationSeconds,
         "Duration in seconds (timed mode, ignored in Stream mode)")
    .opt("channelMode", &StartCaptureParams::channelMode,
         "Buffer/Stream mode")
    .opt("rleEnabled", &StartCaptureParams::rleEnabled,
         "Enable RLE compression (Logic mode)")
    .opt("sampleCount", &StartCaptureParams::sampleCount,
         "Sample count limit (ignored in Stream mode)")
    .opt("repeatIntervalSeconds", &StartCaptureParams::repeatIntervalSeconds,
         "Repeat interval in seconds")
    .opt("instant", &StartCaptureParams::instant,
         "Start instantly without buffering")
    .opt("pattern", &StartCaptureParams::pattern,
         "Demo pattern mode (e.g. 'random', 'graycode', 'i2c')")
MCP_SCHEMA_END

// JSON deserialization for StartCaptureParams (required by Struct API).
// Must be inside namespace mcp so ADL (Argument Dependent Lookup)
// finds from_json/to_json when the type is in namespace mcp.
// NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT generates from_json()
// that reads each field from the JSON object, using the struct's
// default values for missing fields.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(StartCaptureParams,
    deviceId, digitalChannels, analogChannels,
    digitalSampleRate, analogSampleRate, digitalThresholdVolts,
    captureMode, durationSeconds, channelMode,
    rleEnabled, sampleCount, repeatIntervalSeconds, instant, pattern)

} // namespace mcp
