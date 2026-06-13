import math
from .base import *

class RCEncodeGenerator:
    """RC encode (pulse width) generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 0)

    def _us(self, us):
        return int(us * self.sr / 1000000)

    def send_pattern(self, bits):
        """Send RC pattern: long pulse = 1, short pulse = 0."""
        for b in bits:
            self.builder.set_level(self.ch, 1, self._us(500))
            if b:
                self.builder.set_level(self.ch, 0, self._us(1500))
            else:
                self.builder.set_level(self.ch, 0, self._us(500))
        self.builder.set_level(self.ch, 0, self._us(5000))

