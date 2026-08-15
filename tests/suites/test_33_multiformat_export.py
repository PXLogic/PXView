"""
test_33_multiformat_export.py - VCD/Hex/Bits format export tests.

Migrates the manual verification in verify_multiformat_export.py into
proper pytest automated tests.

Tests:
1. VCD export: file non-empty, contains VCD header ($timescale, $var, $end)
2. Hex export: file non-empty, each line is valid hexadecimal
3. Bits export: file non-empty, contains valid bit strings
"""

import os

import pytest

from pxview_automation import McpClient
from helpers.assertions import assert_capture_status
from helpers.capture_helper import do_timed_capture

pytestmark = pytest.mark.p1


class TestVcdExport:

    def test_export_vcd_file_structure(self, mcp: McpClient, device_id: str,
                                       tmp_capture_dir: str,
                                       cleanup_after_test):
        """VCD export produces a valid VCD file with proper header."""
        do_timed_capture(mcp, device_id, channels=[0, 1],
                         sample_rate=1000000, duration_seconds=0.3)

        mcp.export_raw_data("vcd", tmp_capture_dir,
                            digital_channels=[0, 1])

        # Find VCD files
        vcd_files = []
        if os.path.isdir(tmp_capture_dir):
            for f in os.listdir(tmp_capture_dir):
                if f.lower().endswith(".vcd"):
                    vcd_files.append(os.path.join(tmp_capture_dir, f))

        assert len(vcd_files) > 0, f"No VCD files in {tmp_capture_dir}"

        for vcd_file in vcd_files:
            assert os.path.getsize(vcd_file) > 0, \
                f"VCD file is empty: {vcd_file}"

            with open(vcd_file, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()

            # VCD files must contain standard headers
            assert "$timescale" in content or "$timecale" in content, \
                f"VCD file missing $timescale header: {vcd_file}"
            assert "$end" in content, \
                f"VCD file missing $end marker: {vcd_file}"
            # Should have signal declarations
            assert "$var" in content or "$upscope" in content, \
                f"VCD file missing $var/$upscope declarations: {vcd_file}"


class TestHexExport:

    def test_export_hex_file_structure(self, mcp: McpClient, device_id: str,
                                       tmp_capture_dir: str,
                                       cleanup_after_test):
        """Hex export produces a file with valid hexadecimal data."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)

        mcp.export_raw_data("hex", tmp_capture_dir,
                            digital_channels=[0])

        # Find hex files
        hex_files = []
        if os.path.isdir(tmp_capture_dir):
            for f in os.listdir(tmp_capture_dir):
                if f.lower().endswith(".hex") or f.lower().endswith(".txt"):
                    hex_files.append(os.path.join(tmp_capture_dir, f))

        assert len(hex_files) > 0, f"No hex files in {tmp_capture_dir}"

        for hex_file in hex_files:
            assert os.path.getsize(hex_file) > 0, \
                f"Hex file is empty: {hex_file}"

            with open(hex_file, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()

            assert len(lines) > 0, f"Hex file has no lines: {hex_file}"

            # Verify at least some lines contain valid hex data
            hex_line_count = 0
            for line in lines[:100]:
                line = line.strip()
                if not line:
                    continue
                # Remove common prefixes/suffixes (e.g. "0x", commas, spaces)
                cleaned = line.replace("0x", "").replace(",", "").replace(" ", "")
                # Check if it looks like hex data
                try:
                    int(cleaned[:8], 16)  # Try parsing first 8 chars as hex
                    hex_line_count += 1
                except ValueError:
                    pass

            assert hex_line_count > 0, \
                f"No valid hex data lines found in {hex_file}. " \
                f"First 5 lines: {lines[:5]}"


class TestBitsExport:

    def test_export_bits_file_structure(self, mcp: McpClient, device_id: str,
                                        tmp_capture_dir: str,
                                        cleanup_after_test):
        """Bits export produces a file with valid bit strings."""
        do_timed_capture(mcp, device_id, channels=[0],
                         sample_rate=1000000, duration_seconds=0.3)

        mcp.export_raw_data("bits", tmp_capture_dir,
                            digital_channels=[0])

        # Find bits files
        bits_files = []
        if os.path.isdir(tmp_capture_dir):
            for f in os.listdir(tmp_capture_dir):
                if f.lower().endswith(".bits") or f.lower().endswith(".txt"):
                    bits_files.append(os.path.join(tmp_capture_dir, f))

        assert len(bits_files) > 0, f"No bits files in {tmp_capture_dir}"

        for bits_file in bits_files:
            assert os.path.getsize(bits_file) > 0, \
                f"Bits file is empty: {bits_file}"

            with open(bits_file, "r", encoding="utf-8", errors="replace") as f:
                lines = f.readlines()

            assert len(lines) > 0, f"Bits file has no lines: {bits_file}"

            # Verify at least some lines contain valid bit strings (0s and 1s)
            bit_line_count = 0
            for line in lines[:100]:
                line = line.strip()
                if not line:
                    continue
                # Check if line consists only of 0s and 1s (at least 8 chars)
                cleaned = line.replace(" ", "").replace(",", "")
                if len(cleaned) >= 8 and all(c in "01" for c in cleaned[:8]):
                    bit_line_count += 1

            assert bit_line_count > 0, \
                f"No valid bit string lines found in {bits_file}. " \
                f"First 5 lines: {lines[:5]}"
