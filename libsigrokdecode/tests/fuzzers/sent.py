import math
from .base import *

class SENTGenerator:
    """SENT (Single Edge Nibble Transmission) generator."""
    def __init__(self, builder, channel, tick_freq=15000, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.tick_samples = int(samplerate / tick_freq)  # ~67 samples per tick
        self.builder.set_idle(channel, 0)

    def _send_nibble(self, value):
        # Low for 12 ticks, high for (value*12 + 12) ticks
        low_ticks = 12
        high_ticks = value * 12 + 12
        self.builder.set_level(self.channel, 0, low_ticks * self.tick_samples)
        self.builder.set_level(self.channel, 1, high_ticks * self.tick_samples)

    def _sent_crc4(self, nibbles):
        crc = 5
        for nibble in nibbles:
            for i in range(3, -1, -1):
                bit = (nibble >> i) & 1
                if (crc >> 3) ^ bit:
                    crc = ((crc << 1) ^ 0x7) & 0xF
                else:
                    crc = (crc << 1) & 0xF
        return crc

    def send_message(self, status=0, data_nibbles=None):
        if data_nibbles is None:
            data_nibbles = [1, 2, 3, 4, 5, 6]
        # Calibration pulse: 56 ticks low + 12 ticks high
        self.builder.set_level(self.channel, 0, 56 * self.tick_samples)
        self.builder.set_level(self.channel, 1, 12 * self.tick_samples)
        # Status nibble
        self._send_nibble(status)
        # Data nibbles
        for nib in data_nibbles:
            self._send_nibble(nib)
        # CRC nibble
        crc = self._sent_crc4([status] + data_nibbles)
        self._send_nibble(crc)

