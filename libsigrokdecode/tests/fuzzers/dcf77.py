import math
from .base import *

class DCF77Generator:
    """DCF77 time signal generator.
    1 channel: DATA(ch0). At 1MHz samplerate."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 1)

    def _ms(self, ms):
        return int(ms * self.sr / 1000)

    def send_bit(self, bit):
        """Send one second of DCF77 data."""
        if bit:
            self.builder.set_level(self.ch, 0, self._ms(200))
            self.builder.set_level(self.ch, 1, self._ms(800))
        else:
            self.builder.set_level(self.ch, 0, self._ms(100))
            self.builder.set_level(self.ch, 1, self._ms(900))

    def send_time(self, bits_list):
        """Send a sequence of DCF77 bits."""
        for b in bits_list:
            self.send_bit(b)

