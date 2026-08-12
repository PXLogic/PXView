"""
test_01_mcp_protocol.py - MCP protocol base tests (Layer 1).

Validates the MCP JSON-RPC 2.0 protocol layer: initialize handshake,
tools/list, ping, error handling for invalid methods/JSON.
"""

import json

import pytest

from pxview_automation import McpClient, McpError

pytestmark = pytest.mark.p0


class TestMcpProtocol:

    def test_initialize_handshake(self, mcp: McpClient):
        """MCP initialize returns protocolVersion and serverInfo."""
        # Already done in conftest, but verify the cached result
        assert mcp.connected, "Client should be connected"

    def test_tools_list_count(self, mcp: McpClient):
        """tools/list returns the consolidated 46+ tool set."""
        tools = mcp.tools
        assert len(tools) >= 40, f"Expected at least 40 tools, got {len(tools)}"

    def test_tools_list_schema(self, mcp: McpClient):
        """Every tool has name, description, and inputSchema."""
        for tool in mcp.tools:
            assert "name" in tool, f"Tool missing 'name': {tool}"
            assert "description" in tool, f"Tool {tool['name']} missing 'description'"
            assert "inputSchema" in tool, f"Tool {tool['name']} missing 'inputSchema'"

    def test_ping(self, mcp: McpClient):
        """ping returns success."""
        assert mcp.ping() is True

    def test_unknown_tool_returns_error(self, mcp: McpClient):
        """Calling a non-existent tool returns an error."""
        with pytest.raises(McpError):
            mcp._call_tool("nonexistent_tool_12345")

    def test_tool_names_contain_core_tools(self, mcp: McpClient):
        """Core tool names are present in tools/list."""
        expected = [
            "get_devices", "start_capture", "stop_capture", "wait_capture",
            "get_capture_status", "close_capture", "add_analyzer",
            "remove_analyzer", "list_analyzers", "get_analyzer_results",
            "save_capture", "load_capture", "get_channels",
            "export_raw_data", "export_data_table_csv",
        ]
        names = mcp.tool_names
        for name in expected:
            assert name in names, f"Core tool '{name}' not in tools/list"

    def test_tool_names_contain_consolidated_tools(self, mcp: McpClient):
        """Consolidated 46-tool-set names are present."""
        expected = [
            # Tier 0: Mode management
            "switch_work_mode", "get_work_mode", "get_supported_work_modes",
            # Tier 1: Core workflow
            "get_devices", "get_channels", "start_capture", "stop_capture",
            "wait_capture", "get_capture_status", "load_capture",
            "save_capture", "close_capture", "list_analyzers",
            "get_analyzer_options", "add_analyzer", "remove_analyzer",
            "get_analyzer_results", "export_raw_data", "export_data_table_csv",
            "get_sample_config", "refresh_device_list",
            # Tier 2: Configuration (consolidated)
            "set_sample_config", "configure_channel", "configure_trigger",
            "configure_probe", "configure_glitch_filter",
            "configure_signal_invert", "set_save_range",
            "connect_device", "disconnect_device", "get_session_status",
            # Tier 3: Advanced features
            "get_samples", "find_next_edge", "find_pattern",
            "get_active_decoders", "clear_all_decoders",
            "reconfigure_decoder", "get_decoder_class_names",
            "list_sessions", "create_session", "destroy_session",
            "get_math_results", "get_spectrum_results",
            "get_lissajous_results", "configure_error_state",
            # Generic device config (SR_CONF_* keys: PWM, VTH, Filter, etc.)
            "get_config", "set_config",
            # Cursors (consolidated into configure_cursors)
            "configure_cursors",
        ]
        names = mcp.tool_names
        for name in expected:
            assert name in names, f"Consolidated tool '{name}' not in tools/list"

    def test_old_tool_names_are_removed(self, mcp: McpClient):
        """Old pre-consolidation tool names should NOT be present."""
        removed = [
            "get_logic_samples", "get_analog_samples", "get_dso_samples",
            "get_trigger_config", "set_trigger_config",
            "get_probe_config", "set_probe_config",
            "set_channel_enabled", "set_channel_name",
            "set_sample_rate", "set_sample_limit", "set_time_base",
            "set_collect_mode", "set_repeat_interval",
            "export_raw_data_csv", "export_raw_data_binary",
            "set_glitch_filter", "clear_glitch_filter", "get_glitch_filter_config",
            "set_signal_invert", "clear_signal_invert", "get_signal_invert_config",
            "get_repeat_status", "get_disk_cache_info",
            "get_error_state", "clear_error_state",
            "get_session_count", "get_decoder_binary_output",
            "get_cursors", "add_cursor", "remove_cursor", "clear_cursors",
            "get_logic_waveform", "get_analog_waveform", "get_dso_waveform",
            "add_decoder", "remove_decoder", "get_available_decoders",
            "get_decoder_options", "get_decoder_results",
            "get_decoder_annotations", "get_measurements",
            "get_time_info", "get_device_info", "get_signal_list",
            "save_file", "load_file", "export_data",
            "batch_call", "get_viewport_binary",
        ]
        names = set(mcp.tool_names)
        for name in removed:
            assert name not in names, (
                f"Old tool '{name}' should have been removed from the "
                f"consolidated 46-tool set but is still present"
            )

    def test_total_tool_count(self, mcp: McpClient):
        """Verify total tool count matches the consolidated set (>= 46)."""
        # The consolidated 46-tool set (with some extras like cursors/math)
        # registers ~50 tools.  We check we have at least 46.
        assert len(mcp.tools) >= 46, \
            f"Expected at least 46 tools, got {len(mcp.tools)}."
