import math
from .base import *

class SDQGenerator:
    """SDQ (Smart Data Quality) 1-wire protocol generator.
    1 channel: DATA(ch0).
    Idle high. Falling edge starts a bit. Low duration determines value:
    - Short low (~5us) + long high (~60us) = bit 1
    - Long low (~60us) + short high (~5us) = bit 0
    - Very long low (~480us) = reset/break
    Bits are sent LSB first."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 1)

    def _us(self, us):
        return max(1, int(us * self.sr / 1000000))

    def send_reset(self):
        """Send reset pulse + presence detect."""
        # Reset: pull low 480us
        self.builder.set_level(self.ch, 0, self._us(480))
        # Presence: release high, slave pulls low after ~60us
        self.builder.set_level(self.ch, 1, self._us(10))
        self.builder.set_level(self.ch, 0, self._us(60))
        # Release
        self.builder.set_level(self.ch, 1, self._us(100))

    def _send_bit(self, bit):
        """Send one bit: falling edge starts, low duration determines value."""
        if bit:
            # Bit 1: short low (~5us) + long high (~60us)
            self.builder.set_level(self.ch, 0, self._us(5))
            self.builder.set_level(self.ch, 1, self._us(60))
        else:
            # Bit 0: long low (~60us) + short high (~5us)
            self.builder.set_level(self.ch, 0, self._us(60))
            self.builder.set_level(self.ch, 1, self._us(5))

    def send_byte(self, val):
        """Send a byte (LSB first, 1-wire timing)."""
        for i in range(8):
            bit = (val >> i) & 1
            self._send_bit(bit)

    def send_transaction(self):
        """Send a complete SDQ transaction: reset + command byte."""
        # Initial idle high so the falling edge of reset is detected
        self.builder.set_level(self.ch, 1, self._us(100))
        self.send_reset()
        self.send_byte(0x33)  # Read ROM command

