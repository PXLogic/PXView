import math
from .base import *

class CjtagGenerator:
    """2 channels: TCKC(ch0), TMSC(ch1)
    In FOUR_WIRE mode, TMSC carries TMS.
    Generates a JTAG state machine traversal on TCKC/TMSC."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.tckc = channels_map.get('tckc', 0)
        self.tmsc = channels_map.get('tmsc', 1)
        self.half_period = max(2, int(samplerate / 200000))  # 100kHz JTAG clock
        # Set idle states
        self.builder.set_idle(self.tckc, 0)
        self.builder.set_idle(self.tmsc, 0)

    def _clock_cycle(self, tms):
        """One TCKC cycle: set TMSC=TMS, TCKC rising then falling."""
        period = self.half_period * 2
        # Set TCKC low and TMSC for the full period
        self.builder.set_level(self.tckc, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.tmsc, tms, period)
        self.builder.pos -= period
        # TCKC high for first half (rising edge triggers decode)
        self.builder.set_level(self.tckc, 1, self.half_period)
        self.builder.set_level(self.tckc, 0, self.half_period)

    def generate_testdata(self):
        # Go to TEST-LOGIC-RESET: TMS=1 for 5 TCKC cycles
        for _ in range(5):
            self._clock_cycle(1)
        # Go to RUN-TEST/IDLE: TMS=0
        self._clock_cycle(0)
        # Go to SELECT-DR-SCAN: TMS=1
        self._clock_cycle(1)
        # Go to CAPTURE-DR: TMS=0
        self._clock_cycle(0)
        # Go to SHIFT-DR: TMS=0
        self._clock_cycle(0)
        # Shift 8 DR bits (LSB first), TMS=0 for all but last
        data = 0xA5
        for i in range(7):
            self._clock_cycle(0)  # TMS=0, stay in SHIFT-DR
        # Last bit: TMS=1 to exit SHIFT-DR -> EXIT1-DR
        self._clock_cycle(1)
        # EXIT1-DR -> UPDATE-DR: TMS=1
        self._clock_cycle(1)
        # UPDATE-DR -> RUN-TEST/IDLE: TMS=0
        self._clock_cycle(0)
        # Now go to IR path
        # RUN-TEST/IDLE -> SELECT-DR-SCAN: TMS=1
        self._clock_cycle(1)
        # SELECT-DR-SCAN -> SELECT-IR-SCAN: TMS=1
        self._clock_cycle(1)
        # SELECT-IR-SCAN -> CAPTURE-IR: TMS=0
        self._clock_cycle(0)
        # CAPTURE-IR -> SHIFT-IR: TMS=0
        self._clock_cycle(0)
        # Shift 4 IR bits, TMS=0 for all but last
        for i in range(3):
            self._clock_cycle(0)
        # Last bit: TMS=1 -> EXIT1-IR
        self._clock_cycle(1)
        # EXIT1-IR -> UPDATE-IR: TMS=1
        self._clock_cycle(1)
        # UPDATE-IR -> RUN-TEST/IDLE: TMS=0
        self._clock_cycle(0)
