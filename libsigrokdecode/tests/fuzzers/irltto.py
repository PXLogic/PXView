import math
from .base import *

class IrLttoGenerator:
    """LTTO laser tag IR protocol generator (baseband).
    Default polarity is active-low (idle HIGH, burst LOW).
    Protocol: PRE-SYNC pulse (3ms) + PRE-SYNC PAUSE (6ms) + SYNC pulse (3ms)
    Then bits: BIT PAUSE (2ms) + DATA (1ms=0, 2ms=1)"""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.channel = channels_map.get('ir', 0)
        self.sr = samplerate
        self.builder.set_idle(self.channel, 1)  # Idle HIGH (active-low)

    def _us(self, us):
        return max(1, int(us * self.sr / 1e6))

    def send_signature(self, data=0x05, num_bits=5):
        """Send an LTTO signature: pre-sync + pause + sync + data bits."""
        # Initial idle high so the falling edge of PRE-SYNC is detected
        self.builder.set_level(self.channel, 1, self._us(1000))
        # PRE-SYNC: 3ms active (LOW for active-low)
        self.builder.set_level(self.channel, 0, self._us(3000))
        # PRE-SYNC PAUSE: 6ms idle (HIGH)
        self.builder.set_level(self.channel, 1, self._us(6000))
        # SYNC: 3ms active (LOW)
        self.builder.set_level(self.channel, 0, self._us(3000))
        # Data bits: each bit = BIT PAUSE (2ms HIGH) + DATA pulse
        for i in range(num_bits - 1, -1, -1):
            bit = (data >> i) & 1
            # BIT PAUSE: 2ms idle (HIGH)
            self.builder.set_level(self.channel, 1, self._us(2000))
            if bit:
                # Bit 1: 2ms active (LOW)
                self.builder.set_level(self.channel, 0, self._us(2000))
            else:
                # Bit 0: 1ms active (LOW)
                self.builder.set_level(self.channel, 0, self._us(1000))
        # End: long idle
        self.builder.set_level(self.channel, 1, self._us(10000))

    def generate_testdata(self):
        # Send a 5-bit signature 0x05
        self.send_signature(0x05, 5)
