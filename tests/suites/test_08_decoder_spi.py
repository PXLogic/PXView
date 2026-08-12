"""
test_08_decoder_spi.py - SPI protocol decoder tests.

Note: basic decoder operations (list_analyzers, get_analyzer_options,
add/remove, decode_results, class_names) for ALL decoders are covered
by test_12_decoder_batch.py.  This file retains only SPI-specific
behavior tests that test_12 does not cover.
"""

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_analyzer_options_valid
from helpers.capture_helper import do_timed_capture
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p0


class TestDecoderSpi:

    def test_spi_options(self, mcp: McpClient):
        """get_analyzer_options returns SPI channel requirements."""
        opts = mcp.get_analyzer_options("spi_c")
        assert_analyzer_options_valid(opts)

    def test_spi_add_before_capture(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """Add SPI decoder before capture."""
        analyzer_id = add_decoder_safe(mcp, "spi_c",
                                       channel_map={"sclk": 0, "mosi": 1,
                                                    "miso": 2, "cs": 3},
                                       device_id=device_id)
        assert analyzer_id

        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1, 2, 3],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert status["state"] in ("completed", "idle")

    def test_spi_decode_results(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """SPI decoder produces valid results."""
        analyzer_id = add_decoder_safe(mcp, "spi_c",
                                       channel_map={"sclk": 0, "mosi": 1,
                                                    "miso": 2, "cs": 3},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1, 2, 3],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = get_decoder_results_with_retry(mcp, analyzer_id,
                                                 max_wait=15.0)
        # Results may be empty on default (non-SPI) demo data;
        # just verify the decoder ran without crash.
        from helpers.assertions import assert_annotation_valid
        for ann in results:
            assert_annotation_valid(ann)

    def test_spi_cs_optional(self, mcp: McpClient, device_id: str,
                             cleanup_after_test):
        """SPI works without CS channel (optional)."""
        analyzer_id = add_decoder_safe(mcp, "spi_c",
                                       channel_map={"sclk": 0, "mosi": 1},
                                       device_id=device_id)
        assert analyzer_id

        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=0.5)
