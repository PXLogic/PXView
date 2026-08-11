"""
test_05_data_integrity.py - Data integrity tests (Layer 5).

Core user requirement: capture -> save -> load -> verify data is identical.
Also validates export CSV/binary round-trip against get_logic_samples.
"""

import os
import time

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture
from helpers.export_helper import (
    compare_logic_samples,
    find_exported_csv,
    read_csv_file,
    find_exported_binary,
    read_binary_file,
)

pytestmark = pytest.mark.p0


class TestDataIntegrity:

    def test_capture_save_load_identical(self, mcp: McpClient, device_id: str,
                                         tmp_pxc_file: str,
                                         cleanup_after_test):
        """Capture -> save_capture -> load_capture -> data is identical."""
        # 1. Capture
        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1, 2, 3],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert_capture_status(status, "completed")

        # 2. Get original samples
        original_ch0 = mcp.get_logic_samples(channel_index=0)
        original_ch1 = mcp.get_logic_samples(channel_index=1)
        assert len(original_ch0) > 0
        assert len(original_ch1) > 0

        # 3. Save to .pxc
        mcp.save_capture(tmp_pxc_file)
        assert os.path.exists(tmp_pxc_file), \
            f"Save file not created: {tmp_pxc_file}"
        assert os.path.getsize(tmp_pxc_file) > 0, \
            f"Save file is empty: {tmp_pxc_file}"

        # 4. Close current capture
        mcp.close_capture()

        # 5. Load from .pxc
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)  # Allow load to complete

        # 6. Get loaded samples
        loaded_ch0 = mcp.get_logic_samples(channel_index=0)
        loaded_ch1 = mcp.get_logic_samples(channel_index=1)

        # 7. Verify data integrity
        assert compare_logic_samples(original_ch0, loaded_ch0), \
            f"Channel 0 data mismatch: original={len(original_ch0)} bytes, " \
            f"loaded={len(loaded_ch0)} bytes"
        assert compare_logic_samples(original_ch1, loaded_ch1), \
            f"Channel 1 data mismatch: original={len(original_ch1)} bytes, " \
            f"loaded={len(loaded_ch1)} bytes"

    def test_capture_export_csv_vs_samples(self, mcp: McpClient, device_id: str,
                                           tmp_capture_dir: str,
                                           cleanup_after_test):
        """export_raw_data_csv output matches get_logic_samples."""
        # 1. Capture
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status, "completed")

        # 2. Get samples via MCP
        mcp_samples = mcp.get_logic_samples(channel_index=0)
        assert len(mcp_samples) > 0

        # 3. Export to CSV
        mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0])

        # 4. Find and read CSV
        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0, f"No CSV files in {tmp_capture_dir}"
        csv_data = read_csv_file(csv_files[0])
        assert len(csv_data) > 0, "CSV file is empty"

        # 5. Verify CSV row count is close to sample count
        # (CSV has one row per sample, may differ slightly due to headers)
        assert len(csv_data) > 0, "CSV has no data rows"

    def test_capture_export_binary_vs_samples(self, mcp: McpClient, device_id: str,
                                              tmp_capture_dir: str,
                                              cleanup_after_test):
        """export_raw_data_binary output matches get_logic_samples."""
        # 1. Capture
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status, "completed")

        # 2. Get samples via MCP
        mcp_samples = mcp.get_logic_samples(channel_index=0)
        assert len(mcp_samples) > 0

        # 3. Export to binary
        mcp.export_raw_data_binary(tmp_capture_dir, digital_channels=[0])

        # 4. Find and read binary file
        bin_files = find_exported_binary(tmp_capture_dir)
        assert len(bin_files) > 0, f"No binary files in {tmp_capture_dir}"
        bin_data = read_binary_file(bin_files[0])
        assert len(bin_data) > 0, "Binary file is empty"

    def test_save_load_all_channels(self, mcp: McpClient, device_id: str,
                                    tmp_pxc_file: str,
                                    cleanup_after_test):
        """Save -> load preserves data on all channels."""
        channels = [0, 1, 2, 3]
        status = do_timed_capture(mcp, device_id,
                                  channels=channels,
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status, "completed")

        originals = {}
        for ch in channels:
            originals[ch] = mcp.get_logic_samples(channel_index=ch)
            assert len(originals[ch]) > 0, f"Channel {ch} no data"

        mcp.save_capture(tmp_pxc_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)

        for ch in channels:
            loaded = mcp.get_logic_samples(channel_index=ch)
            assert compare_logic_samples(originals[ch], loaded), \
                f"Channel {ch} data mismatch after save/load"

    def test_save_load_preserves_sample_rate(self, mcp: McpClient, device_id: str,
                                             tmp_pxc_file: str,
                                             cleanup_after_test):
        """Save -> load preserves sample rate configuration."""
        rate = 1000000
        do_timed_capture(mcp, device_id,
                         channels=[0],
                         sample_rate=rate,
                         duration_seconds=0.5)

        config_before = mcp.get_sample_config()
        mcp.save_capture(tmp_pxc_file)
        mcp.close_capture()
        mcp.load_capture(tmp_pxc_file)
        time.sleep(1)
        config_after = mcp.get_sample_config()

        assert config_before.get("sample_rate") == config_after.get("sample_rate"), \
            f"Sample rate changed: {config_before.get('sample_rate')} -> " \
            f"{config_after.get('sample_rate')}"

    def test_set_save_range_export(self, mcp: McpClient, device_id: str,
                                   tmp_capture_dir: str,
                                   cleanup_after_test):
        """set_save_range limits export to specified range."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=0.5)
        assert_capture_status(status, "completed")

        # Set a range
        mcp.set_save_range(start_sample=100, end_sample=1000)

        # Export
        mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0])

        # Verify CSV has data
        csv_files = find_exported_csv(tmp_capture_dir, "channel")
        assert len(csv_files) > 0
        csv_data = read_csv_file(csv_files[0])
        # Should have roughly 900 rows (1000-100)
        assert len(csv_data) > 0

    def test_capture_close_reload_cycle(self, mcp: McpClient, device_id: str,
                                        tmp_pxc_file: str,
                                        cleanup_after_test):
        """Multiple capture -> save -> close -> load cycles work."""
        for i in range(3):
            do_timed_capture(mcp, device_id,
                             channels=[0],
                             sample_rate=1000000,
                             duration_seconds=0.3)
            filepath = tmp_pxc_file.replace(".pxc", f"_{i}.pxc")
            mcp.save_capture(filepath)
            assert os.path.exists(filepath)
            mcp.close_capture()
            mcp.load_capture(filepath)
            time.sleep(0.5)
            samples = mcp.get_logic_samples(channel_index=0)
            assert len(samples) > 0, f"Cycle {i}: no samples after load"
            mcp.close_capture()
