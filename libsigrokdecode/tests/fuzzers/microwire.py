import math
from .base import *

class MicrowireGenerator:
    """4 channels: SK(ch0), SI(ch1), SO(ch2), CS(ch3)"""
    def __init__(self, builder, sk_ch, si_ch, so_ch, cs_ch, speed=100000):
        self.builder = builder
        self.sk = sk_ch
        self.si = si_ch
        self.so = so_ch
        self.cs = cs_ch
        self.half_period = max(1, int(builder.samplerate / (speed * 2)))
        self.builder.set_idle(sk_ch, 0)
        self.builder.set_idle(si_ch, 0)
        self.builder.set_idle(so_ch, 0)
        self.builder.set_idle(cs_ch, 1)

    def _send_bit(self, si_bit, so_bit=0):
        """Set SI/SO stable, then SK high, SK low."""
        period = self.half_period * 2
        # Set SK low for full period, then overlay SI and SO
        self.builder.set_level(self.sk, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.si, si_bit, period)
        self.builder.pos -= period
        self.builder.set_level(self.so, so_bit, period)
        self.builder.pos -= period
        # SK high for first half, low for second half
        self.builder.set_level(self.sk, 1, self.half_period)
        self.builder.set_level(self.sk, 0, self.half_period)

    def read(self, opcode=0b10, addr=0x00, data=0xABCD):
        """
        Generate a read command:
        - CS goes low to start
        - Send opcode (2 bits), address (8 bits), then 16 dummy clocks for data out
        - CS goes high to end
        """
        # CS low to start
        self.builder.set_level(self.cs, 0, self.half_period)

        # Opcode: 2 bits MSB first
        for i in range(1, -1, -1):
            self._send_bit((opcode >> i) & 1)

        # Address: 8 bits MSB first
        for i in range(7, -1, -1):
            self._send_bit((addr >> i) & 1)

        # Data out: 16 dummy clocks, SO has data from slave
        for i in range(15, -1, -1):
            self._send_bit(0, (data >> i) & 1)

        # CS high to end
        self.builder.set_level(self.cs, 1, self.half_period)

