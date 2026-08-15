"""
test_30_pxl_format.py - .pxl format save/load tests.

Validates that the .pxl format (the product default session file format)
works identically to .pxc for save/load round-trips.

Tests:
1. .pxl save → load → signal data is byte-identical (ch0 + ch1)
2. .pxl save → load → all channels preserved
3. .pxl save → load → sample rate preserved
"""

import os
import time

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture
from helpers.export_helper import compare_logic_samples

pytestmark = pytest.mark.p0


class TestPxlFormat:

    def test_pxl_save_load_identical(self, mcp: McpClient, device_id: str,
                                     tmp_pxl_file: str,
                                     cleanup_after_test):
        """Capture → save as .pxl → load → data is byte-identical."""
        # 1. Capture
        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1, 2, 3],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert_capture_status(status, "completed")

        # 2. Get original samples
        original_ch0 = mcp.get_samples(channel_type="logic", channel_index=0)
        original_ch1 = mcp.get_samples(channel_type="logic", channel_index=1)
        assert len(original_ch0) > 0
        assert len(original_ch1) > 0

        # 3. Save to .pxl
        mcp.save_capture(tmp_pxl_file)
        assert os.path.exists(tmp_pxl_file), \
            f".pxl file not created: {tmp_pxl_file}"
        assert os.path.getsize(tmp_pxl_file) > 0, \
            f".pxl file is empty: {tmp_pxl_file}"

        # 4. Close current capture
        mcp.close_capture()

        # 5. Load from .pxl
        mcp.load_capture(tmp_pxl_file)
        time.sleep(1)  # Allow load to complete

        # 6. Get loaded samples
        loaded_ch0 = mcp.get_samples(channel_type="logic", channel_index=0)
        loaded_ch1 = mcp.get_samples(channel_type="logic", channel_index=1)

        # 7. Verify data integrity
        assert compare_logic_samples(original_ch0, loaded_ch0), \
            f"Channel 0 data mismatch after .pxl save/load: " \
            f"original={len(original_ch0)} bytes, loaded={len(loaded_ch0)} bytes"
        assert compare_logic_samples(original_ch1, loaded_ch1), \
            f"Channel 1 data mismatch after .pxl save/load: " \
            f"original={len(original_ch1)} bytes, loaded={len(loaded_ch1)} bytes"

    def test_pxl_save_load_all_channels(self, mcp: McpClient, device_id: str,
                                        tmp_pxl_file: str,
                                        cleanup_after_test):
        """Save → load .pxl preserves data on all channels."""
        channels = [0, 1, 2, 3]
        status = do_timed_capture(mcp, device_id,
                                  channels=channels,
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status, "completed")

        originals = {}
        for ch in channels:
            originals[ch] = mcp.get_samples(channel_type="logic", channel_index=ch)
            assert len(originals[ch]) > 0, f"Channel {ch} no data"

        mcp.save_capture(tmp_pxl_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxl_file)
        time.sleep(1)

        for ch in channels:
            loaded = mcp.get_samples(channel_type="logic", channel_index=ch)
            assert compare_logic_samples(originals[ch], loaded), \
                f"Channel {ch} data mismatch after .pxl save/load"

    def test_pxl_save_load_preserves_sample_rate(self, mcp: McpClient,
                                                 device_id: str,
                                                 tmp_pxl_file: str,
                                                 cleanup_after_test):
        """Save → load .pxl preserves sample rate configuration."""
        rate = 1000000
        do_timed_capture(mcp, device_id,
                         channels=[0],
                         sample_rate=rate,
                         duration_seconds=0.5)

        config_before = mcp.get_sample_config()
        mcp.save_capture(tmp_pxl_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxl_file)
        time.sleep(1)
        config_after = mcp.get_sample_config()

        assert config_before.get("sample_rate") == config_after.get("sample_rate"), \
            f"Sample rate changed after .pxl save/load: " \
            f"{config_before.get('sample_rate')} -> {config_after.get('sample_rate')}"
