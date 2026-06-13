import math
from .base import *

class DMX512Generator:
    """DMX512 protocol generator.
    At 4MHz samplerate: bit_samples = 16 (4us per bit at 250kbaud).
    BREAK must be >88us low, MAB must be 8us-1s high."""
    def __init__(self, builder, channel, samplerate=4000000):
        self.builder = builder
        self.channel = channel
        self.sr = samplerate
        self.bit_samples = max(4, int(4 * samplerate / 1e6))  # 4us per bit at 250kbaud
        self.builder.set_idle(channel, 1)

    def _send_byte(self, val):
        # Start bit (0)
        self.builder.set_level(self.channel, 0, self.bit_samples)
        # 8 data bits LSB first
        for i in range(8):
            self.builder.set_level(self.channel, (val >> i) & 1, self.bit_samples)
        # 2 stop bits (1,1)
        self.builder.set_level(self.channel, 1, self.bit_samples * 2)

    def send_frame(self, slots=None):
        if slots is None:
            slots = [0x00, 0xFF, 0x80, 0x40, 0x20]
        # Idle high before BREAK
        self.builder.set_level(self.channel, 1, self.bit_samples * 10)
        # Break: 176us low (44 bit times at 250kbaud) - must be >88us
        break_samples = self.bit_samples * 44
        self.builder.set_level(self.channel, 0, break_samples)
        # Mark After Break: 8us-12us high (2-3 bit times)
        mab_samples = self.bit_samples * 3
        self.builder.set_level(self.channel, 1, mab_samples)
        # Start code: 0x00
        self._send_byte(0x00)
        # Data slots
        for slot in slots:
            self._send_byte(slot)

