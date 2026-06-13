import math
from .base import *


class IecGenerator:
    """IEC (Commodore serial bus) protocol generator.

    3 channels: DATA(ch0), CLK(ch1), ATN(ch2).
    The IEC C decoder uses a step-based state machine:
    - Step 0: Wait for ATN falling OR (DATA low AND CLK high)
    - Step 1: Wait for ATN falling OR (DATA high AND CLK high) OR CLK low
    - Step 2: Wait for ATN falling OR DATA low (EOI) OR CLK low
    - Step 3: Wait for ATN falling OR CLK edge (rising=latch DATA, falling=end bit)

    Sequence for sending a byte:
    1. ATN falls (step 0)
    2. DATA=0, CLK=1 (step 0 -> step 1)
    3. DATA=1, CLK=1 (step 1 -> step 2, start of byte)
    4. CLK falls (step 2 -> step 3)
    5. For each bit: CLK rises (latch DATA), CLK falls (end of bit)
    6. After 8 bits, byte is complete (step -> 0)
    7. ATN rises
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.data = channels_map.get('data', 0)
        self.clk = channels_map.get('clk', 1)
        self.atn = channels_map.get('atn', 2)
        self.half_period = max(2, samplerate // 100000)

    def _send_byte(self, val):
        """Send one byte on the IEC bus.
        8 data bits LSB first, clocked by CLK.
        The decoder latches DATA on CLK rising edge."""
        hp = self.half_period
        for i in range(8):
            bit = (val >> i) & 1
            # Set DATA while CLK is low
            self.builder.write_channels({self.data: bit, self.clk: 0}, hp)
            # CLK rises (decoder latches DATA)
            self.builder.write_channels({self.data: bit, self.clk: 1}, hp)

        # After 8th bit, CLK falls to complete the byte
        self.builder.set_level(self.clk, 0, hp)

    def _send_atn_byte(self, val):
        """Send a byte with ATN asserted (command byte).
        Sequence: ATN falls -> step 0 -> step 1 -> step 2 -> step 3 -> byte -> ATN rises."""
        hp = self.half_period

        # ATN high initially
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 1}, hp * 2)

        # ATN falls (triggers step 0 reset)
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 0}, hp * 2)

        # Step 0 -> Step 1: DATA=0, CLK=1
        self.builder.write_channels({self.data: 0, self.clk: 1, self.atn: 0}, hp * 2)

        # Step 1 -> Step 2: DATA=1, CLK=1 (start of byte)
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 0}, hp * 2)

        # Step 2 -> Step 3: CLK falls
        self.builder.write_channels({self.data: 1, self.clk: 0, self.atn: 0}, hp * 2)

        # Step 3: Send the byte (CLK edges latch DATA)
        self._send_byte(val)

        # ATN rises (end of ATN phase)
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 1}, hp * 2)

    def generate_testdata(self):
        # Set idle states: DATA high, CLK high, ATN high
        self.builder.set_idle(self.data, 1)
        self.builder.set_idle(self.clk, 1)
        self.builder.set_idle(self.atn, 1)

        # Initial idle
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 1}, self.half_period * 10)

        # Send ATN command byte: 0x20 (LISTEN, device 0)
        self._send_atn_byte(0x20)

        # Idle gap
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 1}, self.half_period * 4)

        # Send another ATN command byte: 0x3F (UNLISTEN)
        self._send_atn_byte(0x3F)

        # Final idle
        self.builder.write_channels({self.data: 1, self.clk: 1, self.atn: 1}, self.half_period * 10)
