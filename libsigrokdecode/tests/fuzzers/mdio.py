import math
from .base import *

class MDIOGenerator:
    """2 channels: MDC(ch0), MDIO(ch1)"""
    def __init__(self, builder, mdc_ch, mdio_ch, speed=100000):
        self.builder = builder
        self.mdc = mdc_ch
        self.mdio = mdio_ch
        self.half_period = max(1, int(builder.samplerate / (speed * 2)))
        self.builder.set_idle(mdc_ch, 0)
        self.builder.set_idle(mdio_ch, 1)

    def _send_bit(self, bit):
        """Set MDIO stable, then MDC high, MDC low."""
        period = self.half_period * 2
        # Set MDC low for full period, then overlay MDIO
        self.builder.set_level(self.mdc, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.mdio, bit, period)
        self.builder.pos -= period
        # MDC high for first half, low for second half
        self.builder.set_level(self.mdc, 1, self.half_period)
        self.builder.set_level(self.mdc, 0, self.half_period)

    def read_clause22(self, phy_addr=0x01, reg_addr=0x00, data=0x1141):
        """Generate Clause 22 read frame."""
        # PREAMBLE: 32 bits of 1
        for _ in range(32):
            self._send_bit(1)

        # START: 01
        self._send_bit(0)
        self._send_bit(1)

        # OPCODE: 10 (read)
        self._send_bit(1)
        self._send_bit(0)

        # PHY ADDR: 5 bits MSB first
        for i in range(4, -1, -1):
            self._send_bit((phy_addr >> i) & 1)

        # REG ADDR: 5 bits MSB first
        for i in range(4, -1, -1):
            self._send_bit((reg_addr >> i) & 1)

        # TURNAROUND: Z (high, MDIO high-Z), then 0
        self._send_bit(1)  # High-Z (slave releases, pull-up)
        self._send_bit(0)  # Slave drives 0

        # DATA: 16 bits MSB first (slave drives)
        for i in range(15, -1, -1):
            self._send_bit((data >> i) & 1)

