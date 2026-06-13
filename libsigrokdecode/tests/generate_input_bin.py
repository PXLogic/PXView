#!/usr/bin/env python3
"""Generate input.bin test data files for C decoder tests.

Each input.bin contains bit-packed logic signal data for each channel.
Format: channel 0 data (ceil(sample_count/8) bytes), then channel 1 data, etc.
Bit packing: bit 0 of byte 0 = sample 0, bit 1 of byte 0 = sample 1, etc.
"""

import json
import math
import os
import struct

BASE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "testdata")


def samples_to_bitpacked(channel_data):
    """Convert a list of 0/1 sample values to bit-packed bytes.

    bit 0 of byte 0 = sample 0, bit 1 of byte 0 = sample 1, etc.
    """
    num_bytes = math.ceil(len(channel_data) / 8)
    result = bytearray(num_bytes)
    for i, val in enumerate(channel_data):
        if val:
            byte_idx = i // 8
            bit_idx = i % 8
            result[byte_idx] |= (1 << bit_idx)
    return bytes(result)


def write_input_bin(path, num_channels, sample_count, channels_data):
    """Write input.bin file with bit-packed data for each channel sequentially."""
    with open(path, "wb") as f:
        for ch_idx in range(num_channels):
            packed = samples_to_bitpacked(channels_data[ch_idx])
            f.write(packed)


def generate_spi():
    """SPI test data: CS low, 8 clock cycles with MOSI=0x53, CS high.

    CPOL=0 (clock idles low), CPHA=0 (data sampled on rising edge).
    MOSI sends 0x53 = 01010011 MSB-first.
    4 channels: CLK(0), MISO(1), MOSI(2), CS(3).
    Sample rate: 1MHz, clock period ~10 samples.
    """
    sample_count = 10000
    num_channels = 4
    clk = [0] * sample_count
    miso = [0] * sample_count
    mosi = [0] * sample_count
    cs = [1] * sample_count  # CS idles high (active-low)

    # Pre-SPI idle: 100 samples
    pos = 100

    # CS goes low (asserted)
    for i in range(pos, sample_count):
        cs[i] = 0
    cs_start = pos
    pos += 5  # settle time

    # Send 0x53 = 01010011 MSB-first
    byte_val = 0x53
    half_period = 5  # 10 samples per clock cycle at 1MHz

    for bit_idx in range(8):
        bit_val = (byte_val >> (7 - bit_idx)) & 1  # MSB first
        # Rising edge of clock
        clk[pos] = 0
        mosi[pos] = bit_val
        miso[pos] = 0  # MISO = 0 for test
        pos += 1
        # Clock high phase
        for i in range(half_period - 1):
            if pos < sample_count:
                clk[pos] = 1
                mosi[pos] = bit_val
                miso[pos] = 0
                pos += 1
        # Falling edge - clock goes low
        if pos < sample_count:
            clk[pos] = 0
            mosi[pos] = bit_val
            miso[pos] = 0
            pos += 1
        # Clock low phase
        for i in range(half_period - 1):
            if pos < sample_count:
                clk[pos] = 0
                mosi[pos] = bit_val
                miso[pos] = 0
                pos += 1

    # CS goes high (deasserted)
    for i in range(pos, sample_count):
        cs[i] = 1

    # Fill remaining with idle
    for i in range(pos, sample_count):
        clk[i] = 0
        mosi[i] = 0
        miso[i] = 0

    out_dir = os.path.join(BASE_DIR, "spi_c", "default")
    write_input_bin(os.path.join(out_dir, "input.bin"), num_channels, sample_count,
                    [clk, miso, mosi, cs])
    print(f"Generated SPI input.bin ({sample_count} samples, {num_channels} channels)")


def generate_i2c():
    """I2C test data: START, address 0x50+W, ACK, data 0x42, ACK, STOP.

    2 channels: SCL(0), SDA(1).
    Sample rate: 1MHz, SCL period ~10 samples.
    """
    sample_count = 10000
    num_channels = 2
    scl = [0] * sample_count
    sda = [0] * sample_count

    half_period = 5  # 10 samples per SCL cycle

    pos = 0

    # Idle: both high
    for i in range(100):
        scl[i] = 1
        sda[i] = 1
    pos = 100

    # START condition: SDA falls while SCL is high
    # SCL is already high from idle
    sda[pos] = 1  # SDA still high
    scl[pos] = 1
    pos += 1
    sda[pos] = 0  # SDA falls while SCL high = START
    scl[pos] = 1
    pos += 1

    # Now send address 0x50 + W bit
    # 0x50 = 0101 0000, with W=0: 0b10100000 = 0xA0
    # Address byte: 1010000 0 (7-bit addr 0x50 left-shifted + R/W=0 for write)
    address_byte = 0xA0  # 0x50 << 1 | 0

    for bit_idx in range(8):
        bit_val = (address_byte >> (7 - bit_idx)) & 1
        # SCL low - data setup
        for i in range(half_period):
            if pos < sample_count:
                scl[pos] = 0
                sda[pos] = bit_val
                pos += 1
        # SCL high - data valid
        for i in range(half_period):
            if pos < sample_count:
                scl[pos] = 1
                sda[pos] = bit_val
                pos += 1

    # ACK from slave (SDA low)
    for i in range(half_period):
        if pos < sample_count:
            scl[pos] = 0
            sda[pos] = 0  # ACK = SDA low
            pos += 1
    for i in range(half_period):
        if pos < sample_count:
            scl[pos] = 1
            sda[pos] = 0  # ACK = SDA low
            pos += 1

    # Data byte 0x42 = 0100 0010
    data_byte = 0x42

    for bit_idx in range(8):
        bit_val = (data_byte >> (7 - bit_idx)) & 1
        for i in range(half_period):
            if pos < sample_count:
                scl[pos] = 0
                sda[pos] = bit_val
                pos += 1
        for i in range(half_period):
            if pos < sample_count:
                scl[pos] = 1
                sda[pos] = bit_val
                pos += 1

    # ACK from slave
    for i in range(half_period):
        if pos < sample_count:
            scl[pos] = 0
            sda[pos] = 0
            pos += 1
    for i in range(half_period):
        if pos < sample_count:
            scl[pos] = 1
            sda[pos] = 0
            pos += 1

    # STOP condition: SDA rises while SCL is high
    for i in range(half_period):
        if pos < sample_count:
            scl[pos] = 0
            sda[pos] = 0  # SDA low
            pos += 1
    for i in range(half_period):
        if pos < sample_count:
            scl[pos] = 1
            sda[pos] = 0  # SCL high, SDA still low
            pos += 1
    # SDA rises while SCL high = STOP
    if pos < sample_count:
        scl[pos] = 1
        sda[pos] = 1
        pos += 1

    # Idle after STOP
    for i in range(pos, sample_count):
        scl[i] = 1
        sda[i] = 1

    out_dir = os.path.join(BASE_DIR, "i2c_c", "default")
    write_input_bin(os.path.join(out_dir, "input.bin"), num_channels, sample_count,
                    [scl, sda])
    print(f"Generated I2C input.bin ({sample_count} samples, {num_channels} channels)")


def generate_uart():
    """UART test data: RX sends 0x55 at 9600 baud, TX idle high.

    2 channels: RX(0), TX(1).
    Sample rate: 1MHz, baud 9600 => bit_width ~104 samples.
    0x55 = 01010101, LSB-first: 1,0,1,0,1,0,1,0
    Frame: idle-high, start-bit(0), 8 data bits LSB-first, stop-bit(1).
    """
    sample_count = 10000
    num_channels = 2
    rx = [1] * sample_count  # RX idles high
    tx = [1] * sample_count  # TX idles high

    bit_width = 104  # ~1000000 / 9600

    pos = 100  # start after some idle

    # Start bit (low)
    for i in range(bit_width):
        if pos < sample_count:
            rx[pos] = 0
            pos += 1

    # Data bits for 0x55 = 01010101, LSB-first: bit0=1, bit1=0, bit2=1, bit3=0, bit4=1, bit5=0, bit6=1, bit7=0
    data_val = 0x55
    for bit_idx in range(8):
        bit_val = (data_val >> bit_idx) & 1  # LSB first
        for i in range(bit_width):
            if pos < sample_count:
                rx[pos] = bit_val
                pos += 1

    # Stop bit (high)
    for i in range(bit_width):
        if pos < sample_count:
            rx[pos] = 1
            pos += 1

    # Rest stays idle (high)

    out_dir = os.path.join(BASE_DIR, "uart_c", "default")
    write_input_bin(os.path.join(out_dir, "input.bin"), num_channels, sample_count,
                    [rx, tx])
    print(f"Generated UART input.bin ({sample_count} samples, {num_channels} channels)")


def generate_can():
    """CAN test data: standard frame with ID=0x123, DLC=2, data=0xDE,0xAD.

    1 channel: can_rx(0).
    Sample rate: 1MHz, bitrate 500kbps => bit_width = 2 samples.
    CAN frame: SOF(0), 11-bit ID, RTR(0), IDE(0), r0(0), 4-bit DLC,
               data bytes, 15-bit CRC, CRC delimiter(1), ACK slot(0),
               ACK delimiter(1), EOF(7x1), IFS(3x1).
    """
    sample_count = 10000
    num_channels = 1
    can_rx = [1] * sample_count  # CAN bus idles high (recessive)

    bit_width = 2  # 1MHz / 500kbps = 2 samples per bit

    # Build the CAN frame bit sequence (after stuff-bit encoding)
    # Standard frame: SOF + 11-bit ID + RTR + IDE + r0 + 4-bit DLC + data + CRC + ...
    # ID=0x123 = 00100100011

    # We need to apply bit stuffing: after 5 consecutive same bits, insert opposite bit
    # For simplicity, we'll build the raw frame bits first, then apply stuffing

    # Raw frame bits (before stuffing):
    # SOF: 0
    # ID (11 bits): 00100100011
    # RTR: 0 (data frame)
    # IDE: 0 (standard)
    # r0: 0 (reserved)
    # DLC (4 bits): 0010 (2 bytes)
    # Data byte 0 (8 bits): 11011110 (0xDE)
    # Data byte 1 (8 bits): 10101101 (0xAD)

    raw_bits = []

    # SOF
    raw_bits.append(0)

    # 11-bit identifier: 0x123 = 00100100011
    ident = 0x123
    for i in range(10, -1, -1):
        raw_bits.append((ident >> i) & 1)

    # RTR = 0 (data frame)
    raw_bits.append(0)

    # IDE = 0 (standard frame)
    raw_bits.append(0)

    # r0 = 0 (reserved, dominant)
    raw_bits.append(0)

    # DLC = 2 (4 bits): 0010
    dlc = 2
    for i in range(3, -1, -1):
        raw_bits.append((dlc >> i) & 1)

    # Data byte 0: 0xDE = 11011110
    data0 = 0xDE
    for i in range(7, -1, -1):
        raw_bits.append((data0 >> i) & 1)

    # Data byte 1: 0xAD = 10101101
    data1 = 0xAD
    for i in range(7, -1, -1):
        raw_bits.append((data1 >> i) & 1)

    # CRC-15 (we'll compute a placeholder; for test data, a simple value works)
    # Compute CRC-15-CAN over the raw bits (excluding SOF)
    # CRC polynomial: x^15 + x^14 + x^10 + x^8 + x^7 + x^4 + x^3 + 1
    # = 0x4599 (bit-reversed for LSB-first computation)
    # For simplicity, use a known CRC value. Let's compute it properly.

    def crc15_can(bits):
        """Compute CAN CRC-15 over a sequence of bits."""
        crc = 0x0000
        poly = 0x4599
        for bit in bits:
            c = crc & 0x4000
            crc = ((crc << 1) & 0x7FFF) | bit
            if c:
                crc ^= poly
        return crc & 0x7FFF

    # CRC is computed over: ID + RTR + IDE + r0 + DLC + Data
    crc_bits_input = raw_bits[1:]  # everything after SOF
    crc_val = crc15_can(crc_bits_input)

    # Append 15-bit CRC MSB-first
    for i in range(14, -1, -1):
        raw_bits.append((crc_val >> i) & 1)

    # CRC delimiter: 1 (recessive)
    raw_bits.append(1)

    # ACK slot: 0 (dominant - acknowledged)
    raw_bits.append(0)

    # ACK delimiter: 1 (recessive)
    raw_bits.append(1)

    # EOF: 7 recessive bits
    for _ in range(7):
        raw_bits.append(1)

    # IFS: 3 recessive bits
    for _ in range(3):
        raw_bits.append(1)

    # Now apply bit stuffing to everything between SOF and CRC delimiter
    # Stuff bits are inserted in: ID + RTR + IDE + r0 + DLC + Data + CRC
    # We need to re-build with stuffing applied

    # Bits before stuffing starts (SOF)
    stuffed_bits = [0]  # SOF is not stuffed

    # Apply stuffing to bits from index 1 to end of CRC (before CRC delimiter)
    # The stuffing applies up to and including the CRC sequence
    crc_end = len(raw_bits) - 4  # exclude CRC delimiter, ACK slot, ACK delimiter, EOF, IFS
    # Actually let's find the CRC end: SOF(1) + ID(11) + RTR(1) + IDE(1) + r0(1) + DLC(4) + data(16) + CRC(15) = 50 bits
    stuff_region_end = 1 + 11 + 1 + 1 + 1 + 4 + 16 + 15  # = 50

    stuff_input = raw_bits[1:stuff_region_end]
    stuffed_data = []
    consecutive = 0
    last_bit = -1

    for bit in stuff_input:
        if bit == last_bit:
            consecutive += 1
        else:
            consecutive = 1
            last_bit = bit
        stuffed_data.append(bit)
        if consecutive == 5:
            # Insert opposite stuff bit
            stuff_bit = 1 - last_bit
            stuffed_data.append(stuff_bit)
            consecutive = 1
            last_bit = stuff_bit

    stuffed_bits.extend(stuffed_data)

    # Add the fixed bits after CRC: CRC delimiter, ACK, ACK delimiter, EOF, IFS
    stuffed_bits.append(1)  # CRC delimiter
    stuffed_bits.append(0)  # ACK slot
    stuffed_bits.append(1)  # ACK delimiter
    for _ in range(7):
        stuffed_bits.append(1)  # EOF
    for _ in range(3):
        stuffed_bits.append(1)  # IFS

    # Now write the bits to the channel data
    pos = 100  # start after some idle
    for bit_val in stuffed_bits:
        for i in range(bit_width):
            if pos < sample_count:
                can_rx[pos] = bit_val
                pos += 1

    out_dir = os.path.join(BASE_DIR, "can_c", "default")
    write_input_bin(os.path.join(out_dir, "input.bin"), num_channels, sample_count,
                    [can_rx])
    print(f"Generated CAN input.bin ({sample_count} samples, {num_channels} channels)")
    print(f"  CAN frame: ID=0x123, DLC=2, data=0xDE 0xAD, CRC=0x{crc_val:04x}")
    print(f"  Total stuffed frame bits: {len(stuffed_bits)}")


if __name__ == "__main__":
    generate_spi()
    generate_i2c()
    generate_uart()
    generate_can()
    print("\nAll input.bin files generated successfully.")
