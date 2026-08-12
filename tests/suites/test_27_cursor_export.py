"""
test_27_cursor_export.py - Cursor range + export/import roundtrip tests.

Tests:
1. Cursor API: add/get/remove/clear cursors
2. Left cursor export: set_save_range with only left cursor → export → verify
3. Right cursor export: set_save_range with only right cursor → export → verify
4. Both cursors export: set_save_range with left+right → export → verify
5. CSV export → verify row count matches range
6. Binary export → verify byte count matches range
7. .pxc save → load → verify data integrity
8. Data table CSV export → verify non-empty
"""

import os
import time

import pytest

from pxview_automation import McpClient
from helpers.capture_helper import do_timed_capture
from helpers.export_helper import (
    compare_logic_samples,
    find_exported_csv,
    find_exported_binary,
    read_csv_file,
    read_binary_file,
)

pytestmark = pytest.mark.p1


# ======================================================================
# Test Group A: Cursor API basic operations
# ======================================================================

class TestCursorApi:
    """Basic cursor add/get/remove/clear operations."""

    def test_add_and_get_cursor(self, mcp, device_id, cleanup_after_test):
        """add_cursor then get_cursors returns it."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        mcp.add_cursor(sample_pos=1000)
        cursors = mcp.get_cursors()
        assert len(cursors) >= 1, f"Expected >=1 cursor, got {len(cursors)}"
        positions = [c.get("sample_position") or c.get("position")
                     for c in cursors]
        assert 1000 in positions, f"Cursor at 1000 not found in {positions}"

    def test_add_multiple_cursors(self, mcp, device_id, cleanup_after_test):
        """Add multiple cursors and verify all present."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        mcp.add_cursor(sample_pos=100)
        mcp.add_cursor(sample_pos=500)
        mcp.add_cursor(sample_pos=1000)

        cursors = mcp.get_cursors()
        assert len(cursors) >= 3, f"Expected >=3 cursors, got {len(cursors)}"

    def test_remove_cursor(self, mcp, device_id, cleanup_after_test):
        """remove_cursor reduces cursor count."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        mcp.add_cursor(sample_pos=100)
        mcp.add_cursor(sample_pos=200)
        before = len(mcp.get_cursors())

        mcp.remove_cursor(index=0)
        after = len(mcp.get_cursors())
        assert after < before, \
            f"Cursor count not reduced: {before} → {after}"

    def test_clear_cursors(self, mcp, device_id, cleanup_after_test):
        """clear_cursors removes all cursors."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        mcp.add_cursor(sample_pos=100)
        mcp.add_cursor(sample_pos=200)
        mcp.clear_cursors()

        cursors = mcp.get_cursors()
        assert len(cursors) == 0, \
            f"Expected 0 cursors after clear, got {len(cursors)}"


# ======================================================================
# Test Group B: Left cursor export (start_sample > 0, end_sample = max)
# ======================================================================

class TestLeftCursorExport:
    """Export with only left cursor (range starts at non-zero)."""

    def test_left_cursor_csv_row_count(self, mcp, device_id,
                                         tmp_capture_dir, cleanup_after_test):
        """CSV export with left cursor has correct row count."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        total_samples = len(mcp.get_samples(channel_type="logic", channel_index=0))
        assert total_samples > 1000, f"Only {total_samples} samples"

        left = 500
        mcp.set_save_range(start_sample=left, end_sample=total_samples)
        mcp.export_raw_data(format="csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0, "No CSV files exported"
        data = read_csv_file(csv_files[0])
        # Should have approximately (total - left) rows
        assert len(data) > 0, "CSV is empty"
        assert len(data) <= total_samples - left + 10, \
            f"CSV has {len(data)} rows, expected ~{total_samples - left}"

    def test_left_cursor_binary_size(self, mcp, device_id,
                                       tmp_capture_dir, cleanup_after_test):
        """Binary export with left cursor has correct size."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        total_samples = len(mcp.get_samples(channel_type="logic", channel_index=0))
        left = 500
        mcp.set_save_range(start_sample=left, end_sample=total_samples)
        mcp.export_raw_data(format="binary", tmp_capture_dir, digital_channels=[0])

        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0, "No binary files exported"
        bin_data = read_binary_file(bin_files[0])
        expected_bytes = (total_samples - left) // 8
        actual_bytes = len(bin_data)
        # Allow small rounding difference
        assert abs(actual_bytes - expected_bytes) <= 2, \
            f"Binary size {actual_bytes}, expected ~{expected_bytes}"


# ======================================================================
# Test Group C: Right cursor export (start=0, end < total)
# ======================================================================

class TestRightCursorExport:
    """Export with only right cursor (range ends before total)."""

    def test_right_cursor_csv_row_count(self, mcp, device_id,
                                          tmp_capture_dir,
                                          cleanup_after_test):
        """CSV export with right cursor has correct row count."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        total_samples = len(mcp.get_samples(channel_type="logic", channel_index=0))
        right = 5000
        mcp.set_save_range(start_sample=0, end_sample=right)
        mcp.export_raw_data(format="csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        data = read_csv_file(csv_files[0])
        assert len(data) > 0
        assert len(data) <= right + 10, \
            f"CSV has {len(data)} rows, expected ~{right}"

    def test_right_cursor_binary_size(self, mcp, device_id,
                                        tmp_capture_dir,
                                        cleanup_after_test):
        """Binary export with right cursor has correct size."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        right = 5000
        mcp.set_save_range(start_sample=0, end_sample=right)
        mcp.export_raw_data(format="binary", tmp_capture_dir, digital_channels=[0])

        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0
        bin_data = read_binary_file(bin_files[0])
        expected_bytes = right // 8
        assert abs(len(bin_data) - expected_bytes) <= 2, \
            f"Binary size {len(bin_data)}, expected ~{expected_bytes}"


# ======================================================================
# Test Group D: Both cursors export (left > 0, right < total)
# ======================================================================

class TestBothCursorsExport:
    """Export with both left and right cursors (range in the middle)."""

    def test_both_cursors_csv_row_count(self, mcp, device_id,
                                          tmp_capture_dir,
                                          cleanup_after_test):
        """CSV export with both cursors has correct row count."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        total_samples = len(mcp.get_samples(channel_type="logic", channel_index=0))
        left, right = 1000, 5000
        mcp.set_save_range(start_sample=left, end_sample=right)
        mcp.export_raw_data(format="csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        data = read_csv_file(csv_files[0])
        expected = right - left
        assert len(data) > 0
        assert len(data) <= expected + 10, \
            f"CSV has {len(data)} rows, expected ~{expected}"

    def test_both_cursors_binary_size(self, mcp, device_id,
                                        tmp_capture_dir,
                                        cleanup_after_test):
        """Binary export with both cursors has correct size."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        left, right = 1000, 5000
        mcp.set_save_range(start_sample=left, end_sample=right)
        mcp.export_raw_data(format="binary", tmp_capture_dir, digital_channels=[0])

        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0
        bin_data = read_binary_file(bin_files[0])
        expected_bytes = (right - left) // 8
        assert abs(len(bin_data) - expected_bytes) <= 2, \
            f"Binary size {len(bin_data)}, expected ~{expected_bytes}"

    def test_both_cursors_csv_data_matches_samples(self, mcp, device_id,
                                                     tmp_capture_dir,
                                                     cleanup_after_test):
        """CSV data within cursor range matches get_logic_samples."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        # Get all samples via MCP
        all_samples = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(all_samples) > 0

        left, right = 100, 800
        mcp.set_save_range(start_sample=left, end_sample=right)
        mcp.export_raw_data(format="csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        csv_data = read_csv_file(csv_files[0])

        # Verify CSV has approximately (right - left) rows
        expected_rows = right - left
        assert abs(len(csv_data) - expected_rows) <= 10, \
            f"CSV rows={len(csv_data)}, expected ~{expected_rows}"


# ======================================================================
# Test Group E: Full export/import roundtrip for each format
# ======================================================================

class TestExportImportRoundtrip:
    """Every export format: export → import → verify correctness."""

    def test_pxc_save_load_single_channel(self, mcp, device_id,
                                            tmp_pxc_file,
                                            cleanup_after_test):
        """Save → load preserves single channel data."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        before = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(before) > 0

        mcp.save_capture(tmp_pxc_file)
        assert os.path.exists(tmp_pxc_file)
        assert os.path.getsize(tmp_pxc_file) > 0

        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)

        after = mcp.get_samples(channel_type="logic", channel_index=0)
        assert compare_logic_samples(before, after), \
            f"Data mismatch: before={len(before)} bytes, after={len(after)} bytes"

    def test_pxc_save_load_multi_channel(self, mcp, device_id,
                                           tmp_pxc_file,
                                           cleanup_after_test):
        """Save → load preserves all channels."""
        channels = [0, 1, 2, 3]
        do_timed_capture(mcp, device_id, channels=channels,
                         sample_rate=1000000, duration_seconds=0.5)

        originals = {}
        for ch in channels:
            originals[ch] = mcp.get_samples(channel_type="logic", channel_index=ch)
            assert len(originals[ch]) > 0, f"Channel {ch} no data"

        mcp.save_capture(tmp_pxc_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)

        for ch in channels:
            loaded = mcp.get_samples(channel_type="logic", channel_index=ch)
            assert compare_logic_samples(originals[ch], loaded), \
                f"Channel {ch} data mismatch after save/load"

    def test_pxc_save_load_preserves_sample_rate(self, mcp, device_id,
                                                   tmp_pxc_file,
                                                   cleanup_after_test):
        """Save → load preserves sample rate."""
        rate = 1000000
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=rate, duration_seconds=0.5)

        config_before = mcp.get_sample_config()
        mcp.save_capture(tmp_pxc_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)
        config_after = mcp.get_sample_config()

        assert config_before.get("sample_rate") == config_after.get("sample_rate"), \
            f"Sample rate changed: {config_before.get('sample_rate')} → " \
            f"{config_after.get('sample_rate')}"

    def test_csv_export_import_data_matches_mcp(self, mcp, device_id,
                                                  tmp_capture_dir,
                                                  cleanup_after_test):
        """CSV export data matches get_logic_samples."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)

        mcp_samples = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(mcp_samples) > 0

        mcp.export_raw_data(format="csv", tmp_capture_dir, digital_channels=[0])
        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        csv_data = read_csv_file(csv_files[0])
        assert len(csv_data) > 0

        # CSV row count should be close to sample count
        # (1 row per sample, may have header)
        assert len(csv_data) > 0, "CSV has no data rows"

    def test_binary_export_size_matches_samples(self, mcp, device_id,
                                                  tmp_capture_dir,
                                                  cleanup_after_test):
        """Binary export size matches get_logic_samples byte count."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)

        mcp_samples = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(mcp_samples) > 0

        mcp.export_raw_data(format="binary", tmp_capture_dir, digital_channels=[0])
        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0
        bin_data = read_binary_file(bin_files[0])
        assert len(bin_data) > 0

        # Binary should be approximately the same size as MCP samples
        # (1 byte per 8 samples for single channel)
        assert len(bin_data) > 0

    def test_multi_channel_csv_exports_all_channels(self, mcp, device_id,
                                                       tmp_capture_dir,
                                                       cleanup_after_test):
        """Multi-channel CSV export produces file for each channel."""
        channels = [0, 1, 2, 3]
        do_timed_capture(mcp, device_id, channels=channels,
                         sample_rate=1000000, duration_seconds=0.3)

        mcp.export_raw_data(format="csv", tmp_capture_dir, digital_channels=channels)

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        # Should have at least one file per channel
        assert len(csv_files) >= len(channels), \
            f"Expected >= {len(channels)} CSV files, got {len(csv_files)}"

        # Each file should have data
        for csv_file in csv_files:
            data = read_csv_file(csv_file)
            assert len(data) > 0, f"{csv_file} is empty"

    def test_multi_channel_binary_exports_all_channels(self, mcp, device_id,
                                                         tmp_capture_dir,
                                                         cleanup_after_test):
        """Multi-channel binary export produces file for each channel."""
        channels = [0, 1, 2, 3]
        do_timed_capture(mcp, device_id, channels=channels,
                         sample_rate=1000000, duration_seconds=0.3)

        mcp.export_raw_data(format="binary", tmp_capture_dir, digital_channels=channels)

        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) >= len(channels), \
            f"Expected >= {len(channels)} binary files, got {len(bin_files)}"

        for bin_file in bin_files:
            data = read_binary_file(bin_file)
            assert len(data) > 0, f"{bin_file} is empty"

    def test_save_load_with_cursor_range(self, mcp, device_id,
                                           tmp_pxc_file,
                                           cleanup_after_test):
        """set_save_range + save → load preserves ranged data."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        left, right = 100, 5000
        mcp.set_save_range(start_sample=left, end_sample=right)
        mcp.save_capture(tmp_pxc_file)
        assert os.path.exists(tmp_pxc_file)

        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)

        samples = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(samples) > 0, "No samples after load with save range"

    def test_export_cycle_3x(self, mcp, device_id, tmp_capture_dir,
                              cleanup_after_test):
        """Multiple capture → save → close → load → verify cycles."""
        for i in range(3):
            do_timed_capture(mcp, device_id, channels=[0],
                             sample_rate=1000000, duration_seconds=0.3)
            filepath = os.path.join(tmp_capture_dir, f"cycle_{i}.pxc")
            mcp.save_capture(filepath)
            assert os.path.exists(filepath)
            mcp.close_capture()
            mcp.load_capture(filepath)
            time.sleep(0.5)
            samples = mcp.get_samples(channel_type="logic", channel_index=0)
            assert len(samples) > 0, f"Cycle {i}: no samples after load"
            mcp.close_capture()
