import math
from .base import *

class DCCGenerator:
    """DCC model railway protocol generator."""
    def __init__(self, builder, channel, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.sr = samplerate
        self.builder.set_idle(channel, 1)

    def _us(self, us):
        return int(us * self.sr / 1e6)

    def _send_bit(self, bit):
        if bit:
            # '1': 58us high + 58us low
            self.builder.set_level(self.channel, 1, self._us(58))
            self.builder.set_level(self.channel, 0, self._us(58))
        else:
            # '0': 100us high + 100us low
            self.builder.set_level(self.channel, 1, self._us(100))
            self.builder.set_level(self.channel, 0, self._us(100))

    def send_packet(self, address=0x03, instruction=0x7F):
        # Preamble: 14+ bits of '1'
        for _ in range(14):
            self._send_bit(1)
        # Start bit
        self._send_bit(0)
        # Address byte MSB first
        for i in range(7, -1, -1):
            self._send_bit((address >> i) & 1)
        # Instruction byte MSB first
        for i in range(7, -1, -1):
            self._send_bit((instruction >> i) & 1)
        # Error detection: XOR of address and instruction
        xor_byte = address ^ instruction
        for i in range(7, -1, -1):
            self._send_bit((xor_byte >> i) & 1)
        # End bit
        self._send_bit(0)

