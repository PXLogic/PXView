#include "rpc_dispatcher.h"
#include <QFile>
#include <QDir>
#include <QCoreApplication>

namespace pv::api {

// Debug log helper - writes to temp file since GUI app stderr is unreliable
static void mcp_dbg_log(const char* msg) {
    static QFile dbg_file;
    if (!dbg_file.isOpen()) {
        dbg_file.setFileName(QDir::tempPath() + "/pxview_mcp_debug.log");
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
    return json{
        {"state",                   static_cast<int>(s.state)},
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

// ---- Constructor ----

RpcDispatcher::RpcDispatcher(IAppService* app_svc)
    : app_svc_(app_svc) {}

// ---- MCP Tool Schemas ----

json RpcDispatcher::get_tool_schemas() {
    return json::array({
        // 1. get_devices
        {
            {"name", "get_devices"},
            {"description", "List connected devices. Call this first to discover available devices and their IDs before starting a capture."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"includeSimulationDevices", {
                        {"type", "boolean"},
                        {"description", "Whether to include simulation/demo devices"}
                    }}
                }}
            }}
        },
        // 2. start_capture
        {
            {"name", "start_capture"},
            {"description", "Start a new capture. Typical workflow: 1) get_devices to find device ID, 2) add_analyzer to add decoders (recommended BEFORE capture so auto-decode works), 3) start_capture with device/channel config, 4) wait_capture to wait for completion, 5) get_analyzer_results to read decoded data."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"deviceId", {
                        {"type", "string"},
                        {"description", "Device ID to use for capture"}
                    }},
                    {"logicDeviceConfiguration", {
                        {"type", "object"},
                        {"description", "Logic device configuration"},
                        {"properties", {
                            {"digitalChannels", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Digital channel indices to enable"}}},
                            {"analogChannels", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Analog channel indices to enable"}}},
                            {"digitalSampleRate", {{"type", "integer"}, {"description", "Digital sample rate in Hz"}}},
                            {"analogSampleRate", {{"type", "integer"}, {"description", "Analog sample rate in Hz"}}},
                            {"digitalThresholdVolts", {{"type", "number"}, {"description", "Digital threshold voltage"}}},
                            {"glitchFilters", {
                                {"type", "array"},
                                {"description", "Glitch filter configurations"},
                                {"items", {
                                    {"type", "object"},
                                    {"properties", {
                                        {"channelIndex", {{"type", "integer"}, {"description", "Digital channel index to apply filter"}}},
                                        {"threshold", {{"type", "number"}, {"description", "Minimum pulse width in samples to filter out"}}}
                                    }}
                                }}
                            }},
                            {"channelMode", {{"type", "string"}, {"description", "Channel mode (e.g. Buffer, Stream). Device-specific."}}},
                            {"rleEnabled", {{"type", "boolean"}, {"description", "Enable RLE (Run-Length Encoding) compression"}}},
                            {"streamBufferSizeGB", {{"type", "number"}, {"description", "Disk stream buffer size in GB (1-1024). Used when diskCacheEnabled=true."}}},
                            {"streamMemBufferSizeGB", {{"type", "number"}, {"description", "Memory stream buffer size in GB (1-64). Used when diskCacheEnabled=false."}}},
                            {"diskCacheEnabled", {{"type", "boolean"}, {"description", "Enable disk cache for long captures"}}},
                            {"diskCachePath", {{"type", "string"}, {"description", "Custom disk cache directory path"}}},
                            {"thresholdPreset", {{"type", "string"}, {"description", "Threshold preset name (e.g. 1.8V, 3.3V, 5V, Adjustable). Some devices only support presets, not custom voltage."}}},
                            {"operationMode", {{"type", "string"}, {"description", "Device operation mode (e.g. Buffer, Stream, Internal test). Device-specific."}}},
                            {"bufferOptions", {{"type", "string"}, {"description", "Buffer configuration options. Device-specific."}}},
                            {"digitalFilter", {{"type", "string"}, {"description", "Digital filter mode. Device-specific."}}}
                        }}
                    }},
                    {"captureConfiguration", {
                        {"type", "object"},
                        {"description", "Capture mode configuration"},
                        {"properties", {
                            {"timedCaptureMode", {
                                {"type", "object"},
                                {"properties", {
                                    {"durationSeconds", {{"type", "number"}, {"description", "Capture duration in seconds"}}}
                                }}
                            }},
                            {"manualCaptureMode", {
                                {"type", "object"},
                                {"properties", {
                                    {"sampleCount", {{"type", "integer"}, {"description", "Number of samples to capture"}}}
                                }}
                            }},
                            {"digitalCaptureMode", {
                                {"type", "object"},
                                {"description", "Digital trigger capture mode"},
                                {"properties", {
                                    {"triggerChannelIndex", {{"type", "integer"}, {"description", "Digital channel index for trigger (must be enabled)"}}},
                                    {"triggerType", {{"type", "string"}, {"description", "Trigger type: rising, falling, pulse_high, pulse_low"}, {"enum", json::array({"rising", "falling", "pulse_high", "pulse_low"})}}},
                                    {"afterTriggerSeconds", {{"type", "number"}, {"description", "Post-trigger buffer duration in seconds"}}},
                                    {"minPulseWidthSeconds", {{"type", "number"}, {"description", "Minimum pulse width for pulse trigger in seconds"}}},
                                    {"maxPulseWidthSeconds", {{"type", "number"}, {"description", "Maximum pulse width for pulse trigger in seconds"}}},
                                    {"linkedChannels", {
                                        {"type", "array"},
                                        {"description", "Additional channels with required state for trigger"},
                                        {"items", {
                                            {"type", "object"},
                                            {"properties", {
                                                {"channelIndex", {{"type", "integer"}, {"description", "Digital channel index"}}},
                                                {"state", {{"type", "string"}, {"description", "Required state: high or low"}, {"enum", json::array({"high", "low"})}}}
                                            }}
                                        }}
                                    }}
                                }}
                            }},
                            {"captureRatio", {{"type", "integer"}, {"description", "Trigger position as percentage (0-100). 0=trigger at start, 100=trigger at end. Alternative to afterTriggerSeconds."}}},
                            {"repeatIntervalSeconds", {{"type", "number"}, {"description", "Time between repeat captures in seconds. Only used with repeat capture mode. Default: 0.1"}}}
                        }}
                    }}
                }}
            }}
        },
        // 3. stop_capture
        {
            {"name", "stop_capture"},
            {"description", "Stop the active capture. Use this to abort a capture that is in progress."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        // 4. wait_capture
        {
            {"name", "wait_capture"},
            {"description", "Wait for the current capture to complete. This call blocks until the capture finishes or times out. Call this after start_capture. May take minutes for long captures — set timeoutSeconds accordingly."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"timeoutSeconds", {
                        {"type", "number"},
                        {"description", "Maximum time to wait in seconds"},
                        {"default", 300}
                    }}
                }}
            }}
        },
        // 5. load_capture
        {
            {"name", "load_capture"},
            {"description", "Load a capture from a .pxc session file. Use this to analyze previously saved captures."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"filepath", {
                        {"type", "string"},
                        {"description", "Path to the capture file to load"}
                    }}
                }},
                {"required", json::array({"filepath"})}
            }}
        },
        // 6. save_capture
        {
            {"name", "save_capture"},
            {"description", "Save the current capture to a .pxc session file. Requires an active or completed capture."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"filepath", {
                        {"type", "string"},
                        {"description", "Path to save the capture file"}
                    }}
                }},
                {"required", json::array({"filepath"})}
            }}
        },
        // 7. close_capture
        {
            {"name", "close_capture"},
            {"description", "Close the current capture and free resources. Call this after you are done analyzing the data."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        // 8. add_analyzer
        {
            {"name", "add_analyzer"},
            {"description", "Add a protocol analyzer/decoder. Best called BEFORE start_capture so auto-decode triggers on capture completion. Use list_analyzers to discover available decoders, get_analyzer_options to see required channels/options. Use stackOnAnalyzerId to stack decoders (e.g. i2c_c -> eeprom24c)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"analyzerName", {
                        {"type", "string"},
                        {"description", "Name/ID of the analyzer to add (e.g. 'spi', 'i2c', 'uart')"}
                    }},
                    {"analyzerLabel", {
                        {"type", "string"},
                        {"description", "Custom label for the analyzer instance"}
                    }},
                    {"settings", {
                        {"type", "object"},
                        {"description", "Analyzer-specific settings (channel map, options, etc.)"}
                    }},
                    {"stackOnAnalyzerId", {
                        {"type", "string"},
                        {"description", "ID of an existing analyzer to stack this decoder on top of (for stacked/hierarchical decoding)"}
                    }}
                }},
                {"required", json::array({"analyzerName"})}
            }}
        },
        // 9. remove_analyzer
        {
            {"name", "remove_analyzer"},
            {"description", "Remove a protocol analyzer. Use the analyzerId returned by add_analyzer."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"analyzerId", {
                        {"type", "string"},
                        {"description", "ID of the analyzer instance to remove"}
                    }}
                }},
                {"required", json::array({"analyzerId"})}
            }}
        },
        // 9.5. list_analyzers
        {
            {"name", "list_analyzers"},
            {"description", "List all available protocol analyzers/decoders. Returns analyzer ID, name, description, and channel counts. Use get_analyzer_options to get detailed option/channel info for a specific analyzer."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        // 9.6. get_analyzer_options
        {
            {"name", "get_analyzer_options"},
            {"description", "Get the channel and option requirements for a protocol analyzer. Use this before add_analyzer to discover required/optional channels and configurable options."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"analyzerName", {
                        {"type", "string"},
                        {"description", "Name/ID of the analyzer (e.g. 'spi', 'i2c', 'uart')"}
                    }}
                }},
                {"required", json::array({"analyzerName"})}
            }}
        },
        // 10. export_raw_data_csv
        {
            {"name", "export_raw_data_csv"},
            {"description", "Export raw capture data as CSV files. Requires a completed capture. Use after wait_capture."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"directory", {
                        {"type", "string"},
                        {"description", "Output directory for CSV files"}
                    }},
                    {"digitalChannels", {
                        {"type", "array"},
                        {"items", {{"type", "integer"}}},
                        {"description", "Digital channel indices to export"}
                    }},
                    {"analogChannels", {
                        {"type", "array"},
                        {"items", {{"type", "integer"}}},
                        {"description", "Analog channel indices to export"}
                    }},
                    {"analogDownsampleRatio", {
                        {"type", "integer"},
                        {"description", "Downsample ratio for analog channels"},
                        {"default", 1}
                    }},
                    {"iso8601Timestamp", {
                        {"type", "boolean"},
                        {"description", "Use ISO 8601 timestamp format"},
                        {"default", false}
                    }}
                }},
                {"required", json::array({"directory"})}
            }}
        },
        // 11. export_raw_data_binary
        {
            {"name", "export_raw_data_binary"},
            {"description", "Export raw capture data as binary files. Requires a completed capture. Use after wait_capture."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"directory", {
                        {"type", "string"},
                        {"description", "Output directory for binary files"}
                    }},
                    {"digitalChannels", {
                        {"type", "array"},
                        {"items", {{"type", "integer"}}},
                        {"description", "Digital channel indices to export"}
                    }},
                    {"analogChannels", {
                        {"type", "array"},
                        {"items", {{"type", "integer"}}},
                        {"description", "Analog channel indices to export"}
                    }},
                    {"analogDownsampleRatio", {
                        {"type", "integer"},
                        {"description", "Downsample ratio for analog channels"},
                        {"default", 1}
                    }}
                }},
                {"required", json::array({"directory"})}
            }}
        },
        // 12. export_data_table_csv
        {
            {"name", "export_data_table_csv"},
            {"description", "Export analyzer results as a CSV data table. Requires a completed capture with analyzer results. Use get_analyzer_results first to verify data exists."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"filepath", {
                        {"type", "string"},
                        {"description", "Output CSV file path"}
                    }},
                    {"analyzers", {
                        {"type", "array"},
                        {"description", "Analyzers to export with their settings"},
                        {"items", {
                            {"type", "object"},
                            {"properties", {
                                {"analyzerId", {{"type", "string"}, {"description", "Analyzer instance ID"}}},
                                {"radixType", {{"type", "integer"}, {"description", "Radix type: 1=Binary, 2=Decimal, 3=Hex, 4=Ascii"}}}
                            }}
                        }}
                    }},
                    {"iso8601Timestamp", {
                        {"type", "boolean"},
                        {"description", "Use ISO 8601 timestamp format"},
                        {"default", false}
                    }}
                }},
                {"required", json::array({"filepath"})}
            }}
        },
        // 13. get_capture_status
        {
            {"name", "get_capture_status"},
            {"description", "Get the current capture status and progress. Use this to check if capture is idle, capturing, or completed."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        // 14. get_channels
        {
            {"name", "get_channels"},
            {"description", "Get the list of channels for the current device. Call this to discover available channel indices before configuring start_capture."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", json::object()}
            }}
        },
        // 15. get_analyzer_results
        {
            {"name", "get_analyzer_results"},
            {"description", "Get protocol analyzer decoded annotations/results. Call after wait_capture completes and decoding finishes. Use the analyzerId returned by add_analyzer."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"analyzerId", {
                        {"type", "string"},
                        {"description", "ID of the analyzer instance"}
                    }},
                    {"startSample", {
                        {"type", "integer"},
                        {"description", "Start sample for filtering results"}
                    }},
                    {"endSample", {
                        {"type", "integer"},
                        {"description", "End sample for filtering results"}
                    }},
                    {"maxCount", {
                        {"type", "integer"},
                        {"description", "Maximum number of annotations to return"},
                        {"default", 1000}
                    }}
                }},
                {"required", json::array({"analyzerId"})}
            }}
        }
    });
}

// ---- MCP Protocol Handlers ----

JsonRpcResponse RpcDispatcher::on_initialize(int id) {
    json result = {
        {"protocolVersion", "2025-03-26"},
        {"capabilities", {
            {"tools", json::object()}
        }},
        {"serverInfo", {
            {"name", "pxview"},
            {"version", "1.5.0"}
        }}
    };
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = true;
    resp.is_mcp_direct = true;
    resp.result_json = result.dump();
    return resp;
}

JsonRpcResponse RpcDispatcher::on_tools_list(int id) {
    json result = {
        {"tools", get_tool_schemas()}
    };
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = true;
    resp.is_mcp_direct = true;
    resp.result_json = result.dump();
    return resp;
}

JsonRpcResponse RpcDispatcher::on_ping(int id) {
    JsonRpcResponse resp;
    resp.id = id;
    resp.success = true;
    resp.result_json = json::object().dump();
    return resp;
}

// ---- MCP Tool Dispatch ----

JsonRpcResponse RpcDispatcher::dispatch_mcp_tool(int id, const std::string& tool_name, const json& args) {
    // Map MCP tool names to internal handlers
    if (tool_name == "get_devices")            return on_get_devices(id, args);
    if (tool_name == "start_capture")          return on_start_capture(id, args);
    if (tool_name == "stop_capture")           return on_stop_capture(id, args);
    if (tool_name == "wait_capture")           return on_wait_capture(id, args);
    if (tool_name == "load_capture")           return on_load_capture(id, args);
    if (tool_name == "save_capture")           return on_save_capture(id, args);
    if (tool_name == "close_capture")          return on_close_capture(id, args);
    if (tool_name == "add_analyzer")           return on_add_analyzer(id, args);
    if (tool_name == "remove_analyzer")        return on_remove_analyzer(id, args);
    if (tool_name == "list_analyzers")         return on_list_analyzers(id, args);
    if (tool_name == "get_analyzer_options")   return on_get_analyzer_options(id, args);
    if (tool_name == "export_raw_data_csv")    return on_export_raw_data_csv(id, args);
    if (tool_name == "export_raw_data_binary") return on_export_raw_data_binary(id, args);
    if (tool_name == "export_data_table_csv")  return on_export_data_table_csv(id, args);
    if (tool_name == "get_capture_status")     return on_get_capture_status(id, args);
    if (tool_name == "get_channels")           return on_get_channels(id, args);
    if (tool_name == "get_analyzer_results")   return on_get_analyzer_results(id, args);

    // Build MCP error response for unknown tool
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

    // Auto-create session if deviceId is provided (Logic 2 behavior)
    if (!session && params.contains("deviceId")) {
        std::string device_id = params.value("deviceId", "");
        if (!device_id.empty()) {
            mcp_dbg_log(QString("on_start_capture: creating session for device %1").arg(QString::fromStdString(device_id)).toUtf8().constData());
            auto r = app_svc_->create_session(device_id, "");
            if (!r.ok())
                return error_resp(id, static_cast<int>(r.error().code),
                                  "Failed to create session: " + r.error().message);
            session = app_svc_->get_active_session();
            mcp_dbg_log("on_start_capture: session created, processing events");

            // If create_session() called set_device() (device was not already active),
            // it triggered DSV_MSG_CURRENT_DEVICE_CHANGED which causes massive UI rebuilds.
            // We must let the UI process all pending events before continuing, otherwise
            // configure_and_start() will conflict with the ongoing UI rebuild.
            QCoreApplication::processEvents();
            QCoreApplication::processEvents(); // Second pass for cascading events
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
            operation_mode, buffer_options, digital_filter,
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

    // Auto-create session if none exists, so that add_analyzer can be called
    // before start_capture (the recommended MCP workflow: add decoder first,
    // then start capture, so DSV_MSG_COPY_TO_DOC_DONE auto-starts decode).
    if (!session) {
        mcp_dbg_log("on_add_analyzer: no active session, creating one");
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
            QCoreApplication::processEvents();
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

JsonRpcResponse RpcDispatcher::on_set_glitch_filter(int id, const json& params) {
    auto session = app_svc_->get_active_session();
    GlitchFilterConfig config;
    if (params.contains("channels"))
        config.channels = params["channels"].get<std::vector<int32_t>>();
    if (params.contains("thresholds"))
        config.thresholds = params["thresholds"].get<std::vector<int32_t>>();
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

} // namespace pv::api
