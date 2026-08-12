"""Tests for pxview-automation.

These tests do NOT require a running PXView server — they test
the client's logic (argument parsing, error handling, SSE parsing,
path conversion, etc.) using mocks.

For integration tests that require a real PXView instance, see
``test_integration.py`` (not included by default).
"""

from __future__ import annotations

import json
from unittest.mock import MagicMock, patch

import pytest

from pxview_automation import (
    McpClient,
    McpConnectionError,
    McpError,
    PXView,
    PXViewProcess,
    PxvError,
    __version__,
)
from pxview_automation._utils import (
    format_duration,
    parse_duration,
    parse_int_list,
    to_windows_path,
)
from pxview_automation.cli import _parse_rate, _parse_channel_map, _parse_options
from pxview_automation.types import (
    AppInfo,
    CaptureConfiguration,
    CaptureState,
    CaptureStatus,
    ChannelInfo,
    ChannelType,
    CollectMode,
    CouplingType,
    DataTableExportConfiguration,
    DataTableFilter,
    DeviceDesc,
    DeviceType,
    DigitalTriggerCaptureMode,
    DigitalTriggerLinkedChannel,
    DigitalTriggerLinkedChannelState,
    DigitalTriggerType,
    ExportFormat,
    GlitchFilterEntry,
    LogicDeviceConfiguration,
    ManualCaptureMode,
    ProbeConfig,
    RadixType,
    SampleConfig,
    StreamMode,
    TimedCaptureMode,
    Version,
)


# ======================================================================
# Version
# ======================================================================

def test_version():
    assert __version__ == "1.5.5"


# ======================================================================
# Utility functions
# ======================================================================

class TestParseDuration:
    def test_bare_number(self):
        assert parse_duration("90") == 90.0

    def test_seconds(self):
        assert parse_duration("2s") == 2.0
        assert parse_duration("1.5s") == 1.5

    def test_milliseconds(self):
        assert parse_duration("500ms") == 0.5

    def test_minutes(self):
        assert parse_duration("3m") == 180.0
        assert parse_duration("1min") == 60.0

    def test_hours(self):
        assert parse_duration("1h") == 3600.0
        assert parse_duration("2hr") == 7200.0

    def test_invalid(self):
        with pytest.raises(ValueError):
            parse_duration("abc")

    def test_empty(self):
        with pytest.raises(ValueError):
            parse_duration("")


class TestFormatDuration:
    def test_seconds(self):
        assert "0.5s" in format_duration(0.5)

    def test_minutes(self):
        result = format_duration(65.0)
        assert "1m" in result

    def test_hours(self):
        result = format_duration(3700.0)
        assert "1h" in result


class TestParseIntList:
    def test_simple(self):
        assert parse_int_list("0,1,2") == [0, 1, 2]

    def test_range(self):
        assert parse_int_list("0-3") == [0, 1, 2, 3]

    def test_mixed(self):
        assert parse_int_list("0,2-4,6") == [0, 2, 3, 4, 6]

    def test_single(self):
        assert parse_int_list("5") == [5]

    def test_empty_parts(self):
        assert parse_int_list("0,,1,") == [0, 1]


class TestToWindowsPath:
    def test_msys2_path(self):
        # Only applies on Windows
        import sys
        if sys.platform != "win32":
            return
        assert to_windows_path("/c/Users/test") == "C:/Users/test"

    def test_normal_path(self):
        assert to_windows_path("C:/Users/test") == "C:/Users/test"

    def test_empty(self):
        assert to_windows_path("") == ""


# ======================================================================
# CLI helpers
# ======================================================================

class TestParseRate:
    def test_plain(self):
        assert _parse_rate("1000000") == 1000000

    def test_k(self):
        assert _parse_rate("100K") == 100000
        assert _parse_rate("100k") == 100000

    def test_m(self):
        assert _parse_rate("1M") == 1000000
        assert _parse_rate("2.5M") == 2500000

    def test_g(self):
        assert _parse_rate("1G") == 1000000000


class TestParseChannelMap:
    def test_single(self):
        assert _parse_channel_map(["scl=0"]) == {"scl": 0}

    def test_multiple(self):
        assert _parse_channel_map(["scl=0", "sda=1"]) == {"scl": 0, "sda": 1}

    def test_spaces(self):
        assert _parse_channel_map(["scl = 0"]) == {"scl": 0}

    def test_invalid(self):
        with pytest.raises(ValueError):
            _parse_channel_map(["invalid"])


class TestParseOptions:
    def test_single(self):
        assert _parse_options(["baudrate=115200"]) == {"baudrate": "115200"}

    def test_multiple(self):
        result = _parse_options(["baudrate=115200", "data_bits=8"])
        assert result == {"baudrate": "115200", "data_bits": "8"}

    def test_invalid(self):
        with pytest.raises(ValueError):
            _parse_options(["invalid"])


# ======================================================================
# McpClient (mocked)
# ======================================================================

class TestMcpClient:
    def test_init_defaults(self):
        client = McpClient()
        assert client.url == "http://127.0.0.1:10110/mcp"
        assert client.timeout == 60.0
        assert client.max_retries == 3
        assert client.retry_delay == 0.5
        assert client.connected is False

    def test_init_custom(self):
        client = McpClient(
            url="http://192.168.1.100:10110/mcp",
            timeout=120.0,
            max_retries=5,
        )
        assert client.url == "http://192.168.1.100:10110/mcp"
        assert client.timeout == 120.0
        assert client.max_retries == 5

    def test_repr(self):
        client = McpClient()
        assert "disconnected" in repr(client)

    def test_context_manager(self):
        with McpClient() as client:
            assert client is not None
        assert client.connected is False

    def test_parse_sse_response_simple_json(self):
        text = json.dumps({"jsonrpc": "2.0", "id": 1, "result": {}})
        result = McpClient._parse_sse_response(text)
        assert result["jsonrpc"] == "2.0"

    def test_parse_sse_response_with_events(self):
        text = (
            "event: progress\n"
            'data: {"status":"capturing"}\n'
            "\n"
            "event: result\n"
            'data: {"jsonrpc":"2.0","id":1,"result":{"state":"completed"}}\n'
        )
        result = McpClient._parse_sse_response(text)
        assert result["result"]["state"] == "completed"

    def test_parse_sse_response_no_result_event(self):
        text = (
            "event: progress\n"
            'data: {"status":"capturing"}\n'
        )
        with pytest.raises(McpConnectionError):
            McpClient._parse_sse_response(text)

    @patch("pxview_automation.client.urllib.request.urlopen")
    def test_connect_success(self, mock_urlopen):
        # Mock the three calls: initialize, notifications/initialized, tools/list
        mock_resp = MagicMock()
        mock_resp.read.return_value = json.dumps({
            "jsonrpc": "2.0", "id": 1,
            "result": {"protocolVersion": "2025-03-26"}
        }).encode()
        mock_resp.headers = {"Content-Type": "application/json"}
        mock_urlopen.return_value.__enter__.return_value = mock_resp

        client = McpClient(max_retries=1)
        client.connect()
        assert client.connected is True

    def test_disconnect(self):
        client = McpClient()
        client._connected = True
        client.disconnect()
        assert client.connected is False

    def test_parse_tool_result_error(self):
        client = McpClient()
        resp = {
            "jsonrpc": "2.0",
            "id": 1,
            "error": {"code": -32603, "message": "Internal error"},
        }
        with pytest.raises(McpError) as exc_info:
            client._parse_tool_result(resp)
        assert "Internal error" in str(exc_info.value)

    def test_parse_tool_result_is_error(self):
        client = McpClient()
        resp = {
            "jsonrpc": "2.0",
            "id": 1,
            "result": {
                "isError": True,
                "content": [{"text": "Device not found"}],
            },
        }
        with pytest.raises(McpError) as exc_info:
            client._parse_tool_result(resp)
        assert "Device not found" in str(exc_info.value)

    def test_parse_tool_result_json_content(self):
        client = McpClient()
        resp = {
            "jsonrpc": "2.0",
            "id": 1,
            "result": {
                "content": [{"text": '{"key": "value"}'}],
            },
        }
        result = client._parse_tool_result(resp)
        assert result == {"key": "value"}

    def test_parse_tool_result_text_content(self):
        client = McpClient()
        resp = {
            "jsonrpc": "2.0",
            "id": 1,
            "result": {
                "content": [{"text": "plain text"}],
            },
        }
        result = client._parse_tool_result(resp)
        assert result == "plain text"


# ======================================================================
# PXView (high-level, mocked)
# ======================================================================

class TestPXView:
    def test_init(self):
        pxv = PXView(host="localhost", port=10110)
        assert pxv._client.url == "http://localhost:10110/mcp"
        assert pxv.connected is False

    def test_context_manager(self):
        with PXView() as pxv:
            assert pxv is not None

    @patch("pxview_automation.client.McpClient.get_devices")
    def test_list_devices(self, mock_get):
        mock_get.return_value = [
            {"id": "demo", "is_demo": True, "is_hardware": False},
            {"id": "fx2lafw:0", "is_demo": False, "is_hardware": True},
        ]
        pxv = PXView()
        devices = pxv.list_devices()
        assert len(devices) == 2

    @patch("pxview_automation.client.McpClient.get_devices")
    def test_find_device_demo(self, mock_get):
        mock_get.return_value = [
            {"id": "demo", "is_demo": True, "is_hardware": False},
            {"id": "hw", "is_demo": False, "is_hardware": True},
        ]
        pxv = PXView()
        device = pxv.find_device(demo=True)
        assert device["id"] == "demo"

    @patch("pxview_automation.client.McpClient.get_devices")
    def test_find_device_hardware(self, mock_get):
        mock_get.return_value = [
            {"id": "demo", "is_demo": True, "is_hardware": False},
            {"id": "hw", "is_demo": False, "is_hardware": True, "driver_name": "fx2lafw"},
        ]
        pxv = PXView()
        device = pxv.find_device(hardware=True)
        assert device["id"] == "hw"

    @patch("pxview_automation.client.McpClient.get_devices")
    def test_find_device_not_found(self, mock_get):
        mock_get.return_value = []
        pxv = PXView()
        device = pxv.find_device(demo=True)
        assert device is None


# ======================================================================
# Exception hierarchy
# ======================================================================

class TestExceptions:
    def test_mcp_error_is_pxv_error(self):
        err = McpError("test")
        assert isinstance(err, PxvError)

    def test_mcp_connection_error_is_mcp_error(self):
        err = McpConnectionError("test")
        assert isinstance(err, McpError)
        assert isinstance(err, PxvError)

    def test_error_attributes(self):
        err = McpError("msg", code=42, raw={"a": 1})
        assert err.code == 42
        assert err.raw == {"a": 1}
        assert err.message == "msg"

    def test_error_repr(self):
        err = McpError("msg", code=42)
        assert "McpError" in repr(err)
        assert "42" in repr(err)


# ======================================================================
# Type system tests
# ======================================================================

class TestEnums:
    def test_capture_state_values(self):
        assert CaptureState.IDLE.value == "idle"
        assert CaptureState.CAPTURING.value == "capturing"
        assert CaptureState.COMPLETED.value == "completed"
        assert CaptureState.ERROR.value == "error"

    def test_channel_type_values(self):
        assert ChannelType.LOGIC.value == 0
        assert ChannelType.ANALOG.value == 1
        assert ChannelType.DSO.value == 2

    def test_digital_trigger_type_values(self):
        assert DigitalTriggerType.RISING.value == "rising"
        assert DigitalTriggerType.FALLING.value == "falling"
        assert DigitalTriggerType.PULSE_HIGH.value == "pulse_high"
        assert DigitalTriggerType.PULSE_LOW.value == "pulse_low"

    def test_radix_type_values(self):
        assert RadixType.HEXADECIMAL.value == "hex"
        assert RadixType.BINARY.value == "binary"

    def test_export_format_values(self):
        assert ExportFormat.CSV.value == "csv"
        assert ExportFormat.VCD.value == "vcd"

    def test_collect_mode_values(self):
        assert CollectMode.SINGLE.value == "single"
        assert CollectMode.REPEAT.value == "repeat"

    def test_coupling_type_values(self):
        assert CouplingType.DC.value == 0
        assert CouplingType.AC.value == 1

    def test_device_type_values(self):
        assert DeviceType.DEMO.value == "demo"
        assert DeviceType.FILE.value == "file"


class TestLogicDeviceConfiguration:
    def test_digital_only(self):
        cfg = LogicDeviceConfiguration(
            digital_channels=[0, 1],
            digital_sample_rate=1000000,
        )
        d = cfg.to_dict()
        assert d == {"digitalChannels": [0, 1], "digitalSampleRate": 1000000}

    def test_with_analog(self):
        cfg = LogicDeviceConfiguration(
            digital_channels=[0],
            analog_channels=[1],
            digital_sample_rate=1000000,
            analog_sample_rate=500000,
            digital_threshold_volts=1.8,
        )
        d = cfg.to_dict()
        assert d["analogChannels"] == [1]
        assert d["analogSampleRate"] == 500000
        assert d["digitalThresholdVolts"] == 1.8

    def test_with_glitch_filter(self):
        cfg = LogicDeviceConfiguration(
            digital_channels=[0],
            digital_sample_rate=1000000,
            glitch_filters=[
                GlitchFilterEntry(channel_index=0, pulse_width_seconds=0.0001),
            ],
        )
        d = cfg.to_dict()
        assert "glitchFilters" in d
        assert d["glitchFilters"][0]["channelIndex"] == 0

    def test_empty(self):
        cfg = LogicDeviceConfiguration()
        d = cfg.to_dict()
        assert d == {}


class TestCaptureConfiguration:
    def test_timed_mode(self):
        cap = CaptureConfiguration(
            capture_mode=TimedCaptureMode(duration_seconds=2.0),
        )
        d = cap.to_dict()
        assert "timedCaptureMode" in d
        assert d["timedCaptureMode"]["durationSeconds"] == 2.0

    def test_manual_mode(self):
        cap = CaptureConfiguration(
            capture_mode=ManualCaptureMode(sample_count=10000),
        )
        d = cap.to_dict()
        assert "manualCaptureMode" in d
        assert d["manualCaptureMode"]["sampleCount"] == 10000

    def test_digital_trigger_mode(self):
        cap = CaptureConfiguration(
            capture_mode=DigitalTriggerCaptureMode(
                trigger_type=DigitalTriggerType.RISING,
                trigger_channel_index=0,
                after_trigger_seconds=0.001,
            ),
        )
        d = cap.to_dict()
        assert "digitalCaptureMode" in d
        assert d["digitalCaptureMode"]["triggerType"] == "rising"
        assert d["digitalCaptureMode"]["triggerChannelIndex"] == 0

    def test_digital_trigger_with_linked(self):
        cap = CaptureConfiguration(
            capture_mode=DigitalTriggerCaptureMode(
                trigger_type=DigitalTriggerType.PULSE_HIGH,
                trigger_channel_index=0,
                linked_channels=[
                    DigitalTriggerLinkedChannel(
                        channel_index=1,
                        state=DigitalTriggerLinkedChannelState.HIGH,
                    ),
                ],
            ),
        )
        d = cap.to_dict()
        linked = d["digitalCaptureMode"]["linkedChannels"]
        assert len(linked) == 1
        assert linked[0]["channelIndex"] == 1
        assert linked[0]["state"] == "high"

    def test_buffer_size(self):
        cap = CaptureConfiguration(
            capture_mode=TimedCaptureMode(duration_seconds=1.0),
            buffer_size_megabytes=512,
        )
        d = cap.to_dict()
        assert d["bufferSizeMegabytes"] == 512


class TestGlitchFilterEntry:
    def test_with_samples(self):
        gf = GlitchFilterEntry(channel_index=2, pulse_width_samples=5)
        d = gf.to_dict()
        assert d == {"channelIndex": 2, "pulseWidthSamples": 5}

    def test_with_seconds(self):
        gf = GlitchFilterEntry(channel_index=0, pulse_width_seconds=0.001)
        d = gf.to_dict()
        assert d["pulseWidthSeconds"] == 0.001


class TestDeviceDesc:
    def test_from_dict_demo(self):
        d = DeviceDesc.from_dict({"id": "demo", "is_demo": True})
        assert d.id == "demo"
        assert d.is_demo is True
        assert d.device_type == DeviceType.DEMO

    def test_from_dict_hardware(self):
        d = DeviceDesc.from_dict({"id": "hw", "is_hardware": True, "driver_name": "fx2lafw"})
        assert d.is_hardware is True
        assert d.driver_name == "fx2lafw"


class TestChannelInfo:
    def test_from_dict(self):
        ch = ChannelInfo.from_dict({"index": 0, "name": "D0", "type": 0, "enabled": True})
        assert ch.index == 0
        assert ch.name == "D0"
        assert ch.type == ChannelType.LOGIC
        assert ch.enabled is True


class TestCaptureStatus:
    def test_from_dict_completed(self):
        s = CaptureStatus.from_dict({"state": "completed", "progress": 1.0})
        assert s.state == CaptureState.COMPLETED
        assert s.progress == 1.0

    def test_from_dict_unknown(self):
        s = CaptureStatus.from_dict({"state": "weird"})
        assert s.state == CaptureState.UNKNOWN


class TestVersion:
    def test_from_dict(self):
        v = Version.from_dict({"major": 1, "minor": 2, "patch": 3})
        assert str(v) == "1.2.3"

    def test_default(self):
        v = Version()
        assert str(v) == "0.0.0"


class TestSampleConfig:
    def test_from_dict(self):
        s = SampleConfig.from_dict({"sample_rate": 1000000, "sample_limit": 10000})
        assert s.sample_rate == 1000000
        assert s.sample_limit == 10000


class TestProbeConfig:
    def test_to_dict(self):
        p = ProbeConfig(vdiv=0.1, coupling=CouplingType.DC, vfactor=10)
        d = p.to_dict()
        assert d["vdiv"] == 0.1
        assert d["coupling"] == 0
        assert d["vfactor"] == 10

    def test_from_dict(self):
        p = ProbeConfig.from_dict({"vdiv": 0.5, "coupling": 1})
        assert p.vdiv == 0.5
        assert p.coupling == CouplingType.AC


class TestDataTableFilter:
    def test_to_dict(self):
        f = DataTableFilter(query="value > 100", columns=["data"])
        d = f.to_dict()
        assert d["query"] == "value > 100"
        assert d["columns"] == ["data"]


class TestDataTableExportConfiguration:
    def test_to_dict(self):
        from pxview_automation.types import AnalyzerHandle
        cfg = DataTableExportConfiguration(
            analyzer=AnalyzerHandle(analyzer_id="1:1"),
            radix=RadixType.HEXADECIMAL,
        )
        d = cfg.to_dict()
        assert d["analyzerId"] == "1:1"
        assert d["radixType"] == "hex"


class TestAnalyzerHandle:
    def test_str(self):
        from pxview_automation.types import AnalyzerHandle
        h = AnalyzerHandle(analyzer_id="2:3")
        assert str(h) == "2:3"
