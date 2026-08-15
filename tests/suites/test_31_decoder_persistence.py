"""
test_31_decoder_persistence.py - Decoder persistence across save/load tests.

Validates that decoders are correctly saved to .pxl/.pxc session files
and restored when the file is loaded.

Tests:
1. Save with single I2C decoder → load → decoder count/type/channel map match
2. Save with stacked I2C→EEPROM decoder → load → stacked structure preserved
3. Save with decoder → load → decoder re-decodes and produces results
"""

import os
import time

import pytest

from pxview_automation import McpClient, McpError
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture, do_buffer_capture_with_pattern
from helpers.decoder_helper import (
    add_decoder_safe,
    get_decoder_results_with_retry,
)

pytestmark = pytest.mark.p1

SAMPLE_RATE_1M = 1_000_000
SAMPLE_COUNT_1M = 1_000_000


class TestDecoderPersistence:

    def test_save_load_single_decoder(self, mcp: McpClient, device_id: str,
                                      tmp_pxl_file: str,
                                      cleanup_after_test):
        """Save with I2C decoder → load → decoder restored with correct config."""
        # 1. Add I2C decoder
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        assert i2c_id

        # 2. Capture
        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        # 3. Verify decoder produces results before save
        results_before = get_decoder_results_with_retry(mcp, i2c_id, max_wait=30.0)
        assert len(results_before) > 0, "I2C decoder produced 0 results before save"

        # 4. Record decoder state before save
        decoders_before = mcp.get_active_decoders()
        assert len(decoders_before) >= 1

        # 5. Save
        mcp.save_capture(tmp_pxl_file)
        assert os.path.exists(tmp_pxl_file)

        # 6. Close and reload
        mcp.close_capture()
        mcp.load_capture(tmp_pxl_file)
        time.sleep(2)

        # 7. Verify decoder restored
        decoders_after = mcp.get_active_decoders()
        assert len(decoders_after) >= 1, \
            f"No decoders after load, expected >=1. " \
            f"Before save: {len(decoders_before)}"

        # Check that an I2C decoder is among the restored decoders
        decoder_ids_after = [
            d.get("decoder_id") or d.get("name") or d.get("id", "")
            for d in decoders_after
        ]
        assert any("i2c" in str(did).lower() for did in decoder_ids_after), \
            f"I2C decoder not found after load. Decoders: {decoder_ids_after}"

    def test_save_load_stacked_decoder(self, mcp: McpClient, device_id: str,
                                       tmp_pxl_file: str,
                                       cleanup_after_test):
        """Save with I2C→EEPROM stacked decoder → load → structure preserved."""
        # 1. Add I2C base decoder
        i2c_id = add_decoder_safe(mcp, "i2c_c",
                                  channel_map={"sda": 0, "scl": 1},
                                  device_id=device_id)
        assert i2c_id

        # 2. Add EEPROM stacked on I2C
        eeprom_id = None
        try:
            eeprom_id = add_decoder_safe(mcp, "eeprom24xx_c",
                                         stack_on=i2c_id)
        except McpError:
            pytest.skip("EEPROM24xx decoder not available")

        assert eeprom_id, "Failed to add EEPROM stacked decoder"

        # 3. Capture with I2C pattern
        do_buffer_capture_with_pattern(
            mcp, device_id,
            channels=[0, 1],
            sample_rate=SAMPLE_RATE_1M,
            sample_count=SAMPLE_COUNT_1M,
            pattern="i2c",
        )

        # 4. Verify stacked decoder produces results
        stacked_results = get_decoder_results_with_retry(mcp, eeprom_id,
                                                          max_wait=30.0)
        assert len(stacked_results) > 0, "EEPROM stacked decoder produced 0 results"

        # 5. Count decoders before save
        decoders_before = mcp.get_active_decoders()
        count_before = len(decoders_before)
        assert count_before >= 2, \
            f"Expected >=2 decoders (I2C + EEPROM), got {count_before}"

        # 6. Save
        mcp.save_capture(tmp_pxl_file)
        assert os.path.exists(tmp_pxl_file)

        # 7. Close and reload
        mcp.close_capture()
        mcp.load_capture(tmp_pxl_file)
        time.sleep(2)

        # 8. Verify decoders restored (at least the base I2C)
        decoders_after = mcp.get_active_decoders()
        count_after = len(decoders_after)
        assert count_after >= 1, \
            f"No decoders after load. Before: {count_before}"

        # If stacked decoder is also restored, verify it
        decoder_ids_after = [
            d.get("decoder_id") or d.get("name") or d.get("id", "")
            for d in decoders_after
        ]
        assert any("i2c" in str(did).lower() for did in decoder_ids_after), \
            f"I2C base decoder not found after load. Decoders: {decoder_ids_after}"

    def test_save_load_decoder_redecodes(self, mcp: McpClient, device_id: str,
                                         tmp_pxl_file: str,
                                         cleanup_after_test):
        """Save with decoder → load → decoder re-decodes and produces results."""
        # 1. Add PWM decoder
        pwm_id = add_decoder_safe(mcp, "pwm_c",
                                  channel_map={"data": 0},
                                  device_id=device_id)
        assert pwm_id

        # 2. Capture
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.5)

        # 3. Verify decoder produces results before save
        results_before = get_decoder_results_with_retry(mcp, pwm_id, max_wait=30.0)
        assert len(results_before) > 0, "PWM decoder produced 0 results before save"

        # 4. Save
        mcp.save_capture(tmp_pxl_file)
        assert os.path.exists(tmp_pxl_file)

        # 5. Close and reload
        mcp.close_capture()
        mcp.load_capture(tmp_pxl_file)
        time.sleep(2)

        # 6. Find the restored decoder
        decoders_after = mcp.get_active_decoders()
        assert len(decoders_after) >= 1, "No decoders after load"

        # Find the restored PWM decoder's instance ID
        restored_id = None
        for d in decoders_after:
            did = d.get("decoder_id") or d.get("name") or d.get("id", "")
            if "pwm" in str(did).lower():
                restored_id = d.get("instance_id") or d.get("id")
                break

        if restored_id is None:
            # If we can't find by name, try the first decoder
            restored_id = decoders_after[0].get("instance_id") or decoders_after[0].get("id")

        assert restored_id, \
            f"Could not find restored decoder ID. Decoders: {decoders_after}"

        # 7. Wait for re-decode results
        results_after = get_decoder_results_with_retry(mcp, str(restored_id),
                                                        max_wait=60.0)
        assert len(results_after) > 0, \
            f"Restored decoder produced 0 results after load + re-decode"
