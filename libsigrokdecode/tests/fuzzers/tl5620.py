import math
from .base import *

class TL5620Generator:
    """TL5620 DAC protocol generator (alias for TLC5620).
    2 channels: CLK(ch0), DATA(ch1)."""
    def __init__(self, builder, clk_ch=0, data_ch=1, samplerate=1000000):
        self._impl = TLC5620Generator(builder, clk_ch, data_ch, samplerate)

    def write(self, channel, rng, data_val):
        self._impl.write(channel, rng, data_val)

