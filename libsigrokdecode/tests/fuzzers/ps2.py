import math
from .base import *

class PS2Generator:
    def __init__(self, builder, clk_ch, data_ch, freq=12500):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.half_period = int(builder.samplerate / (freq * 2))
        self.builder.set_idle(clk_ch, 1)
        self.builder.set_idle(data_ch, 1)

    def write_byte(self, val):
        # 11 bits: 1 start (0), 8 data (LSB first), 1 parity (odd), 1 stop (1)
        p = 1
        bits = [0]
        for i in range(8):
            bit = (val >> i) & 1
            bits.append(bit)
            p ^= bit
        bits.append(p)
        bits.append(1)

        period = self.half_period * 2
        for b in bits:
            # Data must be stable for the full bit period (both CLK high and CLK low)
            self.builder.set_level(self.data, b, period)
            self.builder.pos -= period  # Don't advance, just overlay
            # CLK high for first half, low for second half
            self.builder.set_level(self.clk, 1, self.half_period)
            self.builder.set_level(self.clk, 0, self.half_period)

        # Idle: CLK high for a while
        self.builder.set_level(self.clk, 1, self.half_period * 4)
        self.builder.set_level(self.data, 1, self.half_period * 4)

