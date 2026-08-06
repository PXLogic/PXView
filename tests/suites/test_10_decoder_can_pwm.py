"""
test_10_decoder_can.py - CAN protocol decoder tests.
test_11_decoder_pwm.py - PWM protocol decoder tests.
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


class TestDecoderCan:

    def test_list_analyzers_contains_can(self, mcp: McpClient):
        """list_analyzers includes can_c."""
        analyzers = mcp.list_analyzers()
        names = [a.get("id") or a.get("name") for a in analyzers]
        assert "can_c" in names

    def test_can_options(self, mcp: McpClient):
        """get_analyzer_options returns CAN channel requirements."""
        opts = mcp.get_analyzer_options("can_c")
        assert_analyzer_options_valid(opts)

    def test_can_add_and_capture(self, mcp: McpClient, device_id: str,
                                 cleanup_after_test):
        """Add CAN decoder and perform capture."""
        analyzer_id = add_decoder_safe(mcp, "can_c",
                                       channel_map={"can_rx": 0},
                                       device_id=device_id)
        assert analyzer_id

        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert status["state"] in ("completed", "idle")

    def test_can_decode_results(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """CAN decoder produces valid results structure."""
        analyzer_id = add_decoder_safe(mcp, "can_c",
                                       channel_map={"can_rx": 0},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = get_decoder_results_with_retry(mcp, analyzer_id,
                                                 max_wait=15.0)
        for ann in results:
            assert_annotation_valid(ann)

    def test_can_get_class_names(self, mcp: McpClient):
        """get_decoder_class_names returns CAN class names."""
        names = mcp.get_decoder_class_names("can_c")
        assert isinstance(names, list)
        assert len(names) > 0


class TestDecoderPwm:

    def test_list_analyzers_contains_pwm(self, mcp: McpClient):
        """list_analyzers includes pwm_c."""
        analyzers = mcp.list_analyzers()
        names = [a.get("id") or a.get("name") for a in analyzers]
        assert "pwm_c" in names

    def test_pwm_options(self, mcp: McpClient):
        """get_analyzer_options returns PWM channel requirements."""
        opts = mcp.get_analyzer_options("pwm_c")
        assert_analyzer_options_valid(opts)

    def test_pwm_add_before_capture(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """Add PWM decoder before capture."""
        analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                       channel_map={"data": 0},
                                       device_id=device_id)
        assert analyzer_id

        status = do_timed_capture(mcp, device_id,
                                  channels=[0],
                                  sample_rate=1000000,
                                  duration_seconds=1.0)
        assert status["state"] in ("completed", "idle")

    def test_pwm_decode_results(self, mcp: McpClient, device_id: str,
                                cleanup_after_test):
        """PWM decoder produces valid results."""
        analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                       channel_map={"data": 0},
                                       device_id=device_id)
        do_timed_capture(mcp, device_id,
                         channels=[0],
                         sample_rate=1000000,
                         duration_seconds=1.0)

        results = get_decoder_results_with_retry(mcp, analyzer_id,
                                                 max_wait=15.0)
        for ann in results:
            assert_annotation_valid(ann)

    def test_pwm_remove(self, mcp: McpClient, device_id: str,
                        cleanup_after_test):
        """Remove PWM decoder."""
        analyzer_id = add_decoder_safe(mcp, "pwm_c",
                                       channel_map={"data": 0},
                                       device_id=device_id)
        mcp.remove_analyzer(analyzer_id)

        decoders = mcp.get_active_decoders()
        ids = [d.get("instance_id") or d.get("id") for d in decoders]
        assert analyzer_id not in ids

    def test_pwm_get_class_names(self, mcp: McpClient):
        """get_decoder_class_names returns PWM class names."""
        names = mcp.get_decoder_class_names("pwm_c")
        assert isinstance(names, list)
        assert len(names) > 0
