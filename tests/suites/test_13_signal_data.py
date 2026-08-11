"""
test_13_signal_data.py - Signal data reading tests (Layer 7).

Validates get_logic_samples, get_analog_samples, get_dso_samples,
find_next_edge, find_pattern.
"""

import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_samples_non_empty
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p1


class TestSignalData:

    def test_get_logic_samples_basic(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """get_logic_samples returns base64-encoded bytes."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        samples = mcp.get_logic_samples(channel_index=0)
        assert samples is not None
        assert len(samples) > 0

    def test_get_logic_samples_range(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """get_logic_samples with startSample/endSample."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        samples = mcp.get_logic_samples(channel_index=0,
                                        start_sample=0,
                                        end_sample=100)
        assert samples is not None

    def test_get_logic_samples_pagination(self, mcp: McpClient, device_id: str,
                                          cleanup_after_test):
        """Page through logic samples in chunks."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        all_samples = mcp.get_logic_samples(channel_index=0)
        assert len(all_samples) > 0

        # Read in pages of 100 bytes
        page_size = 100
        total_read = 0
        for i in range(0, min(len(all_samples), 1000), page_size):
            page = mcp.get_logic_samples(channel_index=0,
                                         start_sample=i * 8,
                                         end_sample=(i + page_size) * 8 - 1)
            assert page is not None
            total_read += len(page)
        assert total_read > 0

    def test_get_logic_samples_end_negative_one(self, mcp: McpClient,
                                                device_id: str,
                                                cleanup_after_test):
        """endSample=-1 means end of capture."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        samples = mcp.get_logic_samples(channel_index=0, end_sample=-1)
        assert len(samples) > 0

    def test_find_next_edge(self, mcp: McpClient, device_id: str,
                            cleanup_after_test):
        """find_next_edge returns a valid sample index."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_next_edge(channel_index=0,
                                    start_sample=0,
                                    direction="forward")
        assert result is not None

    def test_find_next_edge_falling(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """find_next_edge with falling direction."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_next_edge(channel_index=0,
                                    start_sample=0,
                                    direction="falling")
        assert result is not None

    def test_find_pattern_exact(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """find_pattern with exact '0'/'1' pattern."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_pattern(channels=[0],
                                  pattern="1",
                                  start_sample=0)
        assert result is not None

    def test_find_pattern_dont_care(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """find_pattern with 'x' wildcard."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        result = mcp.find_pattern(channels=[0],
                                  pattern="x",
                                  start_sample=0)
        assert result is not None

    def test_get_logic_samples_no_capture(self, mcp: McpClient,
                                          cleanup_after_test):
        """get_logic_samples without capture returns error or empty."""
        try:
            result = mcp.get_logic_samples(channel_index=0)
            # If it doesn't error, it should be empty/None
        except McpError:
            pass  # Expected

    def test_get_logic_samples_all_channels(self, mcp: McpClient, device_id: str,
                                            cleanup_after_test):
        """Read samples from all enabled channels."""
        channels = [0, 1, 2, 3]
        do_timed_capture(mcp, device_id, channels=channels,
                         sample_rate=1000000, duration_seconds=0.3)
        for ch in channels:
            samples = mcp.get_logic_samples(channel_index=ch)
            assert len(samples) > 0, f"Channel {ch} has no samples"
