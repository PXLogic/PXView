import math
from .base import *

class SwimGenerator:
    """SWIM (Single Wire Interface Module) generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 1)

    def _us(self, us):
        return int(us * self.sr / 1000000)

    def send_command(self, cmd):
        """Send a SWIM command: start + 8 bits."""
        # Initial idle high so the falling edge of start is detected
        self.builder.set_level(self.ch, 1, self._us(50))
        self.builder.set_level(self.ch, 0, self._us(2))
        self.builder.set_level(self.ch, 1, self._us(1))
        for i in range(7, -1, -1):
            bit = (cmd >> i) & 1
            if bit:
                self.builder.set_level(self.ch, 1, self._us(1))
                self.builder.set_level(self.ch, 0, self._us(0.5))
            else:
                self.builder.set_level(self.ch, 1, self._us(0.5))
                self.builder.set_level(self.ch, 0, self._us(1))

