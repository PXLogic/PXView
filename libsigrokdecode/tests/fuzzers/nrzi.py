import math
from .base import *

class NRZIGenerator:
    """NRZI encoding: bit 0 = toggle level, bit 1 = no change."""
    def __init__(self, builder, channel, bitrate=100000):
        self.builder = builder
        self.channel = channel
        self.bit_width = int(builder.samplerate / bitrate)
        self.current_level = 0
        self.builder.set_idle(channel, 0)

    def send_bits(self, bits_list):
        for bit in bits_list:
            if bit == 0:
                self.current_level = 1 - self.current_level
            self.builder.set_level(self.channel, self.current_level, self.bit_width)

    def send_bytes(self, data):
        # Send preamble: 32 zero bits (creates 16 rising edges for sync,
        # since each zero bit toggles level and only half the toggles are rising)
        for _ in range(32):
            self.send_bits([0])
        # Send data bytes MSB first
        for byte in data:
            for i in range(7, -1, -1):
                bit = (byte >> i) & 1
                if bit == 0:
                    self.current_level = 1 - self.current_level
                self.builder.set_level(self.channel, self.current_level, self.bit_width)

