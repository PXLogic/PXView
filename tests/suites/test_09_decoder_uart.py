"""
test_09_decoder_uart.py - UART protocol decoder tests.
"""

import time

import pytest

from mcp_client import McpClient, McpError
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


class TestDecoderUart:

    def test_list_analyzers_contains_uart(self, mcp: McpClient):
        """list_analyzers includes uart_c."""
        analyzers = mcp.list_analyzers()
        names = [a.get("id") or a.get("name") for a in analyzers]
        assert "uart_c" in names

    def test_uart_options(self, mcp: McpClient):
        """get_analyzer_options returns UART channel/option requirements."""
        opts = mcp.get_analyzer_options("uart_c")
        assert_analyzer_options_valid(opts)

    def test_uart_add_before_capture(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """Add UART decoder before capture."""
        analyzer_id = add_decoder_safe(mcp, "uart_c",
                                       channel_map={"rx": 0, "tx": 1},
                                       device_id=device_id)
        assert analyzer_id

        status = do_timed_capture(mcp, device_id,
                                  channels=[0, 1],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert status["state"] in ("completed", "idle")

    def test_uart_add_after_capture(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """Add UART decoder after capture."""
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        analyzer_id = add_decoder_safe(mcp, "uart_c",
                                       channel_map={"rx": 0, "tx": 1})
        assert analyzer_id

    def test_uart_decode_results(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """UART decoder produces valid results."""
        analyzer_id = add_decoder_safe(mcp, "uart_c",
                                       channel_map={"rx": 0, "tx": 1},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = get_decoder_results_with_retry(mcp, analyzer_id,
                                                 max_wait=15.0)
        for ann in results:
            assert_annotation_valid(ann)

    def test_uart_remove(self, mcp: McpClient, device_id: str,
                         cleanup_after_test):
        """Remove UART decoder."""
        analyzer_id = add_decoder_safe(mcp, "uart_c",
                                       channel_map={"rx": 0},
                                       device_id=device_id)
        mcp.remove_analyzer(analyzer_id)

        decoders = mcp.get_active_decoders()
        ids = [d.get("instance_id") or d.get("id") for d in decoders]
        assert analyzer_id not in ids

    def test_uart_get_class_names(self, mcp: McpClient):
        """get_decoder_class_names returns UART class names."""
        names = mcp.get_decoder_class_names("uart_c")
        assert isinstance(names, list)
        assert len(names) > 0

    def test_uart_baudrate_option(self, mcp: McpClient, device_id: str,
                                  cleanup_after_test):
        """UART works with custom baudrate option."""
        analyzer_id = add_decoder_safe(mcp, "uart_c",
                                       channel_map={"rx": 0, "tx": 1},
                                       options={"baudrate": "9600"},
                                       device_id=device_id)
        assert analyzer_id

        do_timed_capture(mcp, device_id,
                         channels=[0, 1],
                         sample_rate=1000000,
                         duration_seconds=0.5)
