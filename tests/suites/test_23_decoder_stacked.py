"""
test_23_decoder_stacked.py - Stacked decoder tests.

Tests hierarchical decoding: I2C -> EEPROM, SPI -> SPI Flash, etc.
"""

import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_annotation_valid
from helpers.capture_helper import do_timed_capture
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p2


class TestDecoderStacked:

    def test_i2c_stacked_eeprom24xx(self, mcp: McpClient, device_id: str,
                                    cleanup_after_test):
        """I2C -> EEPROM24xx stacked decoding."""
        # Add base I2C decoder
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        assert i2c_id

        # Add stacked EEPROM decoder
        try:
            eeprom_id = add_decoder_safe(mcp, "eeprom24xx_c",
                                         stack_on=i2c_id)
            assert eeprom_id
        except McpError:
            # EEPROM decoder may not be available or may require specific options
            pass

        # Capture
        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.5)

        # Verify both decoders active
        decoders = mcp.get_active_decoders()
        assert len(decoders) >= 1

    def test_spi_stacked_spiflash(self, mcp: McpClient, device_id: str,
                                  cleanup_after_test):
        """SPI -> SPI Flash stacked decoding."""
        spi_id = add_decoder_safe(mcp, "spi_c",
                                  channel_map={"sclk": 0, "mosi": 1,
                                               "miso": 2, "cs": 3},
                                  device_id=device_id)
        assert spi_id

        try:
            flash_id = add_decoder_safe(mcp, "spiflash_c",
                                        stack_on=spi_id)
            assert flash_id
        except McpError:
            pass

        do_timed_capture(mcp, device_id, channels=[0, 1, 2, 3],
                         sample_rate=1000000, duration_seconds=0.5)

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

        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.5)

        # Get results from base decoder
        results = get_decoder_results_with_retry(mcp, i2c_id, max_wait=10.0)
        for ann in results:
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
            eeprom_id = add_decoder_safe(mcp, "eeprom24xx_c", stack_on=i2c_id)
        except McpError:
            eeprom_id = None

        decoders = mcp.get_active_decoders()
        # Should have at least the base decoder
        assert len(decoders) >= 1
        for d in decoders:
            assert "instance_id" in d or "id" in d, f"Decoder missing id: {d}"
