import math
from .base import *

class PJDLGenerator:
    """PJDL (PJON Data Link) generator. 8N1 UART-like.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, baud=9600, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.bit_width = int(samplerate / baud)
        builder.set_idle(channel, 1)

    def write_byte(self, val):
        """Send a byte 8N1."""
        self.builder.set_level(self.ch, 0, self.bit_width)
        for i in range(8):
            bit = (val >> i) & 1
            self.builder.set_level(self.ch, bit, self.bit_width)
        self.builder.set_level(self.ch, 1, self.bit_width)

