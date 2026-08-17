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
from helpers.capture_helper import (
    do_timed_capture,
    do_buffer_capture_with_pattern,
)
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

    def test_pxl_range_save_load(self, mcp: McpClient, device_id,
                                 tmp_pxl_file,
                                 ensure_device_connected,
                                 cleanup_after_test):
        """Ranged .pxl save (via set_export_config) → load restores the range.

        Regression test: save_capture used to ignore the save range entirely
        (always writing the whole capture), and the header 'total samples'
        was the full-capture count even when a range was saved.  Both made
        ranged .pxl saves report/display a wrong length (range abnormal).
        """
        import zipfile as _zipfile

        def align_down(x: int) -> int:
            return x - (x % 64)

        def align_up(x: int) -> int:
            return x + ((64 - x % 64) % 64)

        status = do_buffer_capture_with_pattern(
            mcp, device_id, channels=[0, 1],
            sample_rate=1000000, sample_count=1000000, pattern="i2c")
        assert_capture_status(status, "completed")

        orig = mcp.get_samples(channel_type="logic", channel_index=0)
        total = len(orig)
        assert total > 20000, f"Only {total} samples captured"

        left, right = 5000, 8000
        mcp.set_export_config(start_sample=left, end_sample=right)
        mcp.save_capture(tmp_pxl_file)
        assert os.path.exists(tmp_pxl_file)

        # Header "total samples" must match the 64-aligned saved range
        with _zipfile.ZipFile(tmp_pxl_file) as zf:
            header = zf.read("header").decode("utf-8", errors="replace")
        header_samples = None
        for line in header.splitlines():
            if "total samples" in line:
                header_samples = int(line.split("=", 1)[1].strip())
        expected = align_up(right) - align_down(left)
        assert header_samples == expected, \
            f"header total samples {header_samples} != range {expected}"

        mcp.close_capture()
        mcp.load_capture(tmp_pxl_file)
        time.sleep(1)
        loaded = mcp.get_samples(channel_type="logic", channel_index=0)
        assert abs(len(loaded) - expected) <= 8, \
            f"loaded {len(loaded)} != expected ~{expected}"

        # Data integrity: get_samples returns the packed per-channel bitmap
        # (8 samples/byte, length == sample count), so the 64-aligned saved
        # range [al, ar) maps to bitmap bytes [al//8, ar//8).
        al, ar = align_down(left), align_up(right)
        b0, b1 = al // 8, ar // 8
        assert loaded[:b1 - b0] == orig[b0:b1], \
            "loaded range data does not match original capture slice"

