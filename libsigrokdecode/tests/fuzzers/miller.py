import math
from .base import *

class MillerGenerator:
    """Miller encoding generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, bitrate=1000, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.bit_width = int(samplerate / bitrate)
        self.half_bit = self.bit_width // 2
        builder.set_idle(channel, 0)

    def send_bits(self, bits):
        """Miller encoding: 1=transition at mid-bit, 0=no transition."""
        current = 0
        for i, b in enumerate(bits):
            if b:
                self.builder.set_level(self.ch, current, self.half_bit)
                current = 1 - current
                self.builder.set_level(self.ch, current, self.half_bit)
            else:
                self.builder.set_level(self.ch, current, self.bit_width)
                if i > 0 and bits[i-1] == 1:
                    current = 1 - current

