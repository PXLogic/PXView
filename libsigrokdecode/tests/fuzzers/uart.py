import math
from .base import *

class UARTGenerator:
    def __init__(self, builder, channel, baud=9600, data_bits=8, parity='none', stop_bits=1):
        self.builder = builder
        self.channel = channel
        self.bit_width = int(builder.samplerate / baud)
        self.data_bits = data_bits
        self.parity = parity
        self.stop_bits = stop_bits
        self.builder.set_idle(channel, 1)

    def write_byte(self, val):
        # Start bit
        self.builder.set_level(self.channel, 0, self.bit_width)
        # Data bits
        p = 0
        for i in range(self.data_bits):
            bit = (val >> i) & 1
            self.builder.set_level(self.channel, bit, self.bit_width)
            p ^= bit
        # Parity bit
        if self.parity != 'none':
            p_bit = p if self.parity == 'even' else (1 - p)
            self.builder.set_level(self.channel, p_bit, self.bit_width)
        # Stop bits
        self.builder.set_level(self.channel, 1, int(self.bit_width * self.stop_bits))

