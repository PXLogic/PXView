// tool_schemas.inc — MCP tool schema JSON definitions for RpcDispatcher.
//
// This file is #included by rpc_dispatcher.cpp inside get_tool_schemas().
// It contains the 61 MCP tool schema definitions as a json::array literal.
//
// Extracted from rpc_dispatcher.cpp to reduce the file from 3,180 to ~2,000
// lines. The schema data is pure declarative JSON — no logic — so isolating
// it improves readability of the handler logic in rpc_dispatcher.cpp.
//
// DO NOT include this file directly from any other translation unit.
// It is only valid when included inside:
//   json RpcDispatcher::get_tool_schemas() {
//       return json::array({
//       #include "tool_schemas.inc"
//       });
//   }

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
    {"description", "Start a new capture. Typical workflow: 1) get_devices to find device ID, 2) add_analyzer to add decoders (recommended BEFORE capture so auto-decode works), 3) start_capture with device/channel config, 4) wait_capture to wait for completion, 5) get_analyzer_results to read decoded data. NOTE: In Stream mode (channelMode='Stream'), captureConfiguration.timedCaptureMode.durationSeconds and manualCaptureMode.sampleCount are IGNORED — stream mode uses continuous acquisition with no sample limit. Use stop_capture to end streaming."},
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
                            {"durationSeconds", {{"type", "number"}, {"description", "Capture duration in seconds (ignored in Stream mode)"}}}
                        }}
                    }},
                    {"manualCaptureMode", {
                        {"type", "object"},
                        {"properties", {
                            {"sampleCount", {{"type", "integer"}, {"description", "Number of samples to capture (ignored in Stream mode)"}}}
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
        }},
        {"required", json::array({"deviceId", "captureConfiguration"})}
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
                {"description", "Maximum time to wait in seconds. 0 = no timeout (wait forever). Default: 60."},
                {"default", 60}
            }}
        }}
    }}
},
// 5. load_capture
{
    {"name", "load_capture"},
    {"description", "Load a previously saved capture from a .pxc session file."},
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
    {"description", "Add a protocol analyzer/decoder. Best called BEFORE start_capture so auto-decode triggers on capture completion. Use list_analyzers to discover available decoders, get_analyzer_options to see required channels/options. Use stackOnAnalyzerId to stack decoders (e.g. i2c_c -> eeprom24c). Returns an analyzerId in the format '<handle_id>:<version>' that is stable across the stack's lifetime and used by get_analyzer_results / remove_analyzer / export_analyzer_table."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"analyzerName", {
                {"type", "string"},
                {"description", "Name/ID of the analyzer to add (e.g. 'spi', 'i2c', 'uart')"}
            }},
            {"deviceId", {
                {"type", "string"},
                {"description", "Optional device ID. If the active session has no device connected yet (typical in headless mode), pass this to connect the device before adding the analyzer. Same as start_capture's deviceId."}
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
                {"description", "Decoder instance identifier in the format '<handle_id>:<version>', returned by add_analyzer. Stable across the stack's lifetime; becomes invalid after the stack is destroyed/rebuilt. ID of an existing analyzer to stack this decoder on top of (for stacked/hierarchical decoding)"}
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
                {"description", "Decoder instance identifier in the format '<handle_id>:<version>', returned by add_analyzer. Stable across the stack's lifetime; becomes invalid after the stack is destroyed/rebuilt."}
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
// 11b. export_raw_data (unified multi-format)
{
    {"name", "export_raw_data"},
    {"description", "Export raw capture data in a chosen format: csv, binary, vcd, hex, or bits. Requires a completed capture. Use after wait_capture. Each channel is written to its own file (channel_N.<ext> / analog_N.<ext>)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"format", {
                {"type", "string"},
                {"description", "Export format: csv | binary | vcd | hex | bits"},
                {"default", "csv"}
            }},
            {"directory", {
                {"type", "string"},
                {"description", "Output directory for the exported files"}
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
                {"description", "Use ISO8601 timestamps in CSV output"},
                {"default", false}
            }}
        }},
        {"required", json::array({"directory"})}
    }}
},
// 12. export_data_table_csv
{
    {"name", "export_data_table_csv"},
    {"description", "Export decoded analyzer data as a CSV table. Requires a completed capture with at least one active decoder. Use after add_analyzer + wait_capture. Each row is one annotation; columns vary by decoder."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"filepath", {
                {"type", "string"},
                {"description", "Output CSV file path"}
            }},
            {"analyzerId", {
                {"type", "string"},
                {"description", "Decoder instance identifier in the format '<handle_id>:<version>', returned by add_analyzer. If omitted, exports all active decoders."}
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
    {"description", "Get channel list (index, name, type, enabled, vdiv, coupling, vfactor, map_default)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 15. get_analyzer_results
{
    {"name", "get_analyzer_results"},
    {"description", "Get decoded annotation results from a protocol analyzer. Returns annotations (ann_class, ann_class_id, data, start_sample, end_sample, row_index). Use after wait_capture + add_analyzer. Pagination via startAnnotation/maxAnnotations."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"analyzerId", {
                {"type", "string"},
                {"description", "Decoder instance identifier in the format '<handle_id>:<version>', returned by add_analyzer"}
            }},
            {"startAnnotation", {
                {"type", "integer"},
                {"description", "Starting annotation index for pagination (0-based)"},
                {"default", 0}
            }},
            {"maxAnnotations", {
                {"type", "integer"},
                {"description", "Maximum number of annotations to return. 0 = all."},
                {"default", 0}
            }}
        }},
        {"required", json::array({"analyzerId"})}
    }}
},
// 16. get_trigger_config
{
    {"name", "get_trigger_config"},
    {"description", "Get trigger configuration. Returns logic trigger config (stage_count + config_json) or DSO trigger config (source/slope/horiz_pos/holdoff/margin/channel) depending on 'mode' parameter or the active work mode."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"mode", {
                {"type", "string"},
                {"description", "Trigger config mode: 'logic' or 'dso'. If omitted, uses the active work mode."},
            }}
        }}
    }}
},
// 17. set_trigger_config
{
    {"name", "set_trigger_config"},
    {"description", "Set trigger configuration. Accepts logic trigger config (triggerStages with channelIndex/triggerType/etc) or DSO trigger config (source/slope/horiz_pos/holdoff/margin/channel). The 'mode' parameter selects which type to set."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"mode", {
                {"type", "string"},
                {"description", "Trigger config mode: 'logic' or 'dso'"}
            }},
            {"logicTriggerConfig", {
                {"type", "object"},
                {"description", "Logic trigger configuration (required when mode='logic')"},
                {"properties", {
                    {"triggerStages", {
                        {"type", "array"},
                        {"description", "Trigger stage configurations (up to 10 stages)"},
                        {"items", {
                            {"type", "object"},
                            {"properties", {
                                {"channelIndex", {{"type", "integer"}, {"description", "Trigger channel index"}}},
                                {"triggerType", {{"type", "string"}, {"description", "Trigger type: rising, falling, pulse_high, pulse_low"}}},
                                {"afterTriggerRatio", {{"type", "number"}, {"description", "Post-trigger ratio (0-1)"}}},
                                {"minPulseWidth", {{"type", "integer"}, {"description", "Min pulse width for pulse trigger (in samples)"}}},
                                {"maxPulseWidth", {{"type", "integer"}, {"description", "Max pulse width for pulse trigger (in samples)"}}},
                                {"linkedChannels", {
                                    {"type", "array"},
                                    {"items", {
                                        {"type", "object"},
                                        {"properties", {
                                            {"channelIndex", {{"type", "integer"}}},
                                            {"state", {{"type", "string"}, {"description", "high or low"}}}
                                        }}
                                    }}
                                }}
                            }}
                        }}
                    }}
                }}
            }},
            {"dsoTriggerConfig", {
                {"type", "object"},
                {"description", "DSO trigger configuration (required when mode='dso')"},
                {"properties", {
                    {"source", {{"type", "integer"}, {"description", "Trigger source channel index"}}},
                    {"slope", {{"type", "string"}, {"description", "Trigger slope: rising, falling"}}},
                    {"horiz_pos", {{"type", "number"}, {"description", "Horizontal position (0-1)"}}},
                    {"holdoff", {{"type", "integer"}, {"description", "Holdoff time in seconds"}}},
                    {"margin", {{"type", "integer"}, {"description", "Trigger margin"}}},
                    {"channel", {{"type", "integer"}, {"description", "Trigger channel index"}}}
                }}
            }}
        }},
        {"required", json::array({"mode"})}
    }}
},
// 18. get_probe_config
{
    {"name", "get_probe_config"},
    {"description", "Get probe configuration for a channel (vdiv/coupling/vfactor/map_default). Only meaningful for analog/DSO channels."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {
                {"type", "integer"},
                {"description", "Channel index to get probe config for"}
            }}
        }},
        {"required", json::array({"channelIndex"})}
    }}
},
// 19. set_probe_config
{
    {"name", "set_probe_config"},
    {"description", "Set probe configuration for a channel (vdiv/coupling/vfactor/map_default). Only meaningful for analog/DSO channels."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {
                {"type", "integer"},
                {"description", "Channel index to configure"}
            }},
            {"vdiv", {{"type", "integer"}, {"description", "Volts per division"}}},
            {"coupling", {{"type", "string"}, {"description", "Coupling mode: AC or DC"}}},
            {"vfactor", {{"type", "integer"}, {"description", "Voltage factor (probe attenuation ratio)"}}},
            {"mapDefault", {{"type", "boolean"}, {"description", "Whether to use default mapping"}}}
        }},
        {"required", json::array({"channelIndex"})}
    }}
},
// 20. set_channel_enabled
{
    {"name", "set_channel_enabled"},
    {"description", "Enable or disable a channel by index."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {
                {"type", "integer"},
                {"description", "Channel index to enable/disable"}
            }},
            {"enabled", {
                {"type", "boolean"},
                {"description", "true to enable, false to disable"}
            }}
        }},
        {"required", json::array({"channelIndex", "enabled"})}
    }}
},
// 21. set_channel_name
{
    {"name", "set_channel_name"},
    {"description", "Set the display name for a channel."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {
                {"type", "integer"},
                {"description", "Channel index to rename"}
            }},
            {"name", {
                {"type", "string"},
                {"description", "New channel name"}
            }}
        }},
        {"required", json::array({"channelIndex", "name"})}
    }}
},
// 22. get_sample_config
{
    {"name", "get_sample_config"},
    {"description", "Get the full sample configuration (sample_rate, sample_limit, time_base, collect_mode, stream_mode, rle_enabled, repeat_interval, repeat_hold_percent)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 23. set_sample_rate
{
    {"name", "set_sample_rate"},
    {"description", "Set the sample rate in Hz. Must be a value supported by the device."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"sampleRate", {
                {"type", "integer"},
                {"description", "Sample rate in Hz"}
            }}
        }},
        {"required", json::array({"sampleRate"})}
    }}
},
// 24. set_sample_limit
{
    {"name", "set_sample_limit"},
    {"description", "Set the sample limit (capture depth). Must be a value supported by the device."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"sampleLimit", {
                {"type", "integer"},
                {"description", "Number of samples to capture"}
            }}
        }},
        {"required", json::array({"sampleLimit"})}
    }}
},
// 25. set_time_base
{
    {"name", "set_time_base"},
    {"description", "Set the time base in seconds. The device will select the closest supported sample rate."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"timeBase", {
                {"type", "number"},
                {"description", "Time base in seconds"}
            }}
        }},
        {"required", json::array({"timeBase"})}
    }}
},
// 26. set_collect_mode
{
    {"name", "set_collect_mode"},
    {"description", "Set the collect mode (single, repeat, loop). Device-specific."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"collectMode", {
                {"type", "string"},
                {"description", "Collect mode: single, repeat, loop"}
            }}
        }},
        {"required", json::array({"collectMode"})}
    }}
},
// 27. set_repeat_interval
{
    {"name", "set_repeat_interval"},
    {"description", "Set the repeat interval in seconds for repeat capture mode."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"repeatInterval", {
                {"type", "number"},
                {"description", "Repeat interval in seconds"}
            }}
        }},
        {"required", json::array({"repeatInterval"})}
    }}
},
// 28. get_logic_samples
{
    {"name", "get_logic_samples"},
    {"description", "Read logic (digital) samples for a channel. Returns base64-encoded bytes (one byte per sample, 0 or 1). Use startSample/endSample to page through long captures."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {{"type", "integer"}, {"description", "Digital channel index"}}},
            {"startSample", {{"type", "integer"}, {"description", "Start sample index (0-based)"}, {"default", 0}}},
            {"endSample", {{"type", "integer"}, {"description", "End sample index (exclusive). 0 = to end."}, {"default", 0}}}
        }},
        {"required", json::array({"channelIndex"})}
    }}
},
// 29. get_analog_samples
{
    {"name", "get_analog_samples"},
    {"description", "Read analog samples for a channel. Returns base64-encoded float32 array. Use startSample/endSample to page through long captures."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {{"type", "integer"}, {"description", "Analog channel index"}}},
            {"startSample", {{"type", "integer"}, {"description", "Start sample index (0-based)"}, {"default", 0}}},
            {"endSample", {{"type", "integer"}, {"description", "End sample index (exclusive). 0 = to end."}, {"default", 0}}}
        }},
        {"required", json::array({"channelIndex"})}
    }}
},
// 30. get_dso_samples
{
    {"name", "get_dso_samples"},
    {"description", "Read DSO samples for a channel. Returns base64-encoded uint8 array. Use startSample/endSample to page through long captures."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {{"type", "integer"}, {"description", "DSO channel index"}}},
            {"startSample", {{"type", "integer"}, {"description", "Start sample index (0-based)"}, {"default", 0}}},
            {"endSample", {{"type", "integer"}, {"description", "End sample index (exclusive). 0 = to end."}, {"default", 0}}}
        }},
        {"required", json::array({"channelIndex"})}
    }}
},
// 31. find_next_edge
{
    {"name", "find_next_edge"},
    {"description", "Find the next logic edge on a channel starting from a sample index. Returns the sample index of the next rising or falling edge."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channelIndex", {{"type", "integer"}, {"description", "Digital channel index"}}},
            {"startSample", {{"type", "integer"}, {"description", "Sample index to start searching from"}}},
            {"edgeType", {{"type", "string"}, {"description", "Edge type: rising, falling, any"}, {"default", "any"}}}
        }},
        {"required", json::array({"channelIndex", "startSample"})}
    }}
},
// 32. find_pattern
{
    {"name", "find_pattern"},
    {"description", "Find the next occurrence of a multi-channel pattern. Specify channel states and a start sample to search from."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channels", {
                {"type", "array"},
                {"description", "Channel pattern to match"},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"channelIndex", {{"type", "integer"}}},
                        {"state", {{"type", "string"}, {"description", "high or low"}}}
                    }}
                }}
            }},
            {"startSample", {{"type", "integer"}, {"description", "Sample index to start searching from"}, {"default", 0}}}
        }},
        {"required", json::array({"channels"})}
    }}
},
// 33. get_active_decoders
{
    {"name", "get_active_decoders"},
    {"description", "List all currently active decoder instances (instance_id, decoder_id, display_name, row_index, is_running, progress)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 34. clear_all_decoders
{
    {"name", "clear_all_decoders"},
    {"description", "Remove all active decoders/analyzer instances."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 35. list_sessions
{
    {"name", "list_sessions"},
    {"description", "List all sessions (id + device/file summary). Use to manage multiple sessions."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 36. create_session
{
    {"name", "create_session"},
    {"description", "Create a new empty session. Returns the session ID. Use set_active_session to switch to it."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 37. destroy_session
{
    {"name", "destroy_session"},
    {"description", "Destroy a session by ID. Releases all resources associated with the session."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"sessionId", {{"type", "string"}, {"description", "Session ID to destroy"}}}
        }},
        {"required", json::array({"sessionId"})}
    }}
},
// 38. set_active_session
{
    {"name", "set_active_session"},
    {"description", "Set the active session by ID. Subsequent operations will target this session."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"sessionId", {{"type", "string"}, {"description", "Session ID to activate"}}}
        }},
        {"required", json::array({"sessionId"})}
    }}
},
// 39. get_session_count
{
    {"name", "get_session_count"},
    {"description", "Get the total number of sessions."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 40. connect_device
{
    {"name", "connect_device"},
    {"description", "Connect to a device by id (as returned by get_devices). Creates a new session bound to that device if no session exists."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"deviceId", {{"type", "string"}, {"description", "Device ID to connect"}}}
        }},
        {"required", json::array({"deviceId"})}
    }}
},
// 41. disconnect_device
{
    {"name", "disconnect_device"},
    {"description", "Disconnect the currently connected device."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 42. get_config
{
    {"name", "get_config"},
    {"description", "Read a generic SR_CONF_* config value by key. The 'type' field selects how to interpret the value. NOTE: 'int64' currently maps to int32 (no int64 getter exists yet)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"key", {{"type", "integer"}, {"description", "SR_CONF_* config key (numeric)"}}},
            {"type", {{"type", "string"}, {"description", "Value type: bool, int, int64, string, double"}}}
        }},
        {"required", json::array({"key", "type"})}
    }}
},
// 43. set_config
{
    {"name", "set_config"},
    {"description", "Write a generic SR_CONF_* config value by key. The 'type' field selects how to interpret the value."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"key", {{"type", "integer"}, {"description", "SR_CONF_* config key (numeric)"}}},
            {"type", {{"type", "string"}, {"description", "Value type: bool, int, int64, string, double"}}},
            {"value", {{"description", "Value to set (type depends on 'type' field)"}}}
        }},
        {"required", json::array({"key", "type", "value"})}
    }}
},
// 44. set_glitch_filter
{
    {"name", "set_glitch_filter"},
    {"description", "Enable glitch filter on channels with a minimum pulse-width threshold (in samples). Use 'channels' + 'threshold' (applies to all) or per-channel 'thresholds' + 'modes'."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channels", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Digital channel indices to filter"}}},
            {"threshold", {{"type", "integer"}, {"description", "Minimum pulse width in samples (applies to all channels)"}}},
            {"thresholds", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Per-channel thresholds (parallel to channels)"}}},
            {"modes", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Per-channel filter modes: pulse_high, pulse_low, both"}}}
        }},
        {"required", json::array({"channels"})}
    }}
},
// 45. clear_glitch_filter
{
    {"name", "clear_glitch_filter"},
    {"description", "Clear/disable glitch filter on specified channels (or all if omitted)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channels", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Channel indices to clear (omitted = all)"}}}
        }}
    }}
},
// 46. get_glitch_filter_config
{
    {"name", "get_glitch_filter_config"},
    {"description", "Get current glitch filter configuration (channels, thresholds, modes, is_active)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 47. set_signal_invert
{
    {"name", "set_signal_invert"},
    {"description", "Enable signal invert on specified channels."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channels", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Channel indices to invert"}}}
        }},
        {"required", json::array({"channels"})}
    }}
},
// 48. clear_signal_invert
{
    {"name", "clear_signal_invert"},
    {"description", "Clear signal invert. NOTE: currently clears all channels (the optional 'channels' parameter is ignored)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"channels", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"description", "Optional channel indices (currently ignored — clears all)"}}}
        }}
    }}
},
// 49. get_signal_invert_config
{
    {"name", "get_signal_invert_config"},
    {"description", "Get current signal invert configuration (channels + invert_states). NOTE: per-channel detail requires extending SigSession's public API — currently only the active state is reported."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 50. get_repeat_status
{
    {"name", "get_repeat_status"},
    {"description", "Get repeat/collect mode status (is_single_mode, is_repeat_mode, is_loop_mode, repeat_interval, repeat_hold_percent)."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 51. get_disk_cache_info
{
    {"name", "get_disk_cache_info"},
    {"description", "Get disk cache info (enabled, write_speed_mbps, write_queue_depth, is_disk_full). NOTE: 'total_blocks_written' is a TODO — not yet exposed by SessionService."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// ===== Batch B tools =====
// 52. refresh_device_list
{
    {"name", "refresh_device_list"},
    {"description", "Trigger a hot-plug rescan of all drivers and return the updated device list. Use this when a device was connected/disconnected after PXView started. Returns the same DeviceInfo array as get_devices."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 53. set_save_range
{
    {"name", "set_save_range"},
    {"description", "Set the save range (in samples) used by export/save operations. startSample must be less than endSample. Both are absolute sample indices into the current capture."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"startSample", {{"type", "integer"}, {"description", "Start sample index (inclusive)"}}},
            {"endSample", {{"type", "integer"}, {"description", "End sample index (inclusive)"}}}
        }},
        {"required", json::array({"startSample", "endSample"})}
    }}
},
// 54. reconfigure_decoder
{
    {"name", "reconfigure_decoder"},
    {"description", "Reconfigure an existing decoder's options and channel_map in place (no remove + re-add). Triggers a re-decode of the affected stack. Use the analyzerId returned by add_analyzer."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"analyzerId", {{"type", "string"}, {"description", "Decoder instance identifier in the format '<handle_id>:<version>', returned by add_analyzer. Stable across the stack's lifetime; becomes invalid after the stack is destroyed/rebuilt."}}},
            {"options", {{"type", "object"}, {"description", "Decoder option values (string -> string). Omitted keys retain their current values."}, {"additionalProperties", {{"type", "string"}}}}},
            {"channelMap", {{"type", "object"}, {"description", "Channel mapping (decoder channel id -> device channel index). Omitted keys retain their current mapping."}, {"additionalProperties", {{"type", "integer"}}}}}
        }},
        {"required", json::array({"analyzerId"})}
    }}
},
// 55. get_decoder_class_names
{
    {"name", "get_decoder_class_names"},
    {"description", "Return the annotation class names declared by a decoder (the __annotations__ metadata). class_id is the index into the decoder's annotations list and matches the ann_class field of get_analyzer_results annotations. Use this to translate numeric ann_class values to human-readable names."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"analyzerName", {{"type", "string"}, {"description", "Decoder id/name (e.g. 'spi', 'i2c', 'uart') — same as add_analyzer's analyzerName"}}}
        }},
        {"required", json::array({"analyzerName"})}
    }}
},
// 56. get_decoder_binary_output
{
    {"name", "get_decoder_binary_output"},
    {"description", "Read a decoder's binary output stream. output_id selects which binary output class to read. NOTE: current implementation returns ConfigNotSupported because DecoderStack does not register binary output callbacks — this is a data-layer TODO."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", {
            {"analyzerId", {{"type", "string"}, {"description", "Decoder instance identifier in the format '<handle_id>:<version>', returned by add_analyzer."}}},
            {"outputId", {{"type", "integer"}, {"description", "Binary output class id (matches srd_decoder_binary::bin_class)"}}}
        }},
        {"required", json::array({"analyzerId", "outputId"})}
    }}
},
// 57. get_math_results
{
    {"name", "get_math_results"},
    {"description", "Read computed math trace results (ch1_index, ch2_index, math_type, sample_num, samples). math_type: 0=ADD, 1=SUB, 2=MUL, 3=DIV. Returns is_enabled=false when math trace has not been enabled. samples is empty when no data has been computed yet."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 58. get_spectrum_results
{
    {"name", "get_spectrum_results"},
    {"description", "Read computed FFT spectrum results (channel_index, windows_index, dc_ignored, sample_interval, spectrum). Returns is_enabled=false when spectrum trace has not been enabled. spectrum is empty when no data has been computed yet."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 59. get_lissajous_results
{
    {"name", "get_lissajous_results"},
    {"description", "Read Lissajous trace configuration (x_index, y_index, percent). Returns is_enabled=false when Lissajous trace has not been enabled. Note: the actual XY sample rendering is a View-layer concern, so this returns only configuration, not rendered samples."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 60. get_error_state
{
    {"name", "get_error_state"},
    {"description", "Read the session error state (has_error, error_code, error_pattern, error_message). error_code mirrors SigSession::SESSION_ERROR_STATUS (0=No_err, 1=Hw_err, 2=Malloc_err, 3=Test_timeout_err, 4=Pkt_data_err, 5=Data_overflow). error_pattern is a bitmask of error conditions accumulated since last clear."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
},
// 61. clear_error_state
{
    {"name", "clear_error_state"},
    {"description", "Clear the session error state (resets error_code to 0, error_pattern to 0, error_message to empty). Use after handling a previously reported error to allow subsequent captures to be checked cleanly."},
    {"inputSchema", {
        {"type", "object"},
        {"properties", json::object()}
    }}
}
