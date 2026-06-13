import math
from .base import *

class DALIGenerator:
    """DALI protocol generator (Manchester encoded).
    DALI bit period = 833.3us (1200 bps), halfbit = 416.65us.
    Default polarity is active-low (idle HIGH) to match decoder defaults.
    The decoder waits for any edge from IDLE, then processes Manchester
    phases. A forward frame is 16 data bits + stop (two consecutive 1s).
    A backward frame is 8 data bits + stop."""
    def __init__(self, builder, channel, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.bit_samples = max(4, int(samplerate * 0.0008333))  # ~833 samples per bit at 1MHz
        self.half_bit = self.bit_samples // 2  # ~416 samples per half-bit
        self.builder.set_idle(channel, 1)  # Idle HIGH (active-low)

    def _write_manchester(self, bit):
        # DALI Manchester (active-low): 1 = HIGH→LOW at mid-bit, 0 = LOW→HIGH at mid-bit
        # In active-low: HIGH = idle/no-signal, LOW = signal present
        # The decoder samples on edges: PHASE0 captures first half, PHASE1 captures second half
        if bit:
            self.builder.set_level(self.channel, 1, self.half_bit)
            self.builder.set_level(self.channel, 0, self.half_bit)
        else:
            self.builder.set_level(self.channel, 0, self.half_bit)
            self.builder.set_level(self.channel, 1, self.half_bit)

    def send_forward(self, address=0xFF, command=0x20):
        # Idle HIGH before start (decoder needs to see edge from idle)
        self.builder.set_level(self.channel, 1, self.half_bit * 4)
        # 16 bits Manchester: address byte + command byte (MSB first)
        for i in range(7, -1, -1):
            self._write_manchester((address >> i) & 1)
        for i in range(7, -1, -1):
            self._write_manchester((command >> i) & 1)
        # Stop: two consecutive 1s (both phases high in active-low)
        self.builder.set_level(self.channel, 1, self.half_bit)
        self.builder.set_level(self.channel, 1, self.half_bit)
        # Idle after frame
        self.builder.set_level(self.channel, 1, self.half_bit * 4)

    def send_backward(self, data=0xFF):
        # Idle HIGH before start
        self.builder.set_level(self.channel, 1, self.half_bit * 4)
        # 8 bits Manchester (MSB first)
        for i in range(7, -1, -1):
            self._write_manchester((data >> i) & 1)
        # Stop: two consecutive 1s
        self.builder.set_level(self.channel, 1, self.half_bit)
        self.builder.set_level(self.channel, 1, self.half_bit)
        # Idle after frame
        self.builder.set_level(self.channel, 1, self.half_bit * 4)

