import math
from .base import *

class IEBUSGenerator:
    """IEBus (Inter-Equipment Bus) Manchester encoded generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, bitrate=250, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.half_width = int(samplerate / (bitrate * 2))
        builder.set_idle(channel, 0)

    def _manchester_bit(self, bit):
        if bit:
            self.builder.set_level(self.ch, 0, self.half_width)
            self.builder.set_level(self.ch, 1, self.half_width)
        else:
            self.builder.set_level(self.ch, 1, self.half_width)
            self.builder.set_level(self.ch, 0, self.half_width)

    def send_frame(self, sync_bits, cmd, data):
        """Send a frame: sync + command byte + data byte."""
        for b in sync_bits:
            self._manchester_bit(b)
        for i in range(7, -1, -1):
            self._manchester_bit((cmd >> i) & 1)
        for i in range(7, -1, -1):
            self._manchester_bit((data >> i) & 1)

