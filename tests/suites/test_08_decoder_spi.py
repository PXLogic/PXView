"""
test_08_decoder_spi.py - SPI protocol decoder tests.
"""

import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import (
    assert_annotation_valid,
    assert_analyzer_options_valid,
)
from helpers.capture_helper import do_timed_capture
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p0


class TestDecoderSpi:

    def test_list_analyzers_contains_spi(self, mcp: McpClient):
        """list_analyzers includes spi_c."""
        analyzers = mcp.list_analyzers()
        names = [a.get("id") or a.get("name") for a in analyzers]
        assert "spi_c" in names

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

    def test_spi_add_after_capture(self, mcp: McpClient, device_id: str,
                                   cleanup_after_test):
        """Add SPI decoder after capture."""
        do_timed_capture(mcp, device_id,
                         channels=[0, 1, 2, 3],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        analyzer_id = add_decoder_safe(mcp, "spi_c",
                                       channel_map={"sclk": 0, "mosi": 1,
                                                    "miso": 2, "cs": 3})
        assert analyzer_id

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
        for ann in results:
            assert_annotation_valid(ann)

    def test_spi_remove(self, mcp: McpClient, device_id: str,
                        cleanup_after_test):
        """Remove SPI decoder."""
        analyzer_id = add_decoder_safe(mcp, "spi_c",
                                       channel_map={"sclk": 0, "mosi": 1},
                                       device_id=device_id)
        mcp.remove_analyzer(analyzer_id)

        decoders = mcp.get_active_decoders()
        ids = [d.get("instance_id") or d.get("id") for d in decoders]
        assert analyzer_id not in ids

    def test_spi_get_class_names(self, mcp: McpClient):
        """get_decoder_class_names returns SPI class names."""
        names = mcp.get_decoder_class_names("spi_c")
        assert isinstance(names, list)
        assert len(names) > 0

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
