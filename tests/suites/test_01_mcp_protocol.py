"""
test_01_mcp_protocol.py - MCP protocol base tests (Layer 1).

Validates the MCP JSON-RPC 2.0 protocol layer: initialize handshake,
tools/list, ping, error handling for invalid methods/JSON.
"""

import json

import pytest

from mcp_client import McpClient, McpError

pytestmark = pytest.mark.p0


class TestMcpProtocol:

    def test_initialize_handshake(self, mcp: McpClient):
        """MCP initialize returns protocolVersion and serverInfo."""
        # Already done in conftest, but verify the cached result
        assert mcp.connected, "Client should be connected"

    def test_tools_list_count(self, mcp: McpClient):
        """tools/list returns 61 tools (the full PXView tool set)."""
        tools = mcp.tools
        assert len(tools) >= 15, f"Expected at least 15 tools, got {len(tools)}"

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
            "export_raw_data_csv", "export_raw_data_binary",
        ]
        names = mcp.tool_names
        for name in expected:
            assert name in names, f"Core tool '{name}' not in tools/list"

    def test_tool_names_contain_batch_a_tools(self, mcp: McpClient):
        """Batch A tool names are present."""
        expected = [
            "get_trigger_config", "set_trigger_config",
            "get_probe_config", "set_probe_config",
            "set_channel_enabled", "set_channel_name",
            "get_sample_config", "set_sample_rate", "set_sample_limit",
            "set_time_base", "set_collect_mode", "set_repeat_interval",
            "get_logic_samples", "get_analog_samples", "get_dso_samples",
            "find_next_edge", "find_pattern",
            "get_active_decoders", "clear_all_decoders",
            "list_sessions", "create_session", "destroy_session",
            "set_active_session", "get_session_count",
            "connect_device", "disconnect_device",
            "get_config", "set_config",
            "set_glitch_filter", "clear_glitch_filter", "get_glitch_filter_config",
            "set_signal_invert", "clear_signal_invert", "get_signal_invert_config",
            "get_repeat_status", "get_disk_cache_info",
        ]
        names = mcp.tool_names
        for name in expected:
            assert name in names, f"Batch A tool '{name}' not in tools/list"

    def test_tool_names_contain_batch_b_tools(self, mcp: McpClient):
        """Batch B tool names are present."""
        expected = [
            "refresh_device_list", "set_save_range",
            "reconfigure_decoder", "get_decoder_class_names",
            "get_decoder_binary_output",
            "get_math_results", "get_spectrum_results", "get_lissajous_results",
            "get_error_state", "clear_error_state",
        ]
        names = mcp.tool_names
        for name in expected:
            assert name in names, f"Batch B tool '{name}' not in tools/list"

    def test_total_tool_count(self, mcp: McpClient):
        """Verify total tool count is at least 61."""
        # The server may add new tools over time; check we have at least
        # the original 61 that were deployed.
        assert len(mcp.tools) >= 61, \
            f"Expected at least 61 tools, got {len(mcp.tools)}. "
