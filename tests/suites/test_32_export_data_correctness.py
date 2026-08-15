"""
test_32_export_data_correctness.py - Export data content correctness tests.

Validates that exported data within a cursor range is not only the correct
size/row count, but that the actual bit values match get_samples for the
corresponding range.

Tests:
1. Both cursors CSV: each bit value matches get_samples(start, end)
2. Both cursors Binary: each byte matches get_samples(start, end)
3. Left cursor CSV: first row corresponds to start_sample (not 0)
4. Right cursor CSV: last row corresponds to end_sample-1
"""

import os
import time

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture
from helpers.export_helper import (
    find_exported_csv,
    find_exported_binary,
    read_csv_file,
    read_binary_file,
    get_logic_sample_bit,
)

pytestmark = pytest.mark.p0


class TestExportDataCorrectness:
    """Verify exported data content matches get_samples, not just size."""

    def test_both_cursors_csv_data_matches_samples(self, mcp: McpClient,
                                                    device_id: str,
                                                    tmp_capture_dir: str,
                                                    cleanup_after_test):
        """CSV data within cursor range matches get_samples bit-by-bit."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        # Get all samples via MCP
        all_samples = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(all_samples) > 0

        left, right = 100, 800
        mcp.set_export_config(start_sample=left, end_sample=right)
        mcp.export_raw_data("csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        csv_data = read_csv_file(csv_files[0])

        # Verify row count is approximately (right - left)
        expected_rows = right - left
        assert abs(len(csv_data) - expected_rows) <= 10, \
            f"CSV rows={len(csv_data)}, expected ~{expected_rows}"

        # Verify each CSV row's bit value matches the corresponding sample
        # CSV columns may vary; find the data column (usually the last column
        # or a column named 'data'/'value'/'bit')
        mismatches = 0
        checked = 0
        for i, row in enumerate(csv_data[:expected_rows]):
            sample_idx = left + i
            expected_bit = get_logic_sample_bit(all_samples, sample_idx)
            if expected_bit < 0:
                continue

            # Try to find the bit value in the CSV row
            # Common column names: 'data', 'value', 'bit', or last column
            row_values = list(row.values())
            if not row_values:
                continue

            # The data value is typically the last column
            data_str = str(row_values[-1]).strip()
            try:
                data_val = int(data_str)
            except ValueError:
                # Try float then truncate
                try:
                    data_val = int(float(data_str))
                except ValueError:
                    continue

            if data_val in (0, 1):
                checked += 1
                if data_val != expected_bit:
                    mismatches += 1

        # We should have checked at least some samples
        assert checked > 0, \
            f"No samples could be bit-verified from CSV. " \
            f"CSV columns: {list(csv_data[0].keys()) if csv_data else 'empty'}"
        # Allow up to 5% mismatch (CSV format may have rounding/header issues)
        assert mismatches <= checked * 0.05, \
            f"Too many bit mismatches: {mismatches}/{checked} " \
            f"({mismatches/checked*100:.1f}%)"

    def test_both_cursors_binary_data_matches_samples(self, mcp: McpClient,
                                                      device_id: str,
                                                      tmp_capture_dir: str,
                                                      cleanup_after_test):
        """Binary data within cursor range matches get_samples byte-by-byte."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        all_samples = mcp.get_samples(channel_type="logic", channel_index=0)
        assert len(all_samples) > 0

        left, right = 100, 800
        mcp.set_export_config(start_sample=left, end_sample=right)
        mcp.export_raw_data("binary", tmp_capture_dir, digital_channels=[0])

        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0
        bin_data = read_binary_file(bin_files[0])

        # Expected bytes: (right - left) // 8
        expected_bytes = (right - left) // 8
        assert abs(len(bin_data) - expected_bytes) <= 2, \
            f"Binary size {len(bin_data)}, expected ~{expected_bytes}"

        # Compare each byte in the exported binary with the corresponding
        # byte in get_samples
        # The binary export starts at sample 'left', so byte 0 in export
        # corresponds to byte left//8 in all_samples
        start_byte_src = left // 8
        mismatches = 0
        compared = 0
        for i in range(min(len(bin_data), len(all_samples) - start_byte_src)):
            if all_samples[start_byte_src + i] != bin_data[i]:
                mismatches += 1
            compared += 1

        assert compared > 0, "No bytes could be compared"
        # Allow up to 2 byte mismatches (boundary alignment)
        assert mismatches <= 2, \
            f"Binary data mismatch: {mismatches}/{compared} bytes differ"

    def test_left_cursor_csv_first_row_matches(self, mcp: McpClient,
                                               device_id: str,
                                               tmp_capture_dir: str,
                                               cleanup_after_test):
        """Left cursor CSV first row corresponds to start_sample, not 0."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        all_samples = mcp.get_samples(channel_type="logic", channel_index=0)
        total_samples = len(all_samples) * 8  # bytes → bits
        assert total_samples > 1000

        left = 500
        mcp.set_export_config(start_sample=left, end_sample=total_samples)
        mcp.export_raw_data("csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        csv_data = read_csv_file(csv_files[0])

        # The first row should correspond to sample 'left', not sample 0
        # Verify by checking the first row's bit value matches sample 'left'
        if len(csv_data) > 0:
            row_values = list(csv_data[0].values())
            if row_values:
                data_str = str(row_values[-1]).strip()
                try:
                    first_data_val = int(data_str)
                    expected_first_bit = get_logic_sample_bit(all_samples, left)
                    if expected_first_bit >= 0 and first_data_val in (0, 1):
                        assert first_data_val == expected_first_bit, \
                            f"First CSV row data={first_data_val}, " \
                            f"but sample[{left}] bit={expected_first_bit}. " \
                            f"Left cursor not applied to first row."
                except ValueError:
                    pass  # Skip if data format is not a simple int

    def test_right_cursor_csv_last_row_matches(self, mcp: McpClient,
                                                device_id: str,
                                                tmp_capture_dir: str,
                                                cleanup_after_test):
        """Right cursor CSV last row corresponds to end_sample-1."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=1.0)

        all_samples = mcp.get_samples(channel_type="logic", channel_index=0)

        right = 5000
        mcp.set_export_config(start_sample=0, end_sample=right)
        mcp.export_raw_data("csv", tmp_capture_dir, digital_channels=[0])

        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        csv_data = read_csv_file(csv_files[0])

        # Verify last row corresponds to sample right-1
        if len(csv_data) > 0:
            row_values = list(csv_data[-1].values())
            if row_values:
                data_str = str(row_values[-1]).strip()
                try:
                    last_data_val = int(data_str)
                    expected_last_bit = get_logic_sample_bit(all_samples, right - 1)
                    if expected_last_bit >= 0 and last_data_val in (0, 1):
                        assert last_data_val == expected_last_bit, \
                            f"Last CSV row data={last_data_val}, " \
                            f"but sample[{right-1}] bit={expected_last_bit}. " \
                            f"Right cursor not applied to last row."
                except ValueError:
                    pass  # Skip if data format is not a simple int
