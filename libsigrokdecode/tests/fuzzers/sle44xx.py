import math
from .base import *

class Sle44xxGenerator:
    """SLE44xx smartcard decoder generator.
    3 channels: rst(0), clk(1), io(2).
    Flow: RST high with CLK pulse → RST falling → ATR mode (4 bytes).
    Data bits use CLK edges while RST is low:
    - CLK rising while RST low = COND_DATA_START (reads IO value)
    - CLK falling while RST low = COND_DATA_STOP"""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _send_bit(self, rst, clk, io, bit_val, half_period):
        """Send one data bit using CLK edges while RST is low.
        CLK rising → COND_DATA_START (reads IO), CLK falling → COND_DATA_STOP."""
        # Set IO to the bit value (overlay, no advance)
        self.builder.set_level(io, bit_val, 0)
        # CLK rising while RST low → COND_DATA_START, reads IO value
        self.builder.set_level(clk, 1, half_period)
        # CLK falling while RST low → COND_DATA_STOP
        self.builder.set_level(clk, 0, half_period)

    def generate_testdata(self):
        rst = self.channels_map.get("rst", 0)
        clk = self.channels_map.get("clk", 1)
        io = self.channels_map.get("io", 2)

        half_period = max(4, self.samplerate // 100000)  # 10us half-period

        # Idle: RST=0, CLK=0, IO=1 (high)
        self.builder.set_level(rst, 0, 0)   # overlay
        self.builder.set_level(clk, 0, 0)   # overlay
        self.builder.set_level(io, 1, 0)    # overlay

        # Advance a bit
        self.builder.set_pos(self.builder.get_pos() + half_period * 4)

        # RST rising edge (COND_RESET_START)
        self.builder.set_level(rst, 1, half_period * 2)

        # CLK pulse while RST is high (COND_RSTCLK_START/STOP)
        # This sets has_rstclk=1 so RST falling enters ATR mode
        self.builder.set_level(clk, 1, half_period)
        self.builder.set_level(clk, 0, half_period)

        # RST falling edge (COND_RESET_STOP) → enters ATR mode
        self.builder.set_level(rst, 0, half_period * 2)

        # ATR: 4 bytes sent LSB-first
        # Each bit: set IO, CLK rising (DATA_START reads IO), CLK falling (DATA_STOP)
        atr_bytes = [0x3B, 0x00, 0x00, 0x00]

        for byte_val in atr_bytes:
            for bit_idx in range(8):
                bit_val = (byte_val >> bit_idx) & 1  # LSB-first
                self._send_bit(rst, clk, io, bit_val, half_period)
