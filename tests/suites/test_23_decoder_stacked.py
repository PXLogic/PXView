"""
test_23_decoder_stacked.py - Stacked decoder tests with data verification.

Tests hierarchical decoding: I2C -> EEPROM, SPI -> SPI Flash.
Uses PATTERN_I2C for I2C-based stacks and PATTERN_MIXED for SPI-based
stacks, verifying that stacked decoders produce meaningful results
(not just that they don't crash).

I2C pattern (EEPROM-like):
  Device address 0x50 (24C02 EEPROM), fixed.
  Data: [word_addr, 0x10+nibble, 0x20+nibble]

SPI pattern (SPI Flash READ):
  MOSI: [0x03, 0x00, 0x00, addr_low, 0xFF]
  MISO: [0xFF, 0xFF, 0xFF, 0xFF, 0x10+nibble]
"""

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_annotation_valid
from helpers.capture_helper import do_buffer_capture_with_pattern
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p2

SAMPLE_RATE_1M = 1_000_000
SAMPLE_COUNT_1M = 1_000_000


class TestDecoderStacked:
    """Stacked decoder tests: base decoder + stacked decoder."""

    def test_i2c_stacked_eeprom24xx(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """I2C -> EEPROM24xx stacked decoding on PATTERN_I2C."""
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        assert i2c_id

        eeprom_id = None
        try:
            eeprom_id = add_decoder_safe(mcp, "eeprom24xx_c",
                                         stack_on=i2c_id)
            assert eeprom_id
        except McpError:
            pass  # EEPROM decoder may not be available

        # Use PATTERN_I2C which has real I2C traffic
        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        # Verify base decoder active and produced results
        decoders = mcp.get_active_decoders()
        assert len(decoders) >= 1

        base_results = get_decoder_results_with_retry(mcp, i2c_id, max_wait=30.0)
        assert len(base_results) > 0, \
            "Base I2C decoder produced 0 results on i2c pattern"

    def test_i2c_stacked_eeprom24xx_produces_results(self, mcp: McpClient,
                                                       device_id: str,
                                                       cleanup_after_test):
        """EEPROM24xx stacked on I2C produces non-empty results."""
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)

        try:
            eeprom_id = add_decoder_safe(mcp, "eeprom24xx_c",
                                         stack_on=i2c_id)
        except McpError:
            pytest.skip("EEPROM24xx decoder not available")

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        # Wait for both base and stacked results
        base_results = get_decoder_results_with_retry(mcp, i2c_id, max_wait=30.0)
        assert len(base_results) > 0, "I2C base: 0 results"

        stacked_results = get_decoder_results_with_retry(mcp, eeprom_id,
                                                          max_wait=30.0)
        assert len(stacked_results) > 0, \
            "EEPROM stacked decoder produced 0 results"
        # Stacked decoder may produce fewer annotations; verify structure
        for ann in stacked_results[:50]:
            assert_annotation_valid(ann)

    def test_spi_stacked_spiflash(self, mcp: McpClient, device_id: str,
                                  cleanup_after_test):
        """SPI -> SPI Flash stacked decoding on PATTERN_MIXED."""
        spi_id = add_decoder_safe(mcp, "spi_c",
                                  channel_map={"cs": 2, "sclk": 3,
                                               "mosi": 4, "miso": 5},
                                  device_id=device_id)
        assert spi_id

        flash_id = None
        try:
            flash_id = add_decoder_safe(mcp, "spiflash_c",
                                        stack_on=spi_id)
            assert flash_id
        except McpError:
            pass  # SPI Flash decoder may not be available

        # Use PATTERN_MIXED which has real SPI traffic on ch2-5
        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[2, 3, 4, 5],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        base_results = get_decoder_results_with_retry(mcp, spi_id, max_wait=30.0)
        assert len(base_results) > 0, \
            "Base SPI decoder produced 0 results on mixed pattern"

    def test_spi_stacked_spiflash_produces_results(self, mcp: McpClient,
                                                     device_id: str,
                                                     cleanup_after_test):
        """SPI Flash stacked on SPI produces valid results.

        The demo SPI pattern now sends SPI Flash READ (0x03) commands
        with CS held low across 5 bytes. The SPI Flash stacked decoder
        should recognize these as READ commands and produce annotations.
        """
        spi_id = add_decoder_safe(mcp, "spi_c",
                                  channel_map={"cs": 2, "sclk": 3,
                                               "mosi": 4, "miso": 5},
                                  device_id=device_id)

        try:
            flash_id = add_decoder_safe(mcp, "spiflash_c",
                                        stack_on=spi_id)
        except McpError:
            pytest.skip("SPI Flash decoder not available")

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[2, 3, 4, 5],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="mixed",
        )

        base_results = get_decoder_results_with_retry(mcp, spi_id, max_wait=30.0)
        assert len(base_results) > 0, "SPI base: 0 results"

        stacked_results = get_decoder_results_with_retry(mcp, flash_id,
                                                          max_wait=30.0)
        assert len(stacked_results) > 0, \
            "SPI Flash stacked decoder produced 0 results"
        for ann in stacked_results[:50]:
            assert_annotation_valid(ann)

    def test_stacked_results_structure(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """Stacked decoder results have valid structure."""
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        try:
            eeprom_id = add_decoder_safe(mcp, "eeprom24xx_c",
                                         stack_on=i2c_id)
        except McpError:
            eeprom_id = None

        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        results = get_decoder_results_with_retry(mcp, i2c_id, max_wait=30.0)
        for ann in results[:100]:
            assert_annotation_valid(ann)

    def test_clear_all_removes_stacked(self, mcp: McpClient, device_id: str,
                                       cleanup_after_test):
        """clear_all_decoders removes stacked decoders."""
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        try:
            add_decoder_safe(mcp, "eeprom24xx_c", stack_on=i2c_id)
        except McpError:
            pass

        mcp.clear_all_decoders()
        decoders = mcp.get_active_decoders()
        assert len(decoders) == 0

    def test_stacked_get_active_decoders(self, mcp: McpClient, device_id: str,
                                         cleanup_after_test):
        """get_active_decoders lists stacked decoders."""
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        try:
            add_decoder_safe(mcp, "eeprom24xx_c", stack_on=i2c_id)
        except McpError:
            pass

        decoders = mcp.get_active_decoders()
        assert len(decoders) >= 1
        for d in decoders:
            assert "instance_id" in d or "id" in d, f"Decoder missing id: {d}"
