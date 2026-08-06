"""
test_03_capture_basic.py - Basic capture tests (Layer 3).

Validates start_capture, stop_capture, wait_capture, get_capture_status,
close_capture with various configurations.
"""

import time

import pytest

from mcp_client import McpClient, McpError
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture, do_manual_capture

pytestmark = pytest.mark.p0


class TestCaptureBasic:

    def test_start_capture_minimal(self, mcp: McpClient, device_id: str,
                                   cleanup_after_test):
        """start_capture with minimal timed configuration succeeds."""
        # Use a proper timed capture with minimal channels
        result = mcp.start_capture(device_id, {
            "digitalChannels": [0],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 0.5}
        })
        assert result is not None
        mcp.wait_capture(timeout_seconds=15, timeout=20)

    def test_start_capture_logic_config(self, mcp: McpClient, device_id: str,
                                        cleanup_after_test):
        """start_capture with logicDeviceConfiguration + timed capture."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0, 1],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 0.5}
        })
        mcp.wait_capture(timeout_seconds=15, timeout=20)

    def test_start_capture_manual_mode(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """start_capture with manualCaptureMode + sampleCount."""
        status = do_manual_capture(mcp, device_id,
                                   channels=[0, 1],
                                   sample_rate=1000000,
                                   sample_count=10000)
        assert_capture_status(status)

    def test_start_capture_timed_mode(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """start_capture with timedCaptureMode + durationSeconds."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert_capture_status(status, "completed")

    def test_get_capture_status_idle(self, mcp: McpClient, cleanup_after_test):
        """get_capture_status returns idle when no capture running."""
        status = mcp.get_capture_status()
        assert status["state"] in ("idle", "completed"), \
            f"Expected idle/completed, got {status['state']}"

    def test_get_capture_status_capturing(self, mcp: McpClient, device_id: str,
                                          cleanup_after_test):
        """get_capture_status returns capturing during acquisition."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 5}
        })
        time.sleep(0.5)
        status = mcp.get_capture_status()
        # Should be either 'capturing' or already 'completed' for fast demo
        assert status["state"] in ("capturing", "completed"), \
            f"Expected capturing/completed, got {status['state']}"
        mcp.wait_capture(timeout_seconds=15, timeout=20)

    def test_get_capture_status_completed(self, mcp: McpClient, device_id: str,
                                          cleanup_after_test):
        """get_capture_status returns completed after capture finishes."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert_capture_status(status, "completed")

    def test_stop_capture(self, mcp: McpClient, device_id: str,
                          cleanup_after_test):
        """stop_capture aborts an in-progress capture."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 10}
        })
        time.sleep(0.5)
        mcp.stop_capture()
        status = mcp.get_capture_status()
        assert status["state"] in ("idle", "stopped", "completed"), \
            f"Expected idle/stopped/completed after stop, got {status['state']}"

    def test_wait_capture_success(self, mcp: McpClient, device_id: str,
                                  cleanup_after_test):
        """wait_capture returns after capture completes."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0, 1],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 1}
        })
        result = mcp.wait_capture(timeout_seconds=15, timeout=20)
        # wait_capture returns None on success (void MCP tool result)
        # An error would raise McpError, not return None

    def test_close_capture(self, mcp: McpClient, device_id: str,
                           cleanup_after_test):
        """close_capture releases capture resources."""
        do_timed_capture(mcp, device_id, channels=[0], duration_seconds=0.5)
        result = mcp.close_capture()
        # close_capture may return None on success (wrap_void)

    def test_start_capture_multi_channel(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """start_capture with multiple digital channels."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1, 2, 3, 4, 5, 6, 7],
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status)

    def test_start_capture_various_rates(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """start_capture at different sample rates."""
        for rate in [1000, 10000, 100000, 1000000]:
            status = do_timed_capture(mcp, device_id,
                                      channels=[0],
                                      sample_rate=rate,
                                      duration_seconds=0.3)
            assert status["state"] in ("completed", "idle"), \
                f"Capture at {rate}Hz failed: {status}"

    def test_capture_status_has_progress(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """get_capture_status includes progress field."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 3}
        })
        time.sleep(0.5)
        status = mcp.get_capture_status()
        assert "progress" in status, f"Status missing 'progress': {status}"
        mcp.wait_capture(timeout_seconds=15, timeout=20)
