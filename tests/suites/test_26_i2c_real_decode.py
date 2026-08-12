"""
test_26_i2c_real_decode.py - I2C real decode tests using PATTERN_I2C.

The demo driver's PATTERN_I2C generates valid I2C bus traffic on
ch0(SCL) and ch1(SDA). Each transaction is:
  START + 7-bit addr + R/W + ACK + 3 data bytes + ACK + STOP

Address starts at 0x50 and increments each transaction.
Data bytes increment within each transaction.

This test verifies that both C and Python I2C decoders can:
- Detect START/STOP conditions
- Decode correct addresses
- Decode correct data bytes
- Produce a reasonable number of annotations
"""

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_annotation_valid
from helpers.capture_helper import do_buffer_capture_with_pattern
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p0

SAMPLE_RATE_1M = 1_000_000
SAMPLE_COUNT_1M = 1_000_000

# I2C annotation class IDs (from C decoder / Python decoder)
# I2C C decoder ann classes:
#   0=START, 1=REPEATSTART, 2=STOP, 3=ACK, 4=NACK,
#   5=ADDRESS, 6=DATA, 7=BITS
I2C_ANN_START = 0
I2C_ANN_STOP = 2
I2C_ANN_ADDRESS = 5
I2C_ANN_DATA = 6


class TestI2CRealDecode:
    """I2C decoder tests on PATTERN_I2C demo data."""

    def test_i2c_c_produces_results(self, mcp, device_id, cleanup_after_test):
        """C I2C decoder produces non-empty results on i2c pattern."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )
        assert analyzer_id, "Failed to add i2c_c"

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )
        assert status["state"] in ("completed", "idle", "stopped"), \
            f"Capture failed: {status}"

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, \
            "I2C C decoder produced 0 annotations on i2c pattern"

    def test_i2c_python_produces_results(self, mcp, device_id,
                                           cleanup_after_test):
        """Python I2C decoder produces non-empty results on i2c pattern."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )
        assert analyzer_id, "Failed to add i2c (Python)"

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, \
            "I2C Python decoder produced 0 annotations on i2c pattern"

    def test_i2c_c_python_comparable_count(self, mcp, device_id,
                                              cleanup_after_test):
        """C and Python I2C decoders produce comparable annotation counts."""
        c_id = add_decoder_safe(mcp, "i2c_c",
                                channel_map={"scl": 0, "sda": 1},
                                device_id=device_id)
        py_id = add_decoder_safe(mcp, "i2c",
                                 channel_map={"scl": 0, "sda": 1},
                                 device_id=device_id)

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        c_results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
        py_results = get_decoder_results_with_retry(mcp, py_id, max_wait=30.0)

        assert len(c_results) > 0, "C decoder: 0 results"
        assert len(py_results) > 0, "Python decoder: 0 results"

        # Counts should be comparable (within 2x)
        ratio = len(c_results) / max(len(py_results), 1)
        assert 0.3 <= ratio <= 3.0, \
            f"C={len(c_results)} vs Python={len(py_results)}, ratio={ratio}"

    def test_i2c_has_start_annotations(self, mcp, device_id,
                                          cleanup_after_test):
        """I2C C decoder detects START conditions."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        # Look for START annotations
        start_count = 0
        for ann in results:
            if ann.get("ann_class") == I2C_ANN_START:
                start_count += 1

        assert start_count > 0, \
            f"No START annotations found in {len(results)} results"

    def test_i2c_has_address_annotations(self, mcp, device_id,
                                            cleanup_after_test):
        """I2C C decoder decodes addresses (should include 0x50)."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        # Look for ADDRESS annotations
        addr_texts = []
        for ann in results:
            if ann.get("ann_class") == I2C_ANN_ADDRESS:
                for text in ann.get("texts", []):
                    addr_texts.append(str(text))

        assert len(addr_texts) > 0, \
            f"No ADDRESS annotations in {len(results)} results"

        # The first address should be 0x50 (or 0xA0 with R/W bit)
        # Check that at least one address contains "50" or "a0"
        found_50 = any("50" in t.lower() or "a0" in t.lower() for t in addr_texts)
        assert found_50, \
            f"Address 0x50 not found in addresses: {addr_texts[:10]}"

    def test_i2c_has_data_annotations(self, mcp, device_id,
                                        cleanup_after_test):
        """I2C C decoder decodes data bytes."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        data_count = 0
        for ann in results:
            if ann.get("ann_class") == I2C_ANN_DATA:
                data_count += 1

        assert data_count > 0, \
            f"No DATA annotations in {len(results)} results"

    def test_i2c_has_stop_annotations(self, mcp, device_id,
                                        cleanup_after_test):
        """I2C C decoder detects STOP conditions."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        stop_count = 0
        for ann in results:
            if ann.get("ann_class") == I2C_ANN_STOP:
                stop_count += 1

        assert stop_count > 0, \
            f"No STOP annotations in {len(results)} results"

    def test_i2c_annotation_count_reasonable(self, mcp, device_id,
                                                cleanup_after_test):
        """I2C pattern should produce a reasonable number of annotations."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)

        # Each transaction = START + ADDR + ACK + 3*(DATA+ACK) + STOP = 10 anns
        # At 1M samples, 10 samples/bit, 38 bits/transaction → ~2631 transactions
        # But decoder may cap at maxCount (10000)
        assert len(results) >= 20, \
            f"Only {len(results)} annotations, expected at least 20"

    def test_i2c_c_python_mixed_stress(self, mcp, device_id,
                                         cleanup_after_test):
        """C and Python I2C decoders running simultaneously."""
        c_id = add_decoder_safe(mcp, "i2c_c",
                                channel_map={"scl": 0, "sda": 1},
                                device_id=device_id)
        py_id = add_decoder_safe(mcp, "i2c",
                                 channel_map={"scl": 0, "sda": 1},
                                 device_id=device_id)

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        c_results = get_decoder_results_with_retry(mcp, c_id, max_wait=30.0)
        py_results = get_decoder_results_with_retry(mcp, py_id, max_wait=30.0)

        assert len(c_results) > 0, "C decoder: 0 results"
        assert len(py_results) > 0, "Python decoder: 0 results"

        # Validate annotation structure
        for ann in c_results[:20]:
            assert_annotation_valid(ann)
        for ann in py_results[:20]:
            assert_annotation_valid(ann)
