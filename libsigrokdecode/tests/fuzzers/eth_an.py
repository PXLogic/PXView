import math
from .base import *

class EthANGenerator:
    """Ethernet Auto-Negotiation Fast Link Pulse generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 0)

    def _us(self, us):
        return int(us * self.sr / 1000000)

    def send_flp_burst(self, data16):
        """Send one FLP burst."""
        for i in range(17):
            self.builder.set_level(self.ch, 1, self._us(0.1) if self.sr >= 10000000 else 1)
            if i < 16:
                bit = (data16 >> (15 - i)) & 1
                low_time = self._us(6.25) if bit else self._us(12.5)
                self.builder.set_level(self.ch, 0, low_time)
            else:
                self.builder.set_level(self.ch, 0, self._us(100))

    def send_negotiation(self, ability=0x0040):
        """Send 17 FLP bursts with technology ability."""
        for _ in range(17):
            self.send_flp_burst(ability)
            self.builder.set_level(self.ch, 0, self._us(16500))

