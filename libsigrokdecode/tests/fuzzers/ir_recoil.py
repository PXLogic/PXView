import math
from .base import *

class IrRecoilGenerator:
    """Recoil laser tag IR protocol generator (baseband).
    Default polarity is active-low (idle HIGH, burst LOW).
    Protocol: SYNC pulse (3.3ms) + SYNC PAUSE (1.5ms)
    Then data bits: DATA (0.4ms=0, 0.8ms=1) with threshold at 0.6ms"""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.channel = channels_map.get('ir', 0)
        self.sr = samplerate
        self.builder.set_idle(self.channel, 1)  # Idle HIGH (active-low)

    def _us(self, us):
        return max(1, int(us * self.sr / 1e6))

    def send_packet(self, data=0x05, num_bits=5):
        """Send a Recoil packet: sync + sync pause + data bits."""
        # Initial idle high so the falling edge of SYNC is detected
        self.builder.set_level(self.channel, 1, self._us(1000))
        # SYNC: 3.3ms active (LOW for active-low)
        self.builder.set_level(self.channel, 0, self._us(3300))
        # SYNC PAUSE: 1.5ms idle (HIGH)
        self.builder.set_level(self.channel, 1, self._us(1500))
        # Data bits: each bit = DATA pulse
        for i in range(num_bits - 1, -1, -1):
            bit = (data >> i) & 1
            if bit:
                # Bit 1: 0.8ms active (LOW)
                self.builder.set_level(self.channel, 0, self._us(800))
            else:
                # Bit 0: 0.4ms active (LOW)
                self.builder.set_level(self.channel, 0, self._us(400))
            # Small gap between bits
            self.builder.set_level(self.channel, 1, self._us(200))
        # End: long idle
        self.builder.set_level(self.channel, 1, self._us(10000))

    def generate_testdata(self):
        # Send a 5-bit packet 0x05
        self.send_packet(0x05, 5)
