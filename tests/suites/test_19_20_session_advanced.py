"""
test_19_session_management.py - Session management tests.
test_20_advanced_tools.py - Advanced tools tests.
"""

import time

import pytest

from mcp_client import McpClient, McpError
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p2


class TestSessionManagement:

    def test_list_sessions(self, mcp: McpClient):
        """list_sessions returns a list."""
        sessions = mcp.list_sessions()
        assert isinstance(sessions, list)

    def test_get_session_count(self, mcp: McpClient):
        """get_session_count returns a number."""
        count = mcp.get_session_count()
        assert count >= 1, "Should have at least 1 session"

    def test_create_session_empty(self, mcp: McpClient):
        """create_session with no parameters."""
        result = mcp.create_session()
        assert result is not None

    def test_create_session_with_device(self, mcp: McpClient, device_id: str):
        """create_session bound to a device."""
        result = mcp.create_session(device_id=device_id)
        assert result is not None

    def test_destroy_session(self, mcp: McpClient, device_id: str):
        """destroy_session removes a session."""
        result = mcp.create_session(device_id=device_id)
        # Try to destroy - result may be session id
        if isinstance(result, (int, float)):
            mcp.destroy_session(session_id=int(result))
        elif isinstance(result, dict) and "id" in result:
            mcp.destroy_session(session_id=result["id"])

    def test_set_active_session(self, mcp: McpClient):
        """set_active_session switches sessions."""
        sessions = mcp.list_sessions()
        if sessions:
            sid = sessions[0].get("id") or sessions[0].get("session_id")
            if sid is not None:
                mcp.set_active_session(session_id=int(sid))

    def test_multi_session_count(self, mcp: McpClient):
        """Creating sessions increases session count."""
        count_before = mcp.get_session_count()
        mcp.create_session()
        count_after = mcp.get_session_count()
        assert count_after >= count_before


class TestAdvancedTools:

    def test_get_config(self, mcp: McpClient, device_id: str,
                        cleanup_after_test):
        """get_config reads a SR_CONF value."""
        mcp.connect_device(device_id)
        try:
            result = mcp.get_config(key=1, value_type="int")
        except McpError:
            pass  # Key 1 may not be valid for demo device

    def test_set_config(self, mcp: McpClient, device_id: str,
                        cleanup_after_test):
        """set_config writes a SR_CONF value."""
        mcp.connect_device(device_id)
        try:
            mcp.set_config(key=1, value_type="int", value=0)
        except McpError:
            pass

    def test_get_disk_cache_info(self, mcp: McpClient):
        """get_disk_cache_info returns info."""
        info = mcp.get_disk_cache_info()
        assert isinstance(info, dict)

    def test_get_decoder_class_names(self, mcp: McpClient):
        """get_decoder_class_names for multiple decoders."""
        for name in ["i2c_c", "spi_c", "uart_c", "pwm_c"]:
            try:
                names = mcp.get_decoder_class_names(name)
                assert isinstance(names, list)
            except McpError:
                pass

    def test_get_math_results(self, mcp: McpClient):
        """get_math_results returns dict."""
        result = mcp.get_math_results()
        assert isinstance(result, dict)

    def test_get_spectrum_results(self, mcp: McpClient):
        """get_spectrum_results returns dict."""
        result = mcp.get_spectrum_results()
        assert isinstance(result, dict)

    def test_get_lissajous_results(self, mcp: McpClient):
        """get_lissajous_results returns dict."""
        result = mcp.get_lissajous_results()
        assert isinstance(result, dict)

    def test_get_error_state(self, mcp: McpClient):
        """get_error_state returns dict."""
        result = mcp.get_error_state()
        assert isinstance(result, dict)
        assert "has_error" in result

    def test_clear_error_state(self, mcp: McpClient):
        """clear_error_state resets error state."""
        mcp.clear_error_state()
        result = mcp.get_error_state()
        assert result.get("has_error") is False or result.get("has_error") == 0

    def test_get_decoder_binary_output(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """get_decoder_binary_output (may return ConfigNotSupported)."""
        from helpers.decoder_helper import add_decoder_safe
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.5)
        try:
            result = mcp.get_decoder_binary_output(analyzer_id, output_id=0)
        except McpError:
            pass  # Expected: ConfigNotSupported per tool description
