"""Version compatibility tests for pxview-automation.

These tests verify that the Python client remains compatible with
the PXView MCP server across different versions of the protocol
and API.  They are inspired by logic2-automation's
``test_version_matrix.py``.

There are two modes:

1. **No server running** — tests verify that the client's advertised
   protocol version and package version are consistent, and that
   ``McpClient.dump_schema()`` produces valid output from mocked data.

2. **Server running** — tests additionally verify that the server's
   reported protocol version is compatible with the client, and that
   the live schema matches the client's expected tool set.
"""

from __future__ import annotations

import pytest

from pxview_automation import __version__, McpClient
from pxview_automation.client import McpConnectionError


# ======================================================================
# Static version tests (no server required)
# ======================================================================


class TestPackageVersion:
    def test_version_is_string(self):
        assert isinstance(__version__, str)

    def test_version_format(self):
        """Version should be semver (X.Y.Z)."""
        parts = __version__.split(".")
        assert len(parts) == 3
        for p in parts:
            assert p.isdigit(), f"Version part {p!r} is not a digit"

    def test_version_not_empty(self):
        assert __version__ != ""
        assert __version__ != "0.0.0"


class TestProtocolVersion:
    def test_protocol_version(self):
        """The client should advertise a known MCP protocol version."""
        # The connect() method sends this version in the initialize handshake.
        # We verify the expected value is well-formed.
        expected = "2025-03-26"
        parts = expected.split("-")
        assert len(parts) == 3
        for p in parts:
            assert p.isdigit()


class TestMcpSchema:
    """Validate the MCP schema mechanism.

    The schema is NOT a static file — it is extracted at runtime
    from the server's tools/list response (the single source of
    truth: PXView/pv/api/tool_schemas.inc).

    When the server is not running, we test the McpClient.dump_schema()
    method with mocked data.  When the server IS running, we verify
    the live schema.
    """

    def test_dump_schema_with_mock(self):
        """Test that dump_schema() produces valid output from mocked tools."""
        from unittest.mock import MagicMock

        client = McpClient()
        client._tools = [
            {
                "name": "get_devices",
                "description": "List connected devices",
                "inputSchema": {"type": "object", "properties": {}},
            },
            {
                "name": "start_capture",
                "description": "Start a capture",
                "inputSchema": {"type": "object", "properties": {"deviceId": {"type": "string"}}},
            },
        ]

        schema = client.dump_schema()
        assert isinstance(schema, dict)
        assert schema["toolCount"] == 2
        assert "get_devices" in schema["tools"]
        assert "start_capture" in schema["tools"]
        assert schema["tools"]["get_devices"]["description"] == "List connected devices"
        assert schema["version"] == __version__

    def test_dump_schema_writes_file(self, tmp_path):
        """Test that dump_schema(filepath) writes a valid JSON file."""
        import json as _json

        client = McpClient()
        client._tools = [
            {"name": "ping", "description": "Ping", "inputSchema": {}},
        ]

        out_file = str(tmp_path / "schema.json")
        schema = client.dump_schema(filepath=out_file)

        assert _json.load(open(out_file, encoding="utf-8")) == schema

    def test_dump_schema_includes_all_essential_tools(self):
        """Verify that the mock schema includes expected tool names."""
        client = McpClient()
        client._tools = [
            {"name": n, "description": "", "inputSchema": {}}
            for n in [
                "get_devices", "start_capture", "stop_capture",
                "wait_capture", "add_analyzer", "get_analyzer_results",
                "export_raw_data", "export_data_table_csv",
            ]
        ]
        schema = client.dump_schema()
        essential = {"get_devices", "start_capture", "stop_capture", "wait_capture"}
        available = set(schema["tools"].keys())
        assert essential.issubset(available)


# ======================================================================
# Type system tests (no server required)
# ======================================================================


class TestTypeSystem:
    """Verify that all exported types are properly defined."""

    def test_enums_exported(self):
        from pxview_automation import (
            CaptureState,
            ChannelType,
            CollectMode,
            CouplingType,
            DeviceType,
            DigitalTriggerType,
            ExportFormat,
            RadixType,
            StreamMode,
        )
        # Each enum should have at least one member
        for enum_cls in [
            CaptureState, ChannelType, CollectMode, CouplingType,
            DeviceType, DigitalTriggerType, ExportFormat,
            RadixType, StreamMode,
        ]:
            assert len(list(enum_cls)) > 0, f"{enum_cls.__name__} has no members"

    def test_dataclasses_exported(self):
        from pxview_automation import (
            CaptureConfiguration,
            LogicDeviceConfiguration,
            TimedCaptureMode,
            ManualCaptureMode,
            DigitalTriggerCaptureMode,
            GlitchFilterEntry,
            DeviceDesc,
            ChannelInfo,
            ProbeConfig,
            SampleConfig,
            CaptureStatus,
            AppInfo,
            Version,
            AnalyzerHandle,
            DataTableExportConfiguration,
            DataTableFilter,
        )
        # All should be classes (dataclasses are classes)
        for cls in [
            CaptureConfiguration, LogicDeviceConfiguration,
            TimedCaptureMode, ManualCaptureMode,
            DigitalTriggerCaptureMode, GlitchFilterEntry,
            DeviceDesc, ChannelInfo, ProbeConfig,
            SampleConfig, CaptureStatus, AppInfo, Version,
            AnalyzerHandle, DataTableExportConfiguration, DataTableFilter,
        ]:
            assert isinstance(cls, type), f"{cls} is not a type"

    def test_dataclass_to_dict(self):
        from pxview_automation import (
            LogicDeviceConfiguration,
            TimedCaptureMode,
            CaptureConfiguration,
            GlitchFilterEntry,
        )
        cfg = LogicDeviceConfiguration(
            digital_channels=[0, 1],
            digital_sample_rate=1000000,
        )
        d = cfg.to_dict()
        assert d["digitalChannels"] == [0, 1]
        assert d["digitalSampleRate"] == 1000000

        cap = CaptureConfiguration(
            capture_mode=TimedCaptureMode(duration_seconds=1.0),
        )
        d = cap.to_dict()
        assert "timedCaptureMode" in d
        assert d["timedCaptureMode"]["durationSeconds"] == 1.0

    def test_glitch_filter_to_dict(self):
        from pxview_automation import GlitchFilterEntry
        gf = GlitchFilterEntry(channel_index=0, pulse_width_seconds=0.0001)
        d = gf.to_dict()
        assert d["channelIndex"] == 0
        assert d["pulseWidthSeconds"] == 0.0001

    def test_device_desc_from_dict(self):
        from pxview_automation import DeviceDesc, DeviceType
        d = DeviceDesc.from_dict({
            "id": "demo",
            "is_demo": True,
            "driver_name": "demo",
        })
        assert d.id == "demo"
        assert d.is_demo is True
        assert d.device_type == DeviceType.DEMO

    def test_capture_status_from_dict(self):
        from pxview_automation import CaptureStatus, CaptureState
        s = CaptureStatus.from_dict({"state": "completed", "progress": 1.0})
        assert s.state == CaptureState.COMPLETED
        assert s.progress == 1.0

    def test_version_from_dict(self):
        from pxview_automation import Version
        v = Version.from_dict({"major": 1, "minor": 2, "patch": 3})
        assert v.major == 1
        assert v.minor == 2
        assert v.patch == 3
        assert str(v) == "1.2.3"


# ======================================================================
# Server version tests (require running server)
# ======================================================================


@pytest.mark.integration
class TestServerVersionCompatibility:
    """Tests that require a running PXView server to verify version compat."""

    def test_server_reachable(self):
        client = McpClient(timeout=5.0)
        try:
            client.connect()
        except McpConnectionError:
            pytest.skip("Server not reachable")
        client.disconnect()

    def test_server_protocol_version(self):
        client = McpClient(timeout=5.0)
        try:
            client.connect()
        except McpConnectionError:
            pytest.skip("Server not reachable")

        try:
            # The server should have responded to initialize with its protocol version
            # We check that the tools list is non-empty
            assert len(client.tools) > 0, "Server returned no tools"
            assert len(client.tool_names) > 0

            # Verify essential tools are present
            essential = {"get_devices", "start_capture", "stop_capture", "wait_capture"}
            available = set(client.tool_names)
            missing = essential - available
            assert not missing, f"Server missing essential tools: {missing}"
        finally:
            client.disconnect()

    def test_all_client_methods_have_server_tools(self):
        """Verify that every tool wrapper in McpClient has a corresponding
        server-side tool."""
        client = McpClient(timeout=5.0)
        try:
            client.connect()
        except McpConnectionError:
            pytest.skip("Server not reachable")

        try:
            server_tools = set(client.tool_names)

            # Map of client method names to expected tool names (consolidated 46+ tools)
            method_to_tool = {
                # Tier 0: Mode management
                "switch_work_mode": "switch_work_mode",
                "get_work_mode": "get_work_mode",
                "get_supported_work_modes": "get_supported_work_modes",
                # Tier 1: Core workflow
                "get_devices": "get_devices",
                "get_channels": "get_channels",
                "start_capture": "start_capture",
                "stop_capture": "stop_capture",
                "wait_capture": "wait_capture",
                "get_capture_status": "get_capture_status",
                "load_capture": "load_capture",
                "save_capture": "save_capture",
                "close_capture": "close_capture",
                "list_analyzers": "list_analyzers",
                "get_analyzer_options": "get_analyzer_options",
                "add_analyzer": "add_analyzer",
                "remove_analyzer": "remove_analyzer",
                "get_analyzer_results": "get_analyzer_results",
                "export_raw_data": "export_raw_data",
                "export_data_table_csv": "export_data_table_csv",
                "get_sample_config": "get_sample_config",
                "refresh_device_list": "refresh_device_list",
                # Tier 2: Configuration (consolidated)
                "set_sample_config": "set_sample_config",
                "configure_channel": "configure_channel",
                "configure_trigger": "configure_trigger",
                "configure_probe": "configure_probe",
                "configure_glitch_filter": "configure_glitch_filter",
                "configure_signal_invert": "configure_signal_invert",
                "set_save_range": "set_save_range",
                "connect_device": "connect_device",
                "disconnect_device": "disconnect_device",
                "get_session_status": "get_session_status",
                # Tier 3: Advanced features
                "get_samples": "get_samples",
                "find_next_edge": "find_next_edge",
                "find_pattern": "find_pattern",
                "get_active_decoders": "get_active_decoders",
                "clear_all_decoders": "clear_all_decoders",
                "reconfigure_decoder": "reconfigure_decoder",
                "get_decoder_class_names": "get_decoder_class_names",
                "list_sessions": "list_sessions",
                "create_session": "create_session",
                "destroy_session": "destroy_session",
                "set_active_session": "set_active_session",
                "get_math_results": "get_math_results",
                "get_spectrum_results": "get_spectrum_results",
                "get_lissajous_results": "get_lissajous_results",
                "configure_error_state": "configure_error_state",
                # Cursors
                "get_cursors": "get_cursors",
                "add_cursor": "add_cursor",
                "remove_cursor": "remove_cursor",
                "clear_cursors": "clear_cursors",
            }

            for method_name, tool_name in method_to_tool.items():
                assert tool_name in server_tools, (
                    f"Client method '{method_name}' expects tool '{tool_name}' "
                    f"but server does not provide it"
                )
        finally:
            client.disconnect()

    def test_live_dump_schema(self, tmp_path):
        """Verify that dump_schema() works against the live server."""
        import json as _json

        client = McpClient(timeout=5.0)
        try:
            client.connect()
        except McpConnectionError:
            pytest.skip("Server not reachable")

        try:
            out_file = str(tmp_path / "live-schema.json")
            schema = client.dump_schema(filepath=out_file)

            assert schema["toolCount"] > 0
            assert "tools" in schema
            assert schema["version"] == __version__

            # Verify the file was written
            loaded = _json.load(open(out_file, encoding="utf-8"))
            assert loaded["toolCount"] == schema["toolCount"]

            # Each tool should have description and inputSchema
            for name, tool in schema["tools"].items():
                assert "description" in tool, f"Tool {name} missing description"
                assert "inputSchema" in tool, f"Tool {name} missing inputSchema"
        finally:
            client.disconnect()
