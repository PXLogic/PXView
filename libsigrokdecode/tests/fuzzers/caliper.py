import math
from .base import *

class CaliperGenerator:
    """Digital caliper protocol generator.
    2 channels: CLK(ch0), DATA(ch1)."""
    def __init__(self, builder, clk_ch=0, data_ch=1, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.bit_width = int(samplerate / 8000)
        builder.set_idle(clk_ch, 0)
        builder.set_idle(data_ch, 0)

    def send_value(self, value, num_bits=24):
        """Send a value LSB first, clocked by CLK."""
        for i in range(num_bits):
            bit = (value >> i) & 1
            self.builder.set_level(self.data, bit, 0)
            self.builder.set_level(self.clk, 1, self.bit_width)
            self.builder.set_level(self.clk, 0, self.bit_width)

