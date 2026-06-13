import math
from .base import *

class Xy2100Generator:
    """XY2-100 galvanometer protocol generator.
    3 channels: CLK(ch0), SYNC(ch1), DATA(ch2).
    Optional: STATUS(ch3).
    Protocol: 20-bit frames clocked by CLK.
    - CLK rising edge: end previous data bit, start new bit
    - CLK falling edge: sample DATA and SYNC
    - SYNC=1 during data bits, SYNC=0 at end of frame
    - Frame: 1 start bit + 18 data bits + 1 parity bit = 20 bits
    - For 16-bit position frame: bits[0:2] = 001 (type), bits[3:18] = position, bit[19] = parity"""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.clk = channels_map.get('clk', 0)
        self.sync = channels_map.get('sync', 1)
        self.data = channels_map.get('data', 2)

    def generate_testdata(self):
        clk = self.clk
        sync = self.sync
        data = self.data
        hp = max(2, self.samplerate // 200000)

        # Idle: CLK low, SYNC low, DATA low
        self.builder.set_level(clk, 0, hp * 4)
        self.builder.set_level(sync, 0, hp * 4)
        self.builder.set_level(data, 0, hp * 4)

        # Generate a 16-bit position frame with value 0x1000
        # Frame bits (MSB first): type(3) + position(16) + parity(1) = 20 bits
        # Type for 16-bit position: 001
        # Position: 0x1000 = 0001 0000 0000 0000
        # Build the 20-bit frame
        frame_bits = []
        # Type bits: 0, 0, 1 (16-bit position)
        frame_bits.extend([0, 0, 1])
        # Position bits: 0x1000, MSB first (16 bits)
        pos_val = 0x1000
        for i in range(15, -1, -1):
            frame_bits.append((pos_val >> i) & 1)
        # Parity: XOR of bits 0-18
        parity = 0
        for b in frame_bits:
            parity ^= b
        frame_bits.append(parity)

        # Send the frame
        # The decoder samples DATA on CLK falling edge and processes on CLK rising edge
        # SYNC=1 during data, SYNC=0 at the last bit boundary
        for i, bit in enumerate(frame_bits):
            # Set DATA and SYNC before CLK falling edge
            sync_val = 1 if i < 19 else 0  # SYNC goes low at the end

            # CLK falling edge: sample DATA and SYNC
            period = hp * 2
            self.builder.set_level(clk, 0, period)
            self.builder.pos -= period
            self.builder.set_level(data, bit, period)
            self.builder.pos -= period
            self.builder.set_level(sync, sync_val, period)
            self.builder.pos -= period

            # CLK rising edge: process bit
            self.builder.set_level(clk, 1, hp)
            self.builder.set_level(clk, 0, hp)

        # Idle after frame
        self.builder.set_level(clk, 0, hp * 4)
        self.builder.set_level(sync, 0, hp * 4)
        self.builder.set_level(data, 0, hp * 4)
