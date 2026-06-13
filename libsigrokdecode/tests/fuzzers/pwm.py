import math
from .base import *

class PWMGenerator:
    """PWM waveform generator."""
    def __init__(self, builder, channel, freq=1000, duty=0.5):
        self.builder = builder
        self.channel = channel
        self.freq = freq
        self.duty = duty
        self.period = int(builder.samplerate / freq)
        self.builder.set_idle(channel, 0)

    def send_cycles(self, n):
        high_samples = int(self.period * self.duty)
        low_samples = self.period - high_samples
        for _ in range(n):
            self.builder.set_level(self.channel, 1, high_samples)
            self.builder.set_level(self.channel, 0, low_samples)

