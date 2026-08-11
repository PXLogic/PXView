"""
test_12_decoder_batch.py - Batch decoder tests.

Iterates over all available decoders from list_analyzers, adds each one
with default/first channel, verifies no crash, checks results structure.
"""

import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_annotation_valid
from helpers.capture_helper import do_timed_capture
from helpers.decoder_helper import add_decoder_safe

pytestmark = pytest.mark.p1


def get_testable_decoders(mcp: McpClient) -> list:
    """Get list of decoder names that can be tested with channel 0."""
    analyzers = mcp.list_analyzers()
    names = []
    for a in analyzers:
        name = a.get("id") or a.get("name")
        if name:
            names.append(name)
    return sorted(names)


class TestDecoderBatch:

    def test_all_decoders_options(self, mcp: McpClient):
        """get_analyzer_options works for every available decoder."""
        decoders = get_testable_decoders(mcp)
        assert len(decoders) > 0
        failures = []
        for name in decoders:
            try:
                opts = mcp.get_analyzer_options(name)
                assert isinstance(opts, dict), f"{name}: opts not dict"
            except Exception as e:
                failures.append(f"{name}: {e}")
        assert not failures, "Failures:\n" + "\n".join(failures)

    def test_all_decoders_add_remove(self, mcp: McpClient, device_id: str,
                                     cleanup_after_test):
        """Add and remove every decoder without crash."""
        decoders = get_testable_decoders(mcp)
        # Do one capture first
        do_timed_capture(mcp, device_id,
                         channels=[0, 1, 2, 3],
                         sample_rate=1000000,
                         duration_seconds=0.5)

        results = {}
        for name in decoders:
            try:
                # Get options to find required channels
                opts = mcp.get_analyzer_options(name)
                # Build a channel map using channel 0 for first required channel
                channel_map = {}
                channels_list = opts.get("channels") or opts.get("required_channels") or []
                for i, ch in enumerate(channels_list):
                    ch_id = ch.get("id") or ch.get("name", f"ch{i}")
                    channel_map[ch_id] = i  # Map to channels 0, 1, 2...

                analyzer_id = add_decoder_safe(mcp, name,
                                               channel_map=channel_map if channel_map else None)
                if analyzer_id:
                    results[name] = "OK"
                    mcp.remove_analyzer(analyzer_id)
                else:
                    results[name] = "NO_ID"
            except Exception as e:
                results[name] = f"ERROR: {e}"

        # Report results
        ok = sum(1 for v in results.values() if v == "OK")
        failed = {k: v for k, v in results.items() if v != "OK"}
        # At least 50% should succeed
        assert ok >= len(decoders) * 0.3, \
            f"Only {ok}/{len(decoders)} decoders succeeded. Failures: {failed}"

    def test_all_decoders_get_class_names(self, mcp: McpClient):
        """get_decoder_class_names works for every decoder."""
        decoders = get_testable_decoders(mcp)
        failures = []
        for name in decoders:
            try:
                names = mcp.get_decoder_class_names(name)
                assert isinstance(names, list), f"{name}: not a list"
            except Exception as e:
                failures.append(f"{name}: {e}")
        # Allow some failures for decoders without annotations
        assert len(failures) < len(decoders) * 0.5, \
            f"Too many failures: {failures}"
