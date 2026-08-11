"""
test_06_export_import.py - Various format export/import tests (Layer 6).

Validates CSV export, binary export, data table CSV export, .pxc save/load,
with different options and configurations.
"""

import os

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture
from helpers.export_helper import (
    find_exported_csv,
    find_exported_binary,
    read_csv_file,
    read_binary_file,
)
from helpers.decoder_helper import add_decoder_safe, get_decoder_results_with_retry

pytestmark = pytest.mark.p1


class TestExportImport:

    def test_export_csv_single_channel(self, mcp: McpClient, device_id: str,
                                       tmp_capture_dir: str,
                                       cleanup_after_test):
        """Export single channel CSV."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0])
        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        data = read_csv_file(csv_files[0])
        assert len(data) > 0

    def test_export_csv_multi_channel(self, mcp: McpClient, device_id: str,
                                      tmp_capture_dir: str,
                                      cleanup_after_test):
        """Export multi-channel CSV."""
        do_timed_capture(mcp, device_id, channels=[0, 1, 2, 3],
                         sample_rate=1000000, duration_seconds=0.3)
        mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0, 1, 2, 3])
        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) >= 1

    def test_export_csv_iso8601(self, mcp: McpClient, device_id: str,
                                tmp_capture_dir: str,
                                cleanup_after_test):
        """Export CSV with ISO 8601 timestamps."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0],
                                iso8601_timestamp=True)
        csv_files = find_exported_csv(tmp_capture_dir)
        assert len(csv_files) > 0

    def test_export_binary_single_channel(self, mcp: McpClient, device_id: str,
                                          tmp_capture_dir: str,
                                          cleanup_after_test):
        """Export single channel binary."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        mcp.export_raw_data_binary(tmp_capture_dir, digital_channels=[0])
        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0
        data = read_binary_file(bin_files[0])
        assert len(data) > 0

    def test_export_binary_multi_channel(self, mcp: McpClient, device_id: str,
                                         tmp_capture_dir: str,
                                         cleanup_after_test):
        """Export multi-channel binary."""
        do_timed_capture(mcp, device_id, channels=[0, 1, 2, 3],
                         sample_rate=1000000, duration_seconds=0.3)
        mcp.export_raw_data_binary(tmp_capture_dir, digital_channels=[0, 1, 2, 3])
        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) >= 1

    def test_export_data_table_csv(self, mcp: McpClient, device_id: str,
                                   tmp_capture_dir: str,
                                   cleanup_after_test):
        """Export decoder results as CSV data table."""
        analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                       channel_map={"data": 0},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        # Wait for decoder to finish processing before exporting
        get_decoder_results_with_retry(mcp, analyzer_id, max_wait=15.0)

        filepath = os.path.join(tmp_capture_dir, "decode_table.csv")
        mcp.export_data_table_csv(filepath, analyzers=[
            {"analyzerId": analyzer_id, "radixType": 3}
        ])

    def test_export_data_table_csv_radix_binary(self, mcp: McpClient,
                                                device_id: str,
                                                tmp_capture_dir: str,
                                                cleanup_after_test):
        """Export data table with Binary radix (1)."""
        analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                       channel_map={"data": 0},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        # Wait for decoder to finish processing before exporting
        get_decoder_results_with_retry(mcp, analyzer_id, max_wait=15.0)

        filepath = os.path.join(tmp_capture_dir, "decode_bin.csv")
        mcp.export_data_table_csv(filepath, analyzers=[
            {"analyzerId": analyzer_id, "radixType": 1}
        ])

    def test_export_data_table_csv_radix_decimal(self, mcp: McpClient,
                                                 device_id: str,
                                                 tmp_capture_dir: str,
                                                 cleanup_after_test):
        """Export data table with Decimal radix (2)."""
        analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                       channel_map={"data": 0},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        # Wait for decoder to finish processing before exporting
        get_decoder_results_with_retry(mcp, analyzer_id, max_wait=15.0)

        filepath = os.path.join(tmp_capture_dir, "decode_dec.csv")
        mcp.export_data_table_csv(filepath, analyzers=[
            {"analyzerId": analyzer_id, "radixType": 2}
        ])

    def test_pxc_save_load_roundtrip(self, mcp: McpClient, device_id: str,
                                     tmp_pxc_file: str,
                                     cleanup_after_test):
        """Full .pxc save -> load roundtrip."""
        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.5)
        samples_before = mcp.get_logic_samples(channel_index=0)

        mcp.save_capture(tmp_pxc_file)
        assert os.path.exists(tmp_pxc_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)

        import time; time.sleep(1)
        samples_after = mcp.get_logic_samples(channel_index=0)
        assert samples_before == samples_after, "Data changed after save/load"

    def test_export_partial_range(self, mcp: McpClient, device_id: str,
                                  tmp_capture_dir: str,
                                  cleanup_after_test):
        """Export only a partial range using set_save_range."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        mcp.set_save_range(start_sample=100, end_sample=500)
        mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0])
        csv_files = find_exported_csv(tmp_capture_dir)
        assert len(csv_files) > 0

    def test_export_without_capture_fails(self, mcp: McpClient,
                                          tmp_capture_dir: str,
                                          cleanup_after_test):
        """Export without a capture should return an error."""
        with pytest.raises(McpError):
            mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0])

    def test_load_nonexistent_file(self, mcp: McpClient, cleanup_after_test):
        """load_capture with non-existent file returns error."""
        with pytest.raises(McpError):
            mcp.load_capture("/nonexistent/path/file.pxc")
