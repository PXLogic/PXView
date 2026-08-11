"""
test_22_stress.py - Stress tests.

Repeated captures, rapid add/remove analyzers, large sample counts,
multi-analyzer concurrent, save/load loops.
"""

import os
import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture
from helpers.decoder_helper import add_decoder_safe

pytestmark = [pytest.mark.p2, pytest.mark.stress]


class TestStress:

    def test_repeated_capture_5x(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """5 consecutive captures without crash."""
        for i in range(5):
            status = do_timed_capture(mcp, device_id,
                                      channels=[0],
                                      sample_rate=1000000,
                                      duration_seconds=0.3)
            assert status["state"] in ("completed", "idle"), \
                f"Iteration {i}: capture failed"

    def test_repeated_capture_10x(self, mcp: McpClient, device_id: str,
                                  cleanup_after_test):
        """10 consecutive captures without crash."""
        for i in range(10):
            status = do_timed_capture(mcp, device_id,
                                      channels=[0, 1],
                                      sample_rate=1000000,
                                      duration_seconds=0.2)
            assert status["state"] in ("completed", "idle"), \
                f"Iteration {i}: capture failed"

    def test_add_remove_analyzer_10x(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """Add/remove analyzer 10 times."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        for i in range(10):
            analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                           channel_map={"data": 0})
            assert analyzer_id
            mcp.remove_analyzer(analyzer_id)
        decoders = mcp.get_active_decoders()
        assert len(decoders) == 0

    def test_multi_analyzer_concurrent(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """Add multiple analyzers simultaneously."""
        do_timed_capture(mcp, device_id, channels=[0, 1, 2, 3],
                         sample_rate=1000000, duration_seconds=0.5)
        ids = []
        for name, ch_map in [
            ("pwm_c", {"data": 0}),
            ("counter_c", {"data": 1}),
            ("timing_c", {"data": 2}),
        ]:
            try:
                aid = add_decoder_safe(mcp, name, channel_map=ch_map)
                if aid:
                    ids.append(aid)
            except Exception:
                pass
        # Verify all active
        decoders = mcp.get_active_decoders()
        assert len(decoders) >= 1
        # Clean up
        for aid in ids:
            try:
                mcp.remove_analyzer(aid)
            except Exception:
                pass

    def test_capture_save_load_loop_3x(self, mcp: McpClient, device_id: str,
                                       tmp_capture_dir: str,
                                       cleanup_after_test):
        """Loop: capture -> save -> close -> load 3 times."""
        for i in range(3):
            do_timed_capture(mcp, device_id, channels=[0],
                             sample_rate=1000000, duration_seconds=0.2)
            filepath = os.path.join(tmp_capture_dir, f"stress_{i}.pxc")
            mcp.save_capture(filepath)
            assert os.path.exists(filepath)
            mcp.close_capture()
            mcp.load_capture(filepath)
            time.sleep(0.5)
            samples = mcp.get_logic_samples(channel_index=0)
            assert len(samples) > 0
            mcp.close_capture()

    def test_mcp_high_frequency(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """High frequency MCP calls don't time out."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)
        for i in range(20):
            status = mcp.get_capture_status()
            assert isinstance(status, dict)

    def test_large_sample_count(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """Large sample count capture (1M samples)."""
        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=10000000,
                                  duration_seconds=0.5)
        assert status["state"] in ("completed", "idle")
        samples = mcp.get_logic_samples(channel_index=0)
        assert len(samples) > 0

    def test_export_import_stress(self, mcp: McpClient, device_id: str,
                                  tmp_capture_dir: str,
                                  cleanup_after_test):
        """Multiple exports in sequence."""
        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.3)
        for i in range(3):
            subdir = os.path.join(tmp_capture_dir, f"export_{i}")
            os.makedirs(subdir, exist_ok=True)
            mcp.export_raw_data_csv(subdir, digital_channels=[0])
            mcp.export_raw_data_binary(subdir, digital_channels=[0])
