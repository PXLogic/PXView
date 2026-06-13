import math
from .base import *

class SWDGenerator:
    """2 channels: SWCLK(ch0), SWDIO(ch1)"""
    def __init__(self, builder, swclk_ch, swdio_ch, speed=100000):
        self.builder = builder
        self.swclk = swclk_ch
        self.swdio = swdio_ch
        self.half_period = max(1, int(builder.samplerate / (speed * 2)))
        self.builder.set_idle(swclk_ch, 0)
        self.builder.set_idle(swdio_ch, 0)

    def _clock_bit(self, swdio_val):
        """Set SWDIO, then SWCLK high, SWCLK low."""
        period = self.half_period * 2
        # Set SWCLK low for full period, then overlay SWDIO
        self.builder.set_level(self.swclk, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.swdio, swdio_val, period)
        self.builder.pos -= period
        # SWCLK high then low
        self.builder.set_level(self.swclk, 1, self.half_period)
        self.builder.set_level(self.swclk, 0, self.half_period)

    def _turnaround(self):
        """1 clock with SWDIO high (floating/pull-up)."""
        self._clock_bit(1)

    def _read_bits(self, num_bits):
        """Read num_bits from SWDIO (simulate with known data)."""
        for _ in range(num_bits):
            self._clock_bit(1)

    def line_reset(self):
        """Line reset: 50+ clocks with SWDIO=1."""
        for _ in range(50):
            self._clock_bit(1)

    def read_dp(self, addr2=0, addr3=0, data=0x0BC11477):
        """
        Read DP register.
        addr2, addr3: A2, A3 bits of DP register address.
        data: 32-bit read value to simulate.
        """
        # Request phase: 8 bits
        # Start: 1
        start = 1
        apndp = 0  # DP
        rnw = 1    # Read
        # Parity of bits [1:4] = APnDP ^ RnW ^ A2 ^ A3
        parity = (apndp ^ rnw ^ addr2 ^ addr3) & 1
        stop = 0
        park = 1

        request_bits = [start, apndp, rnw, addr2, addr3, parity, stop, park]
        for bit in request_bits:
            self._clock_bit(bit)

        # Turnaround: 1 clock
        self._turnaround()

        # ACK: 3 clocks (001 = OK/FAULT response from target)
        # We simulate ACK=001 (OK)
        ack_bits = [1, 0, 0]  # Written LSB first on wire: bit0=1, bit1=0, bit2=0 => ACK=001
        for bit in ack_bits:
            self._clock_bit(bit)

        # Data: 32 bits LSB first
        for i in range(32):
            self._clock_bit((data >> i) & 1)

        # Data parity: even parity of 32 data bits
        data_parity = 0
        for i in range(32):
            data_parity ^= (data >> i) & 1
        self._clock_bit(data_parity)

        # Turnaround: 1 clock
        self._turnaround()

