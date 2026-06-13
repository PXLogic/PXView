import math
from .base import *

class PXX1Generator:
    """PXX1 RC protocol generator.
    1 channel: DATA(ch0). 8.4us per bit."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.bit_width = int(samplerate * 8.4 / 1000000)
        builder.set_idle(channel, 0)

    def send_bind_frame(self, data16):
        """Send a bind frame with 16 bits."""
        self.builder.set_level(self.ch, 1, self.bit_width * 4)
        self.builder.set_level(self.ch, 0, self.bit_width)
        for i in range(15, -1, -1):
            bit = (data16 >> i) & 1
            if bit:
                self.builder.set_level(self.ch, 1, self.bit_width * 2)
                self.builder.set_level(self.ch, 0, self.bit_width)
            else:
                self.builder.set_level(self.ch, 1, self.bit_width)
                self.builder.set_level(self.ch, 0, self.bit_width * 2)

