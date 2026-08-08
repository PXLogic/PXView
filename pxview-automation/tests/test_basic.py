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
