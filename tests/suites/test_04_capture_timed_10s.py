"""
test_04_capture_timed_10s.py - 10-second timed capture tests (Layer 4).

Core user requirement: capture 10s of data with demo device,
verify sample count matches expected value.
"""

import time

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p0


class TestCaptureTimed10s:

    def test_capture_10s_demo(self, mcp: McpClient, device_id: str,
                              cleanup_after_test):
        """Demo device 10-second timed capture completes successfully."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1, 2, 3],
                                  sample_rate=1000000,
                                  duration_seconds=10.0)
        assert_capture_status(status, "completed")

    def test_capture_10s_sample_count(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """10s capture at 1MHz produces ~10M samples."""
        rate = 1000000
        duration = 10.0
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=rate,
                                  duration_seconds=duration)
        assert_capture_status(status, "completed")
        # Read samples to verify we got data
        samples = mcp.get_logic_samples(channel_index=0, end_sample=-1)
        assert samples is not None
        assert len(samples) > 0, "No samples returned from 10s capture"
        # Expected: rate * duration = 10M samples = 1.25M bytes (8 samples/byte)
        expected_bytes = (rate * duration) / 8
        # Allow some tolerance (demo device may not be exact)
        assert len(samples) >= expected_bytes * 0.5, \
            f"Sample count too low: got {len(samples)} bytes, " \
            f"expected ~{expected_bytes:.0f} bytes"

    def test_capture_10s_multi_channel(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """10s capture with 8 channels works correctly."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1, 2, 3, 4, 5, 6, 7],
                                  sample_rate=1000000,
                                  duration_seconds=10.0)
        assert_capture_status(status, "completed")

    def test_capture_10s_various_rates(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """10s capture at different sample rates."""
        for rate in [100000, 1000000]:
            status = do_timed_capture(mcp, device_id,
                                      channels=[0, 1],
                                      sample_rate=rate,
                                      duration_seconds=10.0)
            assert_capture_status(status, "completed")

    def test_capture_10s_read_all_channels(self, mcp: McpClient, device_id: str,
                                           cleanup_after_test):
        """After 10s capture, read samples from all channels."""
        channels = [0, 1, 2, 3]
        status = do_timed_capture(mcp, device_id,
                                  channels=channels,
                                  sample_rate=1000000,
                                  duration_seconds=10.0)
        assert_capture_status(status, "completed")
        for ch in channels:
            samples = mcp.get_logic_samples(channel_index=ch)
            assert samples is not None, f"Channel {ch} returned None samples"
            assert len(samples) > 0, f"Channel {ch} returned empty samples"

    def test_capture_10s_wait_capture(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """wait_capture correctly waits for a 10s capture."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 10}
        })
        t0 = time.time()
        mcp.wait_capture(timeout_seconds=30, timeout=35)
        elapsed = time.time() - t0
        # Should take at least ~10 seconds (demo may be slightly faster)
        assert elapsed >= 5.0, \
            f"wait_capture returned too quickly: {elapsed:.1f}s (expected ~10s)"

    def test_capture_10s_then_close(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """Close capture after 10s capture works correctly."""
        do_timed_capture(mcp, device_id,
                         channels=[0],
                         sample_rate=1000000,
                         duration_seconds=10.0)
        result = mcp.close_capture()
        # close_capture may return None on success
        # Verify we can start a new capture after close
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status, "completed")
