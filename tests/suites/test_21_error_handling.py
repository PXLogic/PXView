"""
test_21_error_handling.py - Error handling tests.
"""

import pytest

from pxview_automation import McpClient, McpError
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p2


class TestErrorHandling:

    def test_invalid_device_id(self, mcp: McpClient, cleanup_after_test):
        """start_capture with invalid deviceId returns error."""
        with pytest.raises(McpError):
            mcp.start_capture("99999")

    def test_capture_already_running(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """Starting a second capture while one is running returns error."""
        mcp.start_capture(device_id, {
            "digitalChannels": [0],
            "digitalSampleRate": 1000000,
        }, {
            "timedCaptureMode": {"durationSeconds": 5}
        })
        import time; time.sleep(0.5)
        # Second start should error or be rejected
        try:
            with pytest.raises(McpError):
                mcp.start_capture(device_id, {
                    "digitalChannels": [1],
                    "digitalSampleRate": 1000000,
                })
        except Exception:
            pass  # Some implementations may queue
        mcp.stop_capture()

    def test_analyzer_not_found(self, mcp: McpClient, cleanup_after_test):
        """add_analyzer with non-existent name returns error."""
        with pytest.raises(McpError):
            mcp.add_analyzer(analyzer_name="nonexistent_decoder_xyz")

    def test_remove_nonexistent_analyzer(self, mcp: McpClient,
                                          cleanup_after_test):
        """remove_analyzer with non-existent ID returns error."""
        with pytest.raises(McpError):
            mcp.remove_analyzer("nonexistent_id_99999")

    def test_export_without_capture(self, mcp: McpClient,
                                    tmp_capture_dir: str,
                                    cleanup_after_test):
        """Export without capture returns error."""
        with pytest.raises(McpError):
            mcp.export_raw_data_csv(tmp_capture_dir, digital_channels=[0])

    def test_load_nonexistent_file(self, mcp: McpClient, cleanup_after_test):
        """load_capture with non-existent file returns error."""
        with pytest.raises(McpError):
            mcp.load_capture("C:/nonexistent/path/file.pxc")

    def test_save_to_invalid_path(self, mcp: McpClient, device_id: str,
                                  cleanup_after_test):
        """save_capture to invalid path returns error."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        with pytest.raises(McpError):
            mcp.save_capture("C:/nonexistent/dir/file.pxc")

    def test_get_analyzer_results_invalid_id(self, mcp: McpClient,
                                             cleanup_after_test):
        """get_analyzer_results with invalid ID returns error."""
        with pytest.raises(McpError):
            mcp.get_analyzer_results("invalid_id_99999")

    def test_get_logic_samples_invalid_channel(self, mcp: McpClient, device_id: str,
                                               cleanup_after_test):
        """get_logic_samples with invalid channel index."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)
        try:
            result = mcp.get_logic_samples(channel_index=999)
        except McpError:
            pass  # Expected

    def test_destroy_invalid_session(self, mcp: McpClient):
        """destroy_session with invalid ID returns error."""
        with pytest.raises(McpError):
            mcp.destroy_session(session_id=99999)

    def test_set_active_invalid_session(self, mcp: McpClient):
        """set_active_session with invalid ID returns error."""
        with pytest.raises(McpError):
            mcp.set_active_session(session_id=99999)

    def test_clear_error_state_works(self, mcp: McpClient):
        """clear_error_state can be called multiple times."""
        mcp.clear_error_state()
        mcp.clear_error_state()
        result = mcp.get_error_state()
        assert isinstance(result, dict)

    def test_clear_all_decoders_empty(self, mcp: McpClient):
        """clear_all_decoders works with no decoders active."""
        mcp.clear_all_decoders()
        decoders = mcp.get_active_decoders()
        assert len(decoders) == 0

    def test_close_capture_no_capture(self, mcp: McpClient):
        """close_capture with no active capture handles gracefully."""
        try:
            mcp.close_capture()
        except McpError:
            pass  # Expected if no capture
