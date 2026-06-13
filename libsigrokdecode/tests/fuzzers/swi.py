import math
from .base import *

class SWIGenerator:
    """SWI (Single Wire Interface) NXP Manchester encoded generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, bitrate=9600, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.half_width = int(samplerate / (bitrate * 2))
        builder.set_idle(channel, 0)

    def _manchester_bit(self, bit):
        if bit:
            self.builder.set_level(self.ch, 0, self.half_width)
            self.builder.set_level(self.ch, 1, self.half_width)
        else:
            self.builder.set_level(self.ch, 1, self.half_width)
            self.builder.set_level(self.ch, 0, self.half_width)

    def write_byte(self, val):
        """Send a byte with Manchester encoding (MSB first)."""
        # Initial idle low so the first Manchester transition is detected
        self.builder.set_level(self.ch, 0, self.half_width * 4)
        for i in range(7, -1, -1):
            self._manchester_bit((val >> i) & 1)

