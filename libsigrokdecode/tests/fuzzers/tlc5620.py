import math
from .base import *

class TLC5620Generator:
    """TLC5620 DAC protocol generator.
    2 channels: CLK(ch0), DATA(ch1)."""
    def __init__(self, builder, clk_ch=0, data_ch=1, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.half_period = int(samplerate / 200000)
        builder.set_idle(clk_ch, 0)
        builder.set_idle(data_ch, 0)

    def write(self, channel, rng, data_val):
        """Send a write command: 2-bit channel + 1-bit range + 8-bit data."""
        bits = []
        bits.append((channel >> 1) & 1)
        bits.append(channel & 1)
        bits.append(rng)
        for i in range(7, -1, -1):
            bits.append((data_val >> i) & 1)
        for b in bits:
            self.builder.set_level(self.data, b, 0)
            self.builder.set_level(self.clk, 1, self.half_period)
            self.builder.set_level(self.clk, 0, self.half_period)

