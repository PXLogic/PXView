import math
from .base import *

class DeltaSigmaGenerator:
    """Delta-sigma modulator generator.
    2 channels: CLK(ch0), DATA(ch1).
    The decoder triggers on rising CLK edges and runs sinc filters.
    Need enough clock cycles for the sinc filter to produce output.
    A simple pattern of alternating 1s and 0s creates a mid-range signal."""
    def __init__(self, builder, clk_ch=0, data_ch=1, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.half_period = max(2, int(samplerate / 200000))
        builder.set_idle(clk_ch, 0)
        builder.set_idle(data_ch, 0)

    def send_pattern(self, pattern):
        """Send a bit pattern with many repetitions for sinc filter convergence.
        CLK toggles, DATA changes on falling CLK edge (setup for rising edge).
        The sinc filter needs many samples to converge, so we repeat the pattern."""
        # Initial idle: CLK low, DATA low
        self.builder.set_level(self.clk, 0, self.half_period * 4)
        self.builder.set_level(self.data, 0, self.half_period * 4)
        # Repeat the pattern many times for sinc filter convergence
        for _ in range(64):
            for bit in pattern:
                # Set DATA while CLK is low
                period = self.half_period * 2
                self.builder.set_level(self.clk, 0, period)
                self.builder.pos -= period
                self.builder.set_level(self.data, bit, period)
                self.builder.pos -= period
                # CLK rising edge (decoder samples DATA here)
                self.builder.set_level(self.clk, 1, self.half_period)
                # CLK falling edge
                self.builder.set_level(self.clk, 0, self.half_period)

