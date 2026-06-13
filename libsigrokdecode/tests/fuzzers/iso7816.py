import math
from .base import *

class ISO7816Generator:
    """2 channels: CLK(ch0), IO(ch1)"""
    def __init__(self, builder, clk_ch, io_ch, etu=372, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.io = io_ch
        self.bit_width = etu  # 1 etu in samples
        self.clk_half_period = max(1, int(samplerate / 4000000))  # CLK ~2MHz
        self.builder.set_idle(clk_ch, 0)
        self.builder.set_idle(io_ch, 1)

    def _run_clk(self, num_half_periods):
        """Toggle CLK for the given number of half periods."""
        for _ in range(num_half_periods):
            self.builder.set_level(self.clk, 1, self.clk_half_period)
            self.builder.set_level(self.clk, 0, self.clk_half_period)

    def _send_byte(self, val):
        """
        Send a byte: 1 start bit (low), 8 data bits (LSB first),
        1 parity bit, 2 etu guard time.
        """
        # Compute parity (even)
        p = 0
        for i in range(8):
            p ^= (val >> i) & 1

        bits = [0]  # Start bit
        for i in range(8):
            bits.append((val >> i) & 1)  # LSB first
        bits.append(p)  # Parity bit

        for bit in bits:
            self.builder.set_level(self.io, bit, self.bit_width)
            # Run CLK during each etu
            clk_half = self.bit_width // (self.clk_half_period * 2)
            self._run_clk(max(clk_half, 1))

        # Guard time: 2 etu of idle (high) on IO
        self.builder.set_level(self.io, 1, self.bit_width * 2)
        clk_half = (self.bit_width * 2) // (self.clk_half_period * 2)
        self._run_clk(max(clk_half, 1))

    def send_atr(self):
        """Generate ATR (Answer To Reset)."""
        # IO idle high
        self.builder.set_level(self.io, 1, self.bit_width * 4)
        # TS: 0x3B (direct convention)
        self._send_byte(0x3B)
        # T0: 0x80 (TD1 present, no historical bytes)
        self._send_byte(0x80)
        # TD1: 0x31 (T=1 protocol)
        self._send_byte(0x31)

