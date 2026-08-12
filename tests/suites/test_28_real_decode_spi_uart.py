"""
test_28_real_decode_spi_uart.py - Real decode verification for SPI and UART.

Uses the demo driver's PATTERN_MIXED which generates real protocol traffic:
  ch0-1:   I2C  (SCL, SDA)
  ch2-5:   SPI  (CS, SCLK, MOSI, MISO)
  ch6:     UART (RX)
  ch7:     CAN  (RX)

SPI pattern: each frame = SPI Flash READ command (50 bit-times).
  CS held LOW for 5 bytes: [0x03, 0x00, 0x00, addr_low, dummy].
  Then CS HIGH for 10 idle bits.
  MOSI sends: 0x03 (READ cmd), 0x00, 0x00, addr_low, 0xFF (dummy).
  MISO returns: 0xFF, 0xFF, 0xFF, 0xFF, 0x10+(frame&0x0F) (data).
  Frame 0: addr=0x000000, data=0x10.
  Frame 1: addr=0x000001, data=0x11.
  Frame 2: addr=0x000002, data=0x12. ...

UART pattern: each frame = 10 bit-times (1 start + 8 data LSB-first + 1 stop).
  Start=0, Stop=1, Data = (frame_number ^ 0xAA) & 0xFF.
  So frame 0 = 0xAA, frame 1 = 0xAB, frame 2 = 0xA8, ...

This test verifies that C decoders produce non-empty results AND
that the decoded content matches the known pattern values.
"""

import re

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


# ======================================================================
# SPI real decode tests — PATTERN_MIXED ch2-5
# ======================================================================

class TestSpiRealDecode:
    """SPI C decoder on PATTERN_MIXED ch2-5 (CS, SCLK, MOSI, MISO).

    The demo SPI pattern sends SPI Flash READ commands:
      Frame N: [0x03, 0x00, 0x00, N&0xFF, 0xFF] on MOSI
               [0xFF, 0xFF, 0xFF, 0xFF, 0x10+(N&0x0F)] on MISO
    Each frame is a complete READ transaction with CS assertion.
    """

    def test_spi_c_produces_results(self, mcp, device_id, cleanup_after_test):
        """SPI C decoder produces non-empty results on mixed pattern."""
        analyzer_id = add_decoder_safe(
            mcp, "spi_c",
            channel_map={"cs": 2, "sclk": 3, "mosi": 4, "miso": 5},
            device_id=device_id,
        )
        assert analyzer_id, "Failed to add spi_c"

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[2, 3, 4, 5],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )
        assert status["state"] in ("completed", "idle", "stopped"), \
            f"Capture failed: {status}"

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, \
            "SPI C decoder produced 0 annotations on mixed pattern"

    def test_spi_c_annotation_count_reasonable(self, mcp, device_id,
                                                cleanup_after_test):
        """SPI on 1M mixed samples should produce at least 20 annotations."""
        analyzer_id = add_decoder_safe(
            mcp, "spi_c",
            channel_map={"cs": 2, "sclk": 3, "mosi": 4, "miso": 5},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[2, 3, 4, 5],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) >= 20, \
            f"SPI only produced {len(results)} annotations, expected >= 20"

    def test_spi_c_decodes_flash_read_command(self, mcp, device_id,
                                               cleanup_after_test):
        """SPI C decoder decodes SPI Flash READ command bytes.

        The demo SPI pattern sends SPI Flash READ (0x03) commands.
        First MOSI byte should be 0x03 (READ command code).
        We extract hex values and verify 0x03 appears as the first byte.
        """
        analyzer_id = add_decoder_safe(
            mcp, "spi_c",
            channel_map={"cs": 2, "sclk": 3, "mosi": 4, "miso": 5},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[2, 3, 4, 5],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, "No SPI results"

        # Extract hex data values from annotation texts
        hex_values = []
        for ann in results:
            for text in ann.get("texts", []):
                text_str = str(text).strip()
                # Look for hex values like "0x03", "03", "0x00", etc.
                hex_match = re.search(r'0?x?([0-9A-Fa-f]{2})', text_str)
                if hex_match:
                    try:
                        val = int(hex_match.group(1), 16)
                        if 0 <= val <= 255:
                            hex_values.append(val)
                    except ValueError:
                        pass

        assert len(hex_values) > 0, \
            f"No hex data values found in {len(results)} SPI annotations"

        # The first byte should be 0x03 (SPI Flash READ command)
        assert hex_values[0] == 0x03, \
            f"First SPI byte should be 0x03 (READ cmd), got 0x{hex_values[0]:02X}. " \
            f"First 20 values: {[f'0x{v:02X}' for v in hex_values[:20]]}"

        # 0x03 should appear repeatedly (once per READ command frame)
        read_cmd_count = sum(1 for v in hex_values if v == 0x03)
        assert read_cmd_count >= 5, \
            f"Expected >= 5 READ commands (0x03), found {read_cmd_count}. " \
            f"First 20 values: {[f'0x{v:02X}' for v in hex_values[:20]]}"

    def test_spi_c_annotation_structure_valid(self, mcp, device_id,
                                               cleanup_after_test):
        """All SPI annotations have valid structure."""
        analyzer_id = add_decoder_safe(
            mcp, "spi_c",
            channel_map={"cs": 2, "sclk": 3, "mosi": 4, "miso": 5},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[2, 3, 4, 5],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        for ann in results[:100]:  # Check first 100
            assert_annotation_valid(ann)


# ======================================================================
# UART real decode tests — PATTERN_MIXED ch6
# ======================================================================

class TestUartRealDecode:
    """UART C decoder on PATTERN_MIXED ch6 (RX).

    The demo UART pattern sends frames with data = (frame_num ^ 0xAA).
    Frame 0: 0xAA, Frame 1: 0xAB, Frame 2: 0xA8, Frame 3: 0xA9, ...
    Each frame: start bit (0) + 8 data bits (LSB first) + stop bit (1).
    """

    def test_uart_c_produces_results(self, mcp, device_id, cleanup_after_test):
        """UART C decoder produces non-empty results on mixed pattern."""
        analyzer_id = add_decoder_safe(
            mcp, "uart_c",
            channel_map={"rx": 6},
            device_id=device_id,
        )
        assert analyzer_id, "Failed to add uart_c"

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[6],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )
        assert status["state"] in ("completed", "idle", "stopped"), \
            f"Capture failed: {status}"

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, \
            "UART C decoder produced 0 annotations on mixed pattern"

    def test_uart_c_annotation_count_reasonable(self, mcp, device_id,
                                                  cleanup_after_test):
        """UART on 1M mixed samples should produce at least 20 annotations."""
        analyzer_id = add_decoder_safe(
            mcp, "uart_c",
            channel_map={"rx": 6},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[6],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) >= 20, \
            f"UART only produced {len(results)} annotations, expected >= 20"

    def test_uart_c_decodes_xor_aa_pattern(self, mcp, device_id,
                                            cleanup_after_test):
        """UART C decoder decodes data bytes matching (frame ^ 0xAA) pattern.

        The demo UART sends: 0xAA, 0xAB, 0xA8, 0xA9, 0xAE, 0xAF, 0xAC, 0xAD, ...
        We extract hex values and verify the first value is 0xAA.
        """
        analyzer_id = add_decoder_safe(
            mcp, "uart_c",
            channel_map={"rx": 6},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[6],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, "No UART results"

        # Extract hex data values from annotation texts
        hex_values = []
        for ann in results:
            for text in ann.get("texts", []):
                text_str = str(text).strip()
                hex_match = re.search(r'0?x?([0-9A-Fa-f]{2})', text_str)
                if hex_match:
                    try:
                        val = int(hex_match.group(1), 16)
                        if 0 <= val <= 255:
                            hex_values.append(val)
                    except ValueError:
                        pass

        assert len(hex_values) > 0, \
            f"No hex data values found in {len(results)} UART annotations"

        # The first data byte should be 0xAA (frame 0 ^ 0xAA = 0xAA)
        assert hex_values[0] == 0xAA, \
            f"First UART byte should be 0xAA (frame 0), got 0x{hex_values[0]:02X}. " \
            f"First 20 values: {[f'0x{v:02X}' for v in hex_values[:20]]}"

    def test_uart_c_annotation_structure_valid(self, mcp, device_id,
                                                cleanup_after_test):
        """All UART annotations have valid structure."""
        analyzer_id = add_decoder_safe(
            mcp, "uart_c",
            channel_map={"rx": 6},
            device_id=device_id,
        )

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[6],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        for ann in results[:100]:
            assert_annotation_valid(ann)


# ======================================================================
# I2C real decode on PATTERN_MIXED — complement to test_26
# ======================================================================

class TestI2COnMixedRealDecode:
    """I2C C decoder on PATTERN_MIXED ch0-1 (SCL, SDA).

    This complements test_26 which uses the dedicated 'i2c' pattern.
    Here we use the 'mixed' pattern which also includes I2C on ch0-1.
    """

    def test_i2c_c_produces_results_on_mixed(self, mcp, device_id,
                                              cleanup_after_test):
        """I2C C decoder produces results on mixed pattern ch0-1."""
        analyzer_id = add_decoder_safe(
            mcp, "i2c_c",
            channel_map={"scl": 0, "sda": 1},
            device_id=device_id,
        )
        assert analyzer_id

        status = do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )
        assert status["state"] in ("completed", "idle", "stopped")

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0, \
            "I2C C decoder produced 0 annotations on mixed pattern"

    def test_i2c_c_has_start_annotations_on_mixed(self, mcp, device_id,
                                                    cleanup_after_test):
        """I2C C decoder detects START conditions on mixed pattern."""
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
            pattern="mixed",
        )

        results = get_decoder_results_with_retry(mcp, analyzer_id, max_wait=30.0)
        assert len(results) > 0

        # I2C ann_class 0 = START
        start_count = sum(1 for ann in results if ann.get("ann_class") == 0)
        assert start_count > 0, \
            f"No START annotations in {len(results)} I2C results"
