import math
from .base import *

class OpenthermGenerator:
    """Opentherm protocol generator (Manchester encoded)."""
    def __init__(self, builder, channel, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.half_bit = int(500 * samplerate / 1e6)  # 500us half-bit
        self.builder.set_idle(channel, 0)

    def _write_manchester(self, bit):
        # Opentherm Manchester: 1 = low-to-high at mid-bit, 0 = high-to-low at mid-bit
        if bit:
            self.builder.set_level(self.channel, 0, self.half_bit)
            self.builder.set_level(self.channel, 1, self.half_bit)
        else:
            self.builder.set_level(self.channel, 1, self.half_bit)
            self.builder.set_level(self.channel, 0, self.half_bit)

    def send_read_request(self, msg_type=0x00, data_id=0x00, data_value=0x0000):
        # 32-bit frame: 1 start bit + 8 msg type + 8 data ID + 16 data value
        bits = [1]  # Start bit
        for i in range(7, -1, -1):
            bits.append((msg_type >> i) & 1)
        for i in range(7, -1, -1):
            bits.append((data_id >> i) & 1)
        for i in range(15, -1, -1):
            bits.append((data_value >> i) & 1)
        for bit in bits:
            self._write_manchester(bit)

