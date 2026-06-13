#!/usr/bin/env python3
"""
generate_pxl_bit15_test.py - Generate a .pxl file to verify the Bit15 bug
in the numbers_and_state decoder.

Bug: In pd.py decode(), the line:
    channels = [ch for ch in range(_max_channels) if self.has_channel(ch)]
uses range(16), which only checks indices 0..15. But because clk occupies
index 0, Bit15 sits at index 16 and is NEVER checked.

This script generates a .pxl file with 17 logic channels (clk + bit0..bit15).
The data contains several 16-bit values where Bit15 is set (0x8000, 0xFFFF, etc.).
If the bug exists, the decoder will show wrong values for these patterns.

Usage:
    python generate_pxl_bit15_test.py
    # Output: test_bit15_bug.pxl
    # Open this file in PXView, add numbers_and_state decoder,
    # connect all 17 channels (clk + Bit0..Bit15), set count=16, check results.
"""

import json
import math
import os
import struct
import zipfile

# ── Configuration ──────────────────────────────────────────────────────
SAMPLE_RATE = 1000000       # 1 MHz
CLOCK_PERIOD = 20           # 20 samples per clock cycle (50 kHz clock)
NUM_CHANNELS = 17           # ch0=clk, ch1=bit0, ..., ch16=bit15
SAMPLE_COUNT = 2000         # total samples (enough for our test patterns)

# Test values to encode - each will be stable for one clock period
# These values specifically exercise Bit15 (the MSB of a 16-bit number)
TEST_VALUES = [
    0x0000,   # 0      - all bits low (baseline)
    0x0001,   # 1      - only Bit0
    0x00FF,   # 255    - lower 8 bits
    0x0100,   # 256    - Bit8
    0x7FFF,   # 32767  - all bits except Bit15
    0x8000,   # 32768  - ONLY Bit15 (the bug case!)
    0xFFFF,   # 65535  - all 16 bits
    0xAAAA,   # 43690  - alternating 1010...
    0x5555,   # 21845  - alternating 0101...
    0x1234,   # 4660   - arbitrary pattern
]

OUTPUT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "test_bit15_bug.pxl")


def samples_to_bitpacked(channel_data):
    """Convert a list of 0/1 sample values to bit-packed bytes.
    bit 0 of byte 0 = sample 0, bit 1 of byte 0 = sample 1, etc.
    """
    num_bytes = math.ceil(len(channel_data) / 8)
    result = bytearray(num_bytes)
    for i, val in enumerate(channel_data):
        if val:
            result[i // 8] |= (1 << (i % 8))
    return bytes(result)


def generate_header():
    """Generate the header INI text for the .pxl file."""
    lines = []
    lines.append("[version]")
    lines.append("version = 3")
    lines.append("")
    lines.append("[header]")
    lines.append("driver = DSLogic")
    lines.append("device mode = 0")
    lines.append("capturefile = data")
    lines.append(f"total samples = {SAMPLE_COUNT}")
    lines.append(f"total probes = {NUM_CHANNELS}")
    lines.append("total blocks = 1")
    lines.append(f"samplerate = {SAMPLE_RATE // 1000000} MHz")
    lines.append("trigger time = 0")
    lines.append("trigger pos = 0")

    # Channel names: ch0=clk, ch1=Bit0, ..., ch16=Bit15
    lines.append("probe0 = clk")
    for i in range(16):
        lines.append(f"probe{i+1} = Bit{i}")

    return "\n".join(lines) + "\n"


def generate_session():
    """Generate the session JSON for the .pxl file."""
    channels = []
    for i in range(NUM_CHANNELS):
        if i == 0:
            name = "clk"
        else:
            name = f"Bit{i-1}"
        channels.append({
            "index": i,
            "view_index": 0,
            "type": 0,
            "enabled": True,
            "name": name,
            "colour": "default",
            "strigger": ""
        })

    session = {
        "Version": 3,
        "Device": "DSLogic",
        "DeviceMode": 0,
        "CollectMode": 0,
        "channel": channels,
        "decoder": [],
        "trigger": {},
        "samplerate": str(SAMPLE_RATE),
        "limit_samples": str(SAMPLE_COUNT)
    }
    return json.dumps(session, indent=2)


def generate_decoders():
    """Generate the decoders JSON (empty - user adds decoder in GUI)."""
    return "[]"


def generate_channel_data():
    """Generate bitmap data for all 17 channels.

    Channel layout:
      ch0  = clk
      ch1  = Bit0 (LSB)
      ch2  = Bit1
      ...
      ch16 = Bit15 (MSB)

    Data pattern:
      - Idle period (all low) at the start
      - For each TEST_VALUE: one clock cycle with data stable
        - Clock: low for half period, high for half period
        - Data bits: set according to TEST_VALUE, stable throughout
      - Idle period at the end
    """
    # Initialize all channels to 0
    channels = [[0] * SAMPLE_COUNT for _ in range(NUM_CHANNELS)]

    pos = 50  # Start after some idle

    for val in TEST_VALUES:
        if pos + CLOCK_PERIOD > SAMPLE_COUNT:
            break

        # Set data bits (channels 1..16) for this value
        for bit_idx in range(16):
            bit_val = (val >> bit_idx) & 1
            ch_idx = bit_idx + 1  # ch1=Bit0, ch2=Bit1, ..., ch16=Bit15
            for s in range(pos, pos + CLOCK_PERIOD):
                channels[ch_idx][s] = bit_val

        # Clock: low for first half, high for second half
        half = CLOCK_PERIOD // 2
        for s in range(pos, pos + half):
            channels[0][s] = 0  # clk low
        for s in range(pos + half, pos + CLOCK_PERIOD):
            channels[0][s] = 1  # clk high (rising edge = sample point)

        pos += CLOCK_PERIOD

    # Convert to bit-packed bytes
    packed = []
    for ch in range(NUM_CHANNELS):
        packed.append(samples_to_bitpacked(channels[ch]))

    return packed


def main():
    print(f"Generating {OUTPUT_FILE} ...")

    channel_data = generate_channel_data()

    with zipfile.ZipFile(OUTPUT_FILE, 'w', zipfile.ZIP_DEFLATED) as zf:
        # Write header
        header_text = generate_header()
        zf.writestr("header", header_text)

        # Write session
        session_json = generate_session()
        zf.writestr("session", session_json)

        # Write decoders
        decoders_json = generate_decoders()
        zf.writestr("decoders", decoders_json)

        # Write logic channel data: L-{ch_index}/0
        for ch_idx in range(NUM_CHANNELS):
            entry_name = f"L-{ch_idx}/0"
            zf.writestr(entry_name, channel_data[ch_idx])

    # Print verification info
    print(f"\nDone! File: {OUTPUT_FILE}")
    print(f"  Channels: {NUM_CHANNELS} (clk + Bit0..Bit15)")
    print(f"  Sample rate: {SAMPLE_RATE} Hz")
    print(f"  Sample count: {SAMPLE_COUNT}")
    print(f"  Clock period: {CLOCK_PERIOD} samples")
    print(f"\nTest values (should appear in decoder output):")
    for val in TEST_VALUES:
        bit15_set = "YES <-- BUG: if decoder shows 0 instead of this" if (val & 0x8000) else "no"
        print(f"  0x{val:04X} = {val:5d}  Bit15={bit15_set}")

    print(f"\nHow to verify the bug:")
    print(f"  1. Open {OUTPUT_FILE} in PXView")
    print(f"  2. Add 'numbers_and_state' decoder")
    print(f"  3. Connect: clk->Clock, Bit0->Bit0, ..., Bit15->Bit15")
    print(f"  4. Set 'Total bits count' = 16")
    print(f"  5. Set 'Clock edge' = rising")
    print(f"  6. Check decoder output:")
    print(f"     - If Bit15 bug exists: 0x8000 shows as 0, 0xFFFF shows as 0x7FFF=32767")
    print(f"     - If Bit15 works: 0x8000 shows as 32768, 0xFFFF shows as 65535")


if __name__ == "__main__":
    main()
