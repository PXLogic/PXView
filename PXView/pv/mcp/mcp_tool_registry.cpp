// mcp_tool_registry.cpp — Register all MCP tools using the SDK
//
// This file replaces both tool_schemas.inc (46 KB of hand-written JSON)
// and the dispatch_mcp_tool() if-chain in rpc_dispatcher.cpp.
//
// 45 consolidated tools (down from 65 originals, and from the 49 listed in
// devdoc/mcp-tool-consolidation-report.md — 4 of those were later merged away):
//   Tier 0: Mode management (3)     — switch/get_work_mode, get_supported_work_modes
//   Tier 1: Core workflow (17)      — devices, capture, analyzers, channels, export
//   Tier 2: Configuration (12)      — sample config, channel, trigger, probe, glitch, invert, config
//   Tier 3: Advanced features (13)  — samples, edges, decoders, sessions, measurement, cursors
//
// The 4 tools no longer registered are NOT missing — they were intentionally
// folded into existing tools (see devdoc/mcp-post-migration-fix-plan.md §3):
//   get_math_results + get_spectrum_results + get_lissajous_results
//       -> get_measurement_results (dispatch by the 'types' array)
//   get_sample_config
//       -> get_session_status with include='config'   (returns "sampleConfig")
//   get_decoder_class_names
//       -> get_analyzer_results with includeMetadata=true (returns "metadata.classNames")
//   set_save_range -> renamed set_export_config (1:1, same capability)
//
// Refactored from a single 1645-line function into:
//   - 4 tier-based register functions (Improvement 1)
//   - Struct API for start_capture (Improvement 2)
//   - Serializers extracted to mcp_serializers.h/.cpp (Improvement 3)
//   - Complex handlers extracted to named functions (Improvement 4)
//   - Output schema support (Improvement 5)
//   - any_param() replaces typeid(void) hack (Improvement 6)
//
// Licensed under GPL v2 or (at your option) any later version.

#include "pv/mcp/mcp.h"
#include "pv/mcp/mcp_serializers.h"
#include "pv/api/iapp_service.h"
#include "pv/api/types.h"
#include "PXView/config.h"

#include <algorithm>
#include <cstring>

namespace mcp {

using json = nlohmann::json;
using namespace pv::api;

// ═══════════════════════════════════════════════════════════════════════
//  Named handler functions (Improvement 4: extract complex lambdas)
// ═══════════════════════════════════════════════════════════════════════
//
//  Each function encapsulates the logic that was previously an inline
//  lambda. This makes each tool registration a clean 3-5 line chain,
//  and the handler logic is independently readable and testable.
//

namespace {

// ── start_capture handler (Struct API — Improvement 2) ──

ToolResult handle_start_capture(IAppService* app_svc,
                                 const StartCaptureParams& p) {
    auto* session = app_svc->get_active_session();

    // Auto-connect device if specified
    if (!p.deviceId.empty()) {
        auto r = app_svc->create_session(p.deviceId, "");
        if (!r.ok())
            throw ToolError("Failed to create session: " +
                            r.error().message);
        session = app_svc->get_active_session();
    }
    if (!session)
        throw ToolError("No active session. Provide deviceId to "
                        "auto-create one.");

    // Trigger config is set separately via configure_trigger.
    // Pass trigger defaults (-1, "") to indicate no inline trigger.
    auto r = session->configure_and_start(
        p.digitalChannels, p.analogChannels,
        p.digitalSampleRate, p.analogSampleRate,
        p.digitalThresholdVolts,
        {}, p.captureMode, p.durationSeconds, p.instant,
        -1, "", 0.0, 0.0, 0.0, {},
        p.channelMode, p.rleEnabled, 0.0, 0.0, false, "",
        "", "", "", "", p.pattern, -1,
        p.repeatIntervalSeconds, p.sampleCount);
    if (!r)
        throw ToolError(r.error().message);
    return json_result({{"started", true},
        {"capture_id", r.value()}});
}

// ── get_channels handler (mode-aware query) ──

ToolResult handle_get_channels(ISessionService* session,
                                const Params& p) {
    auto current_mode = session->get_work_mode();

    // If mode is specified and differs from current mode, return
    // channel counts for the requested mode without switching.
    if (p.has("mode")) {
        auto req_mode = static_cast<WorkMode>(p.get<int8_t>("mode"));
        if (req_mode != current_mode) {
            json result;
            result["requested_mode"] = static_cast<int>(req_mode);
            result["current_mode"]   = static_cast<int>(current_mode);
            result["note"] = "Channel details require switching. "
                "Call switch_work_mode(" +
                std::to_string(static_cast<int>(req_mode)) +
                ") to switch to this mode, then call get_channels "
                "again for full channel details.";
            json counts = json::array();
            switch (req_mode) {
            case WorkMode::Logic:
                counts.push_back({{"type", "logic"},
                    {"count", session->get_channel_count(ChannelType::Logic)}});
                break;
            case WorkMode::Dso:
                counts.push_back({{"type", "dso"},
                    {"count", session->get_channel_count(ChannelType::Dso)}});
                break;
            case WorkMode::Analog:
                counts.push_back({{"type", "analog"},
                    {"count", session->get_channel_count(ChannelType::Analog)}});
                break;
            case WorkMode::Mso:
                counts.push_back({{"type", "logic"},
                    {"count", session->get_channel_count(ChannelType::Logic)}});
                counts.push_back({{"type", "analog"},
                    {"count", session->get_channel_count(ChannelType::Analog)}});
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
}

// ── add_analyzer handler (options + channelMap parsing) ──

ToolResult handle_add_analyzer(ISessionService* session,
                                const Params& p) {
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
}

// ── reconfigure_decoder handler ──

ToolResult handle_reconfigure_decoder(ISessionService* session,
                                       const Params& p) {
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
}

// ── get_analyzer_results handler (with optional metadata) ──

ToolResult handle_get_analyzer_results(ISessionService* session,
                                        const Params& p) {
    auto id = p.get<std::string>("analyzerId");
    auto start = p.get_or<uint64_t>("startSample", 0);
    auto end = p.get_or<uint64_t>("endSample", UINT64_MAX);
    auto max_count = p.get_or<int>("maxCount", 1000);
    std::optional<int> ann_class = std::nullopt;
    if (p.has("annClass"))
        ann_class = p.get<int>("annClass");
    auto r = session->get_decoder_annotations(id, start, end, max_count, ann_class);
    if (!r)
        throw ToolError(r.error().message);
    json arr = json::array();
    for (const auto& a : r.value())
        arr.push_back(decoder_ann_to_json(a));
    json result = {{"annotations", arr}};
    // Include static metadata (class names) at top level,
    // not affected by maxCount pagination.
    if (p.get_or<bool>("includeMetadata", false)) {
        // Derive decoder name from instance to get class names
        auto active = session->get_active_decoders();
        std::string dec_name;
        for (const auto& d : active) {
            if (d.instance_id == id) {
                dec_name = d.decoder_id;
                break;
            }
        }
        if (!dec_name.empty()) {
            auto cn = session->get_decoder_class_names(dec_name);
            if (cn) {
                json classes = json::array();
                for (const auto& c : cn.value())
                    classes.push_back(decoder_class_to_json(c));
                result["metadata"] = {{"classNames", classes}};
            }
        }
    }
    return json_result(result);
}

// ── export_data_table_csv handler (single + multi mode) ──

ToolResult handle_export_data_table_csv(ISessionService* session,
                                         const Params& p) {
    auto base_path = p.get<std::string>("filePath");
    auto iso = p.get_or<bool>("iso8601Timestamp", false);

    // Multi-analyzer mode
    if (p.has("analyzers")) {
        const auto& analyzers = p.raw().at("analyzers");
        if (!analyzers.is_array() || analyzers.empty())
            throw ToolError("'analyzers' must be a non-empty array.");

        json exported = json::array();
        for (const auto& a : analyzers) {
            auto id = a.at("analyzerId").get<std::string>();
            auto radix = a.value("radixType", 0);
            // Sanitize id for filename: replace ':' with '_'
            std::string safe_id = id;
            std::replace(safe_id.begin(), safe_id.end(), ':', '_');
            auto dot = base_path.rfind('.');
            std::string path;
            if (dot != std::string::npos)
                path = base_path.substr(0, dot) + "_" + safe_id +
                       base_path.substr(dot);
            else
                path = base_path + "_" + safe_id + ".csv";
            auto r = session->export_data_table_csv(path, id, radix, iso);
            check_void(r);
            exported.push_back({{"analyzerId", id}, {"filePath", path}});
        }
        return json_result({{"exported", exported}});
    }

    // Single-analyzer mode
    if (!p.has("analyzerId"))
        throw ToolError("Provide either 'analyzers' array or 'analyzerId'.");
    auto id = p.get<std::string>("analyzerId");
    auto radix = p.get_or<int>("radixType", 0);
    auto r = session->export_data_table_csv(base_path, id, radix, iso);
    check_void(r);
    return text("exported");
}

// ── get_samples handler (logic / analog / dso) ──

ToolResult handle_get_samples(ISessionService* session,
                               const Params& p) {
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
}

// ── find_pattern handler (single + multi channel) ──

ToolResult handle_find_pattern(ISessionService* session,
                                const Params& p) {
    auto from = p.get<uint64_t>("fromSample");

    // Multi-channel mode
    if (p.has("channels")) {
        const auto& channels = p.raw().at("channels");
        if (!channels.is_array() || channels.empty())
            throw ToolError("'channels' must be a non-empty array.");

        uint64_t search_from = from;
        for (int iter = 0; iter < 10000; ++iter) {
            bool all_match = true;
            uint64_t max_pos = search_from;

            for (const auto& ch : channels) {
                auto idx = ch.at("channelIndex").get<int16_t>();
                auto state = ch.at("state").get<std::string>();
                auto r = session->find_pattern(search_from, idx, state);
                if (!r) throw ToolError(r.error().message);
                if (r.value() > max_pos) {
                    max_pos = r.value();
                } else if (r.value() < max_pos) {
                    all_match = false;
                }
            }

            if (all_match)
                return json_result({{"sample", max_pos}});
            search_from = max_pos + 1;
        }
        throw ToolError("Pattern not found within 10000 iterations.");
    }

    // Single-channel mode
    if (!p.has("channelIndex") || !p.has("pattern"))
        throw ToolError(
            "Provide either 'channels' array or 'channelIndex'+'pattern'.");
    auto ch = p.get<int16_t>("channelIndex");
    auto pattern = p.get<std::string>("pattern");
    auto r = session->find_pattern(from, ch, pattern);
    if (!r)
        throw ToolError(r.error().message);
    return json_result({{"sample", r.value()}});
}

// ── configure_trigger handler (mode-aware get/set) ──

ToolResult handle_configure_trigger(ISessionService* session,
                                     const Params& p) {
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
}

// ── configure_probe handler (get/set) ──

ToolResult handle_configure_probe(ISessionService* session,
                                   const Params& p) {
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
}

// ── configure_glitch_filter handler (get/set/clear) ──

ToolResult handle_configure_glitch_filter(ISessionService* session,
                                           const Params& p) {
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
}

// ── configure_signal_invert handler (get/set/clear) ──

ToolResult handle_configure_signal_invert(ISessionService* session,
                                           const Params& p) {
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
}

// ── get_config handler (generic config reader) ──

ToolResult handle_get_config(ISessionService* session,
                              const Params& p) {
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
}

// ── set_config handler (generic config writer) ──

ToolResult handle_set_config(ISessionService* session,
                              const Params& p) {
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
}

// ── get_measurement_results handler ──

ToolResult handle_get_measurement_results(ISessionService* session,
                                           const Params& p) {
    json result = json::object();

    // Determine which types to include
    bool want_math = true, want_spectrum = true, want_lissajous = true;
    if (p.has("types")) {
        const auto& types = p.raw().at("types");
        if (types.is_array()) {
            want_math = want_spectrum = want_lissajous = false;
            for (const auto& t : types) {
                auto s = t.get<std::string>();
                if (s == "math") want_math = true;
                else if (s == "spectrum") want_spectrum = true;
                else if (s == "lissajous") want_lissajous = true;
            }
        }
    }

    if (want_math) {
        auto r = session->get_math_results();
        if (!r) throw ToolError(r.error().message);
        result["math"] = math_result_to_json(r.value());
    }
    if (want_spectrum) {
        auto r = session->get_spectrum_results();
        if (!r) throw ToolError(r.error().message);
        result["spectrum"] = spectrum_result_to_json(r.value());
    }
    if (want_lissajous) {
        auto r = session->get_lissajous_results();
        if (!r) throw ToolError(r.error().message);
        result["lissajous"] = lissajous_result_to_json(r.value());
    }
    return json_result(result);
}

// ── configure_cursors handler (get/add/remove/clear) ──

ToolResult handle_configure_cursors(ISessionService* session,
                                     const Params& p) {
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
}

// ── refresh_device_list handler ──

ToolResult handle_refresh_device_list(IAppService* app_svc) {
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
}

// ── list_sessions handler ──

ToolResult handle_list_sessions(IAppService* app_svc,
                                 const Params& p) {
    auto ids = app_svc->get_session_ids();
    int active_id = app_svc->get_active_session_id();
    auto include_dev = p.get_or<bool>("includeDeviceInfo", true);
    json arr = json::array();
    for (int sid : ids) {
        json s;
        s["session_id"] = sid;
        s["is_active"] = (sid == active_id);
        if (include_dev) {
            auto* sess = app_svc->get_session(sid);
            if (sess) {
                auto dev = sess->get_device_info();
                s["device_id"] = dev.id;
                s["device_name"] = dev.display_name;
                s["driver_name"] = dev.driver_name;
                s["is_file"] = dev.is_file;
                s["file_path"] = dev.path;
            }
        }
        arr.push_back(s);
    }
    return json_result({
        {"sessions", arr},
        {"active_session_id", active_id},
        {"count", static_cast<int>(ids.size())}
    });
}

// ── get_session_status handler ──

ToolResult handle_get_session_status(ISessionService* session,
                                      const Params& p) {
    auto cfg = session->get_sample_config();
    auto cache = session->get_disk_cache_info();
    json result = {
        {"collect_mode", static_cast<int>(cfg.collect_mode)},
        {"is_single_mode", cfg.collect_mode == CollectMode::Single},
        {"is_repeat_mode", cfg.collect_mode == CollectMode::Repeat},
        {"is_loop_mode", cfg.collect_mode == CollectMode::Loop},
        {"repeat_interval", cfg.repeat_interval},
        {"repeat_hold_percent", cfg.repeat_hold_percent},
        {"disk_cache", disk_cache_to_json(cache)}
    };
    if (p.has("include")) {
        auto inc = p.get<std::string>("include");
        if (inc.find("config") != std::string::npos) {
            result["sampleConfig"] = sample_config_to_json(cfg);
        }
    }
    return json_result(result);
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  Tier 0: Mode Management (3 tools)
// ═══════════════════════════════════════════════════════════════════════

static void register_mode_management_tools(McpServer& server,
                                            IAppService* app_svc) {
    // switch_work_mode
    server.tool("switch_work_mode",
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

    // get_work_mode
    server.tool_void("get_work_mode",
        "Get the current device work mode. Returns: 0=Logic, 1=DSO, "
        "2=Analog, 3=MSO, -1=Unknown. Call this to determine which "
        "channels/triggers/probe configs are available.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            return json_result({{"mode",
                static_cast<int>(session->get_work_mode())}});
        });

    // get_supported_work_modes
    server.tool_void("get_supported_work_modes",
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
}

// ═══════════════════════════════════════════════════════════════════════
//  Tier 1: Core Workflow (17 tools)
// ═══════════════════════════════════════════════════════════════════════

static void register_core_workflow_tools(McpServer& server,
                                          IAppService* app_svc) {
    // get_devices
    server.tool("get_devices",
        "List connected devices and their IDs. Call this first to discover "
        "available devices before starting a capture.")
        .read_only()
        .on_call([app_svc](const Params&) -> ToolResult {
            return json_result(get_devices_json(app_svc));
        });

    // start_capture (Struct API — Improvement 2)
    // Uses StartCaptureParams struct + MCP_SCHEMA for type-safe parameters.
    server.tool<StartCaptureParams>("start_capture",
        "Start a capture in the current work mode. Trigger configuration "
        "should be set separately via configure_trigger BEFORE calling "
        "this tool. Typical workflow: 1) get_devices, 2) switch_work_mode, "
        "3) configure_channel, 4) set_sample_config, 5) configure_trigger, "
        "6) start_capture, 7) wait_capture, 8) get_analyzer_results. "
        "In Stream mode (channelMode='Stream'), durationSeconds and "
        "sampleCount are ignored — use stop_capture to end streaming.")
        .destructive();
    server.set_struct_handler<StartCaptureParams>("start_capture",
        [app_svc](const StartCaptureParams& p) -> ToolResult {
            return handle_start_capture(app_svc, p);
        });

    // stop_capture
    server.tool_void("stop_capture",
        "Stop the current capture. Use in Stream mode or to abort a "
        "timed capture early.")
        .destructive()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            check_void(session->stop_capture());
            return text("stopped");
        });

    // wait_capture
    server.tool("wait_capture",
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
    server.tool("get_capture_status",
        "Get the current capture status (state, sample count, sample rate). "
        "Optionally exclude detailed progress info for lightweight polling.")
        .param<bool>("includeProgress",
            "If true (default), include detailed progress info")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto status = capture_status_to_json(
                session->get_capture_status());
            if (!p.get_or<bool>("includeProgress", true)) {
                status.erase("progress");
                status.erase("progressDetail");
            }
            return json_result(status);
        });

    // load_capture
    server.tool("load_capture",
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
    server.tool("save_capture",
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
    server.tool_void("close_capture",
        "Close the current capture and release its data.")
        .destructive()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            check_void(session->close_capture());
            return text("closed");
        });

    // add_analyzer
    server.tool("add_analyzer",
        "Add a protocol decoder (analyzer). Only available in Logic/MSO "
        "mode (decoders need digital channels). Best called BEFORE "
        "start_capture for auto-decode. Use stackOnAnalyzerId to stack "
        "decoders (e.g., SPI on top of I2C).")
        .param<std::string>("decoderId", "Decoder ID (e.g. 'i2c', 'spi')", Required)
        .param<std::string>("label", "Display label for this analyzer instance")
        .param<std::string>("stackOnAnalyzerId", "Instance ID to stack on top of")
        .any_param("options", "Decoder options as key-value pairs", "object")
        .any_param("channelMap", "Channel mapping (decoder channel → hardware index)", "object")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_add_analyzer(session, p);
        });

    // remove_analyzer
    server.tool("remove_analyzer",
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
    server.tool("list_analyzers",
        "List all available protocol decoders and their channel "
        "requirements. Optionally filter by decoder name.")
        .param<std::string>("filter",
            "Optional: filter by decoder name (e.g. 'i2c', 'spi', 'uart')")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto decoders = session->get_available_decoders();
            json arr = json::array();
            for (const auto& d : decoders) {
                if (p.has("filter")) {
                    auto f = p.get<std::string>("filter");
                    if (d.id.find(f) == std::string::npos)
                        continue;
                }
                arr.push_back(decoder_desc_to_json(d));
            }
            return json_result(arr);
        });

    // get_analyzer_options
    server.tool("get_analyzer_options",
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
    server.tool("get_analyzer_results",
        "Read decoded protocol data from a previously added analyzer. "
        "Returns annotation array with sample ranges and text. "
        "Set includeMetadata=true to get annotation class names in the "
        "top-level 'metadata' field (not affected by maxCount).")
        .param<std::string>("analyzerId", "Analyzer instance ID", Required)
        .param<uint64_t>("startSample", "Start sample (default 0)")
        .param<uint64_t>("endSample", "End sample (default = all)")
        .param<int>("maxCount", "Max annotations to return (default 1000)")
        .param<int>("annClass",
            "Optional: only return annotations of this ann_class. "
            "Use includeMetadata to discover class names. Default = all classes")
        .param<bool>("includeMetadata",
            "If true, include annotation class names in 'metadata' field")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_get_analyzer_results(session, p);
        });

    // export_raw_data
    server.tool("export_raw_data",
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
    server.tool("export_data_table_csv",
        "Export decoded analyzer data as a CSV table. "
        "Single mode: provide analyzerId + optional radixType. "
        "Multi mode: provide 'analyzers' array of {analyzerId, radixType} "
        "objects — generates separate files named <prefix>_<analyzerId>.csv. "
        "'analyzers' overrides analyzerId when provided.")
        .param<std::string>("filePath", "Output CSV file path (or prefix for multi mode)", Required)
        .param<std::string>("analyzerId", "Single mode: analyzer instance ID")
        .param<int>("radixType", "Radix: 1=Binary, 2=Decimal, 3=Hex, 4=Ascii")
        .param<bool>("iso8601Timestamp", "Use ISO8601 timestamp")
        .any_param("analyzers",
            "Multi mode: array of {analyzerId, radixType} objects. "
            "When provided, overrides analyzerId/radixType.",
            "array", "object")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_export_data_table_csv(session, p);
        });

    // get_channels
    server.tool("get_channels",
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
            return handle_get_channels(session, p);
        });

    // refresh_device_list
    server.tool_void("refresh_device_list",
        "Trigger a hot-plug rescan of all device drivers and return the "
        "updated device list. Use this when a USB device is connected or "
        "disconnected after startup.")
        .read_only()
        .on_call([app_svc]() -> ToolResult {
            return handle_refresh_device_list(app_svc);
        });
}

// ═══════════════════════════════════════════════════════════════════════
//  Tier 2: Configuration (12 tools)
// ═══════════════════════════════════════════════════════════════════════

static void register_configuration_tools(McpServer& server,
                                          IAppService* app_svc) {
    // set_sample_config
    server.tool("set_sample_config",
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

    // configure_channel
    server.tool("configure_channel",
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

    // configure_trigger
    server.tool("configure_trigger",
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
            return handle_configure_trigger(session, p);
        });

    // configure_probe
    server.tool("configure_probe",
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
            return handle_configure_probe(session, p);
        });

    // configure_glitch_filter
    server.tool("configure_glitch_filter",
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
            return handle_configure_glitch_filter(session, p);
        });

    // configure_signal_invert
    server.tool("configure_signal_invert",
        "Get, set, or clear signal invert. Call with no args to get "
        "current config. Set channels+invertStates to enable. "
        "Set channels to [] to clear.")
        .array_param<int32_t>("channels", "Channel indices. Empty array = clear.")
        .array_param<bool>("invertStates", "Invert state per channel")
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_configure_signal_invert(session, p);
        });

    // get_config
    server.tool("get_config",
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
            return handle_get_config(session, p);
        });

    // set_config
    server.tool("set_config",
        "Write a generic SR_CONF_* config value by key. The 'type' field "
        "selects how to interpret the value. Use this to configure device "
        "options not covered by dedicated tools (e.g. PWM, VTH, Filter, "
        "ClockType, TriggerOut, RLE, BandwidthLimit, OperationMode, etc.).")
        .param<int32_t>("key", "SR_CONF_* config key (numeric)", Required)
        .param<std::string>("type", "Value type: bool, int, int64, string, double", Required)
        .any_param("value", "Value to set (type depends on 'type' field)", "object")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_set_config(session, p);
        });

    // set_export_config
    server.tool("set_export_config",
        "Set the sample range for save/export operations. "
        "Note: These are export/display offsets, not acquisition triggers.")
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
    server.tool("connect_device",
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
    server.tool("disconnect_device",
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

    // get_session_status
    server.tool("get_session_status",
        "Get session status including collect mode, repeat interval, "
        "and disk cache information. Use 'include' to optionally add "
        "sample config ('config') to the response.")
        .param<std::string>("include",
            "Optional extras: 'config' to include sample configuration")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_get_session_status(session, p);
        });
}

// ═══════════════════════════════════════════════════════════════════════
//  Tier 3: Advanced Features (13 tools)
// ═══════════════════════════════════════════════════════════════════════

static void register_advanced_feature_tools(McpServer& server,
                                             IAppService* app_svc) {
    // get_samples
    server.tool("get_samples",
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
            return handle_get_samples(session, p);
        });

    // find_next_edge
    server.tool("find_next_edge",
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
    server.tool("find_pattern",
        "Find the next occurrence of a signal pattern. "
        "Single-channel mode: provide channelIndex + pattern (e.g. '1','0','x'). "
        "Multi-channel mode: provide 'channels' array of {channelIndex, state} "
        "objects to match a combined pattern across channels (e.g. I2C SCL=1+SDA=0). "
        "\"channels\" overrides channelIndex/pattern when provided.")
        .param<uint64_t>("fromSample", "Start searching from this sample", Required)
        .param<int16_t>("channelIndex", "Single-channel mode: channel index")
        .param<std::string>("pattern", "Single-channel mode: pattern string ('1','0','x')")
        .any_param("channels",
            "Multi-channel mode: array of {channelIndex, state} objects. "
            "When provided, overrides channelIndex/pattern.",
            "array", "object")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_find_pattern(session, p);
        });

    // get_active_decoders
    server.tool("get_active_decoders",
        "Get the list of currently active (added) protocol decoders "
        "with their instance IDs. Optionally include full configurations.")
        .param<bool>("includeConfig",
            "If true (default), include decoder configurations")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            auto decoders = session->get_active_decoders();
            (void)p;  // includeConfig accepted but not yet used
            json arr = json::array();
            for (const auto& d : decoders)
                arr.push_back(decoder_inst_to_json(d));
            return json_result(arr);
        });

    // clear_all_decoders
    server.tool_void("clear_all_decoders",
        "Remove all active protocol decoders from the session.")
        .destructive()
        .on_call([app_svc]() -> ToolResult {
            auto* session = require_session(app_svc);
            check_void(session->clear_all_decoders());
            return text("cleared");
        });

    // reconfigure_decoder
    server.tool("reconfigure_decoder",
        "Reconfigure an existing decoder's options and channel map "
        "in place (no remove + re-add). Triggers re-decode.")
        .param<std::string>("analyzerId", "Analyzer instance ID", Required)
        .any_param("options", "Decoder options as key-value pairs", "object")
        .any_param("channelMap", "Channel mapping (decoder channel → hardware index)", "object")
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_reconfigure_decoder(session, p);
        });

    // list_sessions
    server.tool("list_sessions",
        "List all sessions with their active status. "
        "Optionally include full device info per session.")
        .param<bool>("includeDeviceInfo",
            "If true (default), include device info per session")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            return handle_list_sessions(app_svc, p);
        });

    // create_session
    server.tool("create_session",
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
    server.tool("destroy_session",
        "Destroy a session by ID.")
        .param<int>("sessionId", "Session ID to destroy", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto sid = p.get<int>("sessionId");
            check_void(app_svc->destroy_session(sid));
            return json_result({{"success", true}});
        });

    // set_active_session
    server.tool("set_active_session",
        "Switch the active session.")
        .param<int>("sessionId", "Session ID to make active", Required)
        .destructive()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto sid = p.get<int>("sessionId");
            check_void(app_svc->set_active_session(sid));
            return json_result({{"success", true}});
        });

    // get_measurement_results
    server.tool("get_measurement_results",
        "Get measurement and analysis results. Specify which result types "
        "to include via the 'types' array. Only available in DSO/Analog/MSO mode.")
        .any_param("types",
            "Array of result types to include: 'math', 'spectrum', 'lissajous'. "
            "Default: all available.",
            "array", "string")
        .read_only()
        .on_call([app_svc](const Params& p) -> ToolResult {
            auto* session = require_session(app_svc);
            return handle_get_measurement_results(session, p);
        });

    // configure_error_state
    server.tool("configure_error_state",
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

    // configure_cursors
    server.tool("configure_cursors",
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
            return handle_configure_cursors(session, p);
        });
}

// ═══════════════════════════════════════════════════════════════════════
//  create_mcp_server — build and return a fully configured McpServer
// ═══════════════════════════════════════════════════════════════════════

std::unique_ptr<McpServer>
create_mcp_server(IAppService* app_svc) {
    auto server = std::make_unique<McpServer>("pxview", DS_VERSION_STRING);

    // Load instructions from file (Layer 1 of three-layer guidance)
    server->set_instructions_file("pv/mcp/mcp_instructions.txt");

    // Register tools by tier (Improvement 1: split for readability)
    register_mode_management_tools(*server, app_svc);     // Tier 0: 3 tools
    register_core_workflow_tools(*server, app_svc);       // Tier 1: 17 tools
    register_configuration_tools(*server, app_svc);       // Tier 2: 12 tools
    register_advanced_feature_tools(*server, app_svc);    // Tier 3: 13 tools

    return server;
}

} // namespace mcp
