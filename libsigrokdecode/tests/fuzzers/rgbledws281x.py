import math
from .base import *

class RGBLEDWS281xGenerator:
    """WS2812B LED protocol generator.
    1 channel: DATA(ch0).
    At 8MHz samplerate: T0H=350ns=2.8samples, T1H=700ns=5.6samples.
    The decoder checks: tH >= 625ns for bit=1, else duty > 50% for bit=1.
    RESET: low > 50us = 400000ns = 400 samples at 8MHz.
    We use sample counts directly for reliable timing."""
    def __init__(self, builder, channel=0, samplerate=8000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        # Calculate sample counts for timing at the given samplerate
        # T0H: 350ns (bit 0 high time), T1H: 700ns (bit 1 high time)
        # T0L: 800ns (bit 0 low time), T1L: 600ns (bit 1 low time)
        # Total bit period: ~1250ns
        self.t0h = max(2, int(350 * samplerate / 1e9))   # 350ns high for 0
        self.t1h = max(3, int(700 * samplerate / 1e9))   # 700ns high for 1
        self.t0l = max(3, int(800 * samplerate / 1e9))   # 800ns low for 0
        self.t1l = max(2, int(600 * samplerate / 1e9))   # 600ns low for 1
        self.reset_samples = max(50, int(55000 * samplerate / 1e9))  # >50us low for RESET
        builder.set_idle(channel, 0)

    def _send_bit(self, bit):
        if bit:
            self.builder.set_level(self.ch, 1, self.t1h)
            self.builder.set_level(self.ch, 0, self.t1l)
        else:
            self.builder.set_level(self.ch, 1, self.t0h)
            self.builder.set_level(self.ch, 0, self.t0l)

    def send_rgb(self, green, red, blue):
        """Send GRB data for one LED (24 bits) with RESET before."""
        # RESET: low > 50us
        self.builder.set_level(self.ch, 0, self.reset_samples)
        # Green byte MSB first
        for i in range(7, -1, -1):
            self._send_bit((green >> i) & 1)
        # Red byte MSB first
        for i in range(7, -1, -1):
            self._send_bit((red >> i) & 1)
        # Blue byte MSB first
        for i in range(7, -1, -1):
            self._send_bit((blue >> i) & 1)

    def send_reset(self):
        """Send reset signal (low >50us)."""
        self.builder.set_level(self.ch, 0, self.reset_samples)

