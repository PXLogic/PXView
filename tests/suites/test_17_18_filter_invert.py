"""
test_17_18_filter_invert.py - Glitch filter and signal invert tests.
"""

import pytest

from pxview_automation import McpClient, McpError
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p1


class TestGlitchFilter:

    def test_set_glitch_filter(self, mcp: McpClient, device_id: str,
                               cleanup_after_test):
        """set_glitch_filter enables filter on channels."""
        mcp.connect_device(device_id)
        mcp.configure_glitch_filter(channels=[0], thresholds=[10])

    def test_get_glitch_filter_config(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """get_glitch_filter_config returns config."""
        mcp.connect_device(device_id)
        config = mcp.configure_glitch_filter()
        assert isinstance(config, dict)

    def test_clear_glitch_filter(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """clear_glitch_filter removes filter."""
        mcp.connect_device(device_id)
        mcp.configure_glitch_filter(channels=[0], thresholds=[10])
        mcp.configure_glitch_filter(channels=[])

    def test_glitch_filter_per_channel(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """Per-channel thresholds and modes."""
        mcp.connect_device(device_id)
        mcp.configure_glitch_filter(channels=[0, 1],
                              thresholds=[5, 10],
                              modes=[0, 1])

    def test_glitch_filter_with_capture(self, mcp: McpClient, device_id: str,
                                        cleanup_after_test):
        """Glitch filter doesn't break capture."""
        mcp.connect_device(device_id)
        mcp.configure_glitch_filter(channels=[0], thresholds=[5])
        status = do_timed_capture(mcp, device_id, channels=[0],
                                  sample_rate=1000000, duration_seconds=0.3)
        assert status["state"] in ("completed", "idle")
        mcp.configure_glitch_filter(channels=[])


class TestSignalInvert:

    def test_set_signal_invert(self, mcp: McpClient, device_id: str,
                               cleanup_after_test):
        """set_signal_invert inverts channels."""
        mcp.connect_device(device_id)
        mcp.configure_signal_invert(channels=[0])

    def test_get_signal_invert_config(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """get_signal_invert_config returns config."""
        mcp.connect_device(device_id)
        config = mcp.configure_signal_invert()
        assert isinstance(config, dict)

    def test_clear_signal_invert(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """clear_signal_invert removes inversion."""
        mcp.connect_device(device_id)
        mcp.configure_signal_invert(channels=[0])
        mcp.configure_signal_invert(channels=[])

    def test_signal_invert_multi_channel(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """Signal invert on multiple channels."""
        mcp.connect_device(device_id)
        mcp.configure_signal_invert(channels=[0, 1, 2])
        mcp.configure_signal_invert(channels=[])

    def test_signal_invert_with_capture(self, mcp: McpClient, device_id: str,
                                        cleanup_after_test):
        """Signal invert doesn't break capture."""
        mcp.connect_device(device_id)
        mcp.configure_signal_invert(channels=[0])
        status = do_timed_capture(mcp, device_id, channels=[0],
                                  sample_rate=1000000, duration_seconds=0.3)
        assert status["state"] in ("completed", "idle")
        mcp.configure_signal_invert(channels=[])
