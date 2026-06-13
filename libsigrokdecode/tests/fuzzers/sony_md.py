import math
from .base import *

class SonyMDGenerator:
    """Sony Minidisc LCD Remote protocol generator.
    1 channel: DATA(ch0).
    The decoder expects:
    - Reset pulse: ~40ms low (optional, or presync pulse directly)
    - Presync pulse: ~1100us high
    - Presync delay: ~950us low
    - Sync pulse: ~220us high
    - Data bits: short low (~17us) = 1, long low (~220us) = 0
    Default expectedBitCount = 16 (2 bytes)."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 0)

    def _us(self, us):
        return max(1, int(us * self.sr / 1e6))

    def send_message(self, data_bytes=None):
        """Send a Sony MD message: presync + presync delay + sync + data bits.
        Default: 2 bytes (16 bits) = short message."""
        if data_bytes is None:
            data_bytes = [0x00, 0x01]
        # Initial idle low so the rising edge of presync is detected
        self.builder.set_level(self.ch, 0, self._us(500))
        # Presync pulse: ~1100us high (within 20% margin)
        self.builder.set_level(self.ch, 1, self._us(1100))
        # Presync delay: ~950us low (within 800-1500us range)
        self.builder.set_level(self.ch, 0, self._us(950))
        # Sync pulse: ~220us high (within 20-280us range)
        self.builder.set_level(self.ch, 1, self._us(220))
        # Data bits: each bit is high-then-low
        # Short low (~17us) = bit 1, Long low (~220us) = bit 0
        # The high portion before each low is ~32.5us (bitDelayHigh)
        bits = []
        for byte_val in data_bytes:
            for i in range(7, -1, -1):
                bits.append((byte_val >> i) & 1)
        for b in bits:
            # High portion: ~32.5us
            self.builder.set_level(self.ch, 1, self._us(32))
            if b:
                # Bit 1: short low (~17us)
                self.builder.set_level(self.ch, 0, self._us(17))
            else:
                # Bit 0: long low (~220us)
                self.builder.set_level(self.ch, 0, self._us(220))
        # End: hold low for a while (idle)
        self.builder.set_level(self.ch, 0, self._us(5000))

