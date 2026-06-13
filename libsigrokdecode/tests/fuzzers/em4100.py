import math
from .base import *

class EM4100Generator:
    """EM4100 RFID protocol generator.
    1 channel: DATA(ch0). Manchester encoded, 64 bits."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        self.bit_period = int(samplerate * 64 / 1000000)
        self.half_bit = self.bit_period // 2
        builder.set_idle(channel, 0)

    def _manchester_bit(self, bit):
        if bit:
            self.builder.set_level(self.ch, 0, self.half_bit)
            self.builder.set_level(self.ch, 1, self.half_bit)
        else:
            self.builder.set_level(self.ch, 1, self.half_bit)
            self.builder.set_level(self.ch, 0, self.half_bit)

    def send_card(self, card_id):
        """Send an EM4100 card data (simplified)."""
        for _ in range(9):
            self._manchester_bit(1)
        data_bits = []
        for i in range(39, -1, -1):
            data_bits.append((card_id >> i) & 1)
        for row in range(10):
            row_bits = data_bits[row*4:(row+1)*4]
            for b in row_bits:
                self._manchester_bit(b)
            self._manchester_bit(sum(row_bits) % 2)
        for col in range(4):
            col_p = sum(data_bits[col + row*4] for row in range(10)) % 2
            self._manchester_bit(col_p)
        self._manchester_bit(0)

