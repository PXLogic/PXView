import math
from .base import *


class JitterGenerator:
    """Jitter measurement protocol generator.

    2 channels: CLK(ch0), SIG(ch1).
    The jitter decoder measures timing between clock edges and signal edges.
    Default polarity: rising edges for both CLK and SIG.

    State machine:
    - STATE_CLK: Wait for CLK edge -> record clk_start, go to STATE_SIG
    - STATE_SIG: Wait for SIG edge -> record sig_start, compute jitter,
                 output annotation, go back to STATE_CLK

    We generate a clock signal and a signal with slight delay to produce
    jitter annotations.
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.clk = channels_map.get('clk', 0)
        self.sig = channels_map.get('sig', 1)
        self.half_period = max(2, samplerate // 200000)

    def generate_testdata(self):
        hp = self.half_period

        # Set idle states
        self.builder.set_idle(self.clk, 0)
        self.builder.set_idle(self.sig, 0)

        # Initial idle: both low
        self.builder.write_channels({self.clk: 0, self.sig: 0}, hp * 10)

        # Generate clock pulses with signal edges slightly delayed
        # The decoder measures jitter = time from CLK edge to SIG edge
        for i in range(10):
            # CLK rising edge (triggers STATE_CLK -> STATE_SIG)
            self.builder.set_level(self.clk, 1, hp)
            # Small delay before SIG rises (this is the jitter)
            delay = hp // 3  # ~1/3 of half period
            self.builder.set_level(self.sig, 0, delay)
            self.builder.set_level(self.sig, 1, hp - delay)
            # CLK falling edge
            self.builder.set_level(self.clk, 0, hp)
            # SIG falls
            self.builder.set_level(self.sig, 0, hp)

        # Generate a few more with varying jitter
        for i in range(5):
            self.builder.set_level(self.clk, 1, hp)
            delay = hp // 4 + i * (hp // 8)  # Varying delay
            self.builder.set_level(self.sig, 0, delay)
            self.builder.set_level(self.sig, 1, hp - delay)
            self.builder.set_level(self.clk, 0, hp)
            self.builder.set_level(self.sig, 0, hp)

        # Final idle
        self.builder.write_channels({self.clk: 0, self.sig: 0}, hp * 10)
