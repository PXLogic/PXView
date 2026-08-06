"""
test_07_decoder_i2c.py - I2C protocol decoder tests (Layer 7).

Validates add_analyzer, get_analyzer_options, get_analyzer_results,
remove_analyzer, reconfigure_decoder, get_decoder_class_names for I2C.
"""

import time

import pytest

from mcp_client import McpClient, McpError
from helpers.assertions import (
    assert_annotation_valid,
    assert_analyzer_list_valid,
    assert_analyzer_options_valid,
)
from helpers.capture_helper import do_timed_capture
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p0


class TestDecoderI2c:

    def test_list_analyzers_contains_i2c(self, mcp: McpClient):
        """list_analyzers includes i2c_c."""
        analyzers = mcp.list_analyzers()
        assert_analyzer_list_valid(analyzers)
        names = [a.get("id") or a.get("name") for a in analyzers]
        assert "i2c_c" in names, f"i2c_c not in analyzer list: {names}"

    def test_get_analyzer_options_i2c(self, mcp: McpClient):
        """get_analyzer_options returns I2C channel requirements."""
        opts = mcp.get_analyzer_options("i2c_c")
        assert_analyzer_options_valid(opts)

    def test_add_i2c_before_capture(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """Add I2C decoder before capture, then capture."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        assert analyzer_id, f"Failed to get analyzer_id: {analyzer_id}"

        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert status["state"] in ("completed", "idle")

    def test_add_i2c_after_capture(self, mcp: McpClient, device_id: str,
                                   cleanup_after_test):
        """Add I2C decoder after capture completes."""
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1})
        assert analyzer_id

    def test_i2c_get_results_structure(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """I2C decoder results have valid annotation structure."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = get_decoder_results_with_retry(mcp, analyzer_id,
                                                 max_wait=15.0)
        # Results may be empty if demo data doesn't contain I2C pattern
        for ann in results:
            assert_annotation_valid(ann)

    def test_i2c_remove_analyzer(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """remove_analyzer removes the I2C decoder."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        mcp.remove_analyzer(analyzer_id)

        # Verify it's gone from active decoders
        decoders = mcp.get_active_decoders()
        ids = [d.get("instance_id") or d.get("id") for d in decoders]
        assert analyzer_id not in ids, \
            f"Analyzer {analyzer_id} still active after remove"

    def test_i2c_get_class_names(self, mcp: McpClient):
        """get_decoder_class_names returns I2C annotation class names."""
        names = mcp.get_decoder_class_names("i2c_c")
        assert isinstance(names, list)
        assert len(names) > 0, "I2C should have annotation class names"

    def test_i2c_reconfigure(self, mcp: McpClient, device_id: str,
                             cleanup_after_test):
        """reconfigure_decoder changes channel mapping."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1, 2, 3],
                         sample_rate=1000000,
                         duration_seconds=0.5)

        # Reconfigure to different channels
        mcp.reconfigure_decoder(analyzer_id,
                                channel_map={"sda": 2, "scl": 3})
        time.sleep(1)

    def test_i2c_export_data_table(self, mcp: McpClient, device_id: str,
                                   tmp_capture_dir: str,
                                   cleanup_after_test):
        """export_data_table_csv exports I2C decode results."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        filepath = tmp_capture_dir + "/i2c_results.csv"
        mcp.export_data_table_csv(filepath, analyzers=[
            {"analyzerId": analyzer_id, "radixType": 3}
        ])

    def test_i2c_results_max_count(self, mcp: McpClient, device_id: str,
                                   cleanup_after_test):
        """get_analyzer_results respects maxCount parameter."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = mcp.get_analyzer_results(analyzer_id, max_count=5)
        if results:
            assert len(results) <= 5, \
                f"maxCount=5 but got {len(results)} results"

    def test_i2c_results_sample_range(self, mcp: McpClient, device_id: str,
                                      cleanup_after_test):
        """get_analyzer_results with startSample/endSample filter."""
        analyzer_id = add_decoder_safe(mcp, "i2c_c",
                                       channel_map={"sda": 0, "scl": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = mcp.get_analyzer_results(analyzer_id,
                                           start_sample=0,
                                           end_sample=500000,
                                           max_count=1000)
        for ann in results:
            assert ann["start_sample"] <= 500000, \
                f"Annotation outside range: {ann}"
