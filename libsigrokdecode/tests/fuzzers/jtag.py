import math
from .base import *

class JTAGGenerator:
    """4 channels: TDI(ch0), TDO(ch1), TCK(ch2), TMS(ch3)
    Channel order matches C decoder: TDI=0, TDO=1, TCK=2, TMS=3"""
    def __init__(self, builder, tdi, tdo, tck, tms, speed=100000):
        self.builder = builder
        self.tdi = tdi
        self.tdo = tdo
        self.tck = tck
        self.tms = tms
        self.half_period = max(1, int(builder.samplerate / (speed * 2)))
        self.builder.set_idle(tck, 0)
        self.builder.set_idle(tms, 0)
        self.builder.set_idle(tdi, 0)
        self.builder.set_idle(tdo, 0)

    def _clock_cycle(self, tms, tdi):
        """One TCK cycle: set TDI/TMS stable, TCK high, TCK low."""
        period = self.half_period * 2
        # Set TCK low for the full period first, then overlay TMS/TDI
        self.builder.set_level(self.tck, 0, period)
        self.builder.pos -= period
        # Set TMS and TDI for the full period
        self.builder.set_level(self.tms, tms, period)
        self.builder.pos -= period
        self.builder.set_level(self.tdi, tdi, period)
        self.builder.pos -= period
        # Now write TCK high for first half
        self.builder.set_level(self.tck, 1, self.half_period)
        self.builder.set_level(self.tck, 0, self.half_period)

    def reset_tap(self):
        """Go to Test-Logic-Reset: TMS=1 for 5 TCK cycles."""
        for _ in range(5):
            self._clock_cycle(1, 0)

    def go_to_run_test_idle(self):
        """From TLR to Run-Test-Idle: TMS=0."""
        self._clock_cycle(0, 0)

    def shift_dr(self, data, num_bits=8):
        """
        Go from Run-Test-Idle to Shift-DR, shift in data (LSB first),
        then exit back to Run-Test-Idle.
        """
        # Run-Test-Idle -> Select-DR: TMS=1
        self._clock_cycle(1, 0)
        # Select-DR -> Capture-DR: TMS=0
        self._clock_cycle(0, 0)
        # Capture-DR -> Shift-DR: TMS=0
        self._clock_cycle(0, 0)

        # Shift in data bits (LSB first), keep TMS=0 for all but last bit
        for i in range(num_bits - 1):
            bit = (data >> i) & 1
            self._clock_cycle(0, bit)

        # Last bit: TMS=1 to exit Shift-DR -> Exit1-DR
        last_bit = (data >> (num_bits - 1)) & 1
        self._clock_cycle(1, last_bit)

        # Exit1-DR -> Update-DR: TMS=1
        self._clock_cycle(1, 0)
        # Update-DR -> Run-Test-Idle: TMS=0
        self._clock_cycle(0, 0)

