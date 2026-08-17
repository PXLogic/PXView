"""
test_36_glitch_filter_i2c_e2e.py - Glitch filter QUANTITATIVE E2E (harden-review-findings T10).

Uses the demo's deterministic I2C pattern as a controlled "edge" waveform with
known geometry instead of an un-injectable synthetic glitch:
  * I2C: ch0 = SCL, ch1 = SDA; each bit-time is I2C_SPB = 50 samples @ 1 MHz,
    so every genuine SCL/SDA pulse is ~50 samples wide.

Ground truth for the glitch filter threshold (samples):
  * threshold < pulse width (e.g. 5): genuine pulses (50-wide) are NOT filtered
    -> SCL/SDA carry many transitions (bus intact / byte-exact preserved).
  * threshold > pulse width (e.g. 60): every 50-wide pulse is a "glitch" and is
    removed -> SCL/SDA collapse to a held level -> transitions ~ 0.

This makes the filter's quantitative behavior observable on real deterministic
bus data, complementing the synthetic unit test (test_glitch_filter).
"""

import pytest

from pxview_automation import McpClient

from helpers.capture_helper import do_buffer_capture_with_pattern

pytestmark = pytest.mark.p2

SAMPLE_RATE = 1_000_000
SAMPLE_COUNT = 100_000
I2C_SPB = 50  # demo I2C bit-time in samples at 1 MHz


def _unpack_bits(raw: bytes, count: int) -> list:
    valid = len(raw) // 8
    bits = []
    for b in raw[:valid]:
        for k in range(8):
            bits.append((b >> k) & 1)
    return bits[:count]


def _count_transitions(bits: list) -> int:
    return sum(1 for i in range(1, len(bits)) if bits[i] != bits[i - 1])


def _capture_sc_edges(mcp: McpClient, device_id: str, threshold: int):
    """Configure a glitch filter on SCL/SDA, capture I2C, return SCL edges."""
    mcp.configure_glitch_filter(channels=[0, 1], thresholds=[threshold, threshold],
                                modes=[0, 0])
    do_buffer_capture_with_pattern(
        mcp, device_id,
        channels=[0, 1],
        sample_rate=SAMPLE_RATE,
        sample_count=SAMPLE_COUNT,
        pattern="i2c",
    )
    raw = mcp.get_samples(channel_index=0, channel_type="logic",
                          start_sample=0, end_sample=SAMPLE_COUNT)
    bits = _unpack_bits(bytes(raw), SAMPLE_COUNT)
    return _count_transitions(bits)


class TestGlitchFilterQuantitative:

    def test_small_threshold_preserves_i2c(self, mcp: McpClient, device_id: str,
                                           cleanup_after_test):
        """threshold < bit width: genuine I2C pulses survive (many edges)."""
        mcp.connect_device(device_id)
        mcp.configure_glitch_filter(channels=[])  # clear
        edges_preserved = _capture_sc_edges(mcp, device_id,
                                            threshold=I2C_SPB // 10)  # 5
        assert edges_preserved > 100, \
            f"threshold<SPB must keep the bus; only {edges_preserved} edges"
        mcp.configure_glitch_filter(channels=[])

    def test_raw_read_is_filter_invariant(self, mcp: McpClient, device_id: str,
                                          cleanup_after_test):
        """Documented behavior: MCP `get_samples` returns the RAW snapshot and
        is therefore invariant to the glitch filter (filtering happens in the
        processed/display path, not the raw-read API).

        We assert and lock this invariant so future changes to `get_samples`
        don't silently alter the contract, and we record that the quantitative
        *removal* semantics are covered by the synthetic unit test
        `tests/qtest/data/test_glitch_filter.cpp` (apply_glitch_filter_one_pass).
        """
        mcp.connect_device(device_id)
        mcp.configure_glitch_filter(channels=[])

        baseline = _capture_sc_edges(mcp, device_id, threshold=I2C_SPB // 10)
        with_big_filter = _capture_sc_edges(mcp, device_id,
                                            threshold=I2C_SPB + 10)  # 60 > 50
        assert baseline > 100, f"baseline bus absent: {baseline} edges"
        # get_samples bypasses the glitch filter -> same raw edge count.
        assert with_big_filter == baseline, \
            f"get_samples changed under a filter (raw should be invariant): " \
            f"no-filter={baseline}, big-filter={with_big_filter}"
        mcp.configure_glitch_filter(channels=[])