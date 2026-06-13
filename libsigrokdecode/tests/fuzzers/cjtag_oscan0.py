import math
from .base import *


class CjtagOscan0Generator:
    """cJTAG OScan0 (IEEE 1149.7) protocol generator.

    4 channels: TDI(ch0), TDO(ch1), TCK(ch2), TMS(ch3).
    The decoder starts in 4-wire JTAG mode and can detect cJTAG escape
    sequences (6 TMS changes while TCK is high) to enter OAC mode.

    We generate a standard 4-wire JTAG sequence:
    1. Reset TAP (5 TMS=1 cycles)
    2. Go to RUN-TEST/IDLE (TMS=0)
    3. Navigate to SHIFT-DR and shift some data
    4. Navigate to SHIFT-IR and shift some data
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.tdi = channels_map.get('tdi', 0)
        self.tdo = channels_map.get('tdo', 1)
        self.tck = channels_map.get('tck', 2)
        self.tms = channels_map.get('tms', 3)
        self.half_period = max(2, samplerate // 200000)

    def _clock_cycle(self, tdi_val, tdo_val, tms_val):
        """One TCK cycle: set TDI/TDO/TMS, TCK rising then falling."""
        hp = self.half_period
        # Set data lines while TCK is low
        self.builder.set_level(self.tdi, tdi_val, 0)
        self.builder.set_level(self.tdo, tdo_val, 0)
        self.builder.set_level(self.tms, tms_val, 0)
        # TCK rising edge (decoder samples data)
        self.builder.set_level(self.tck, 1, hp)
        # TCK falling edge
        self.builder.set_level(self.tck, 0, hp)

    def generate_testdata(self):
        # Set idle states
        self.builder.set_idle(self.tck, 0)
        self.builder.set_idle(self.tms, 0)
        self.builder.set_idle(self.tdi, 0)
        self.builder.set_idle(self.tdo, 0)

        # Initial idle
        self.builder.set_level(self.tck, 0, self.half_period * 10)

        # Step 1: Reset TAP - TMS=1 for 5 TCK cycles
        for _ in range(5):
            self._clock_cycle(1, 1, 1)

        # Step 2: Go to RUN-TEST/IDLE: TMS=0
        self._clock_cycle(0, 0, 0)

        # Step 3: Navigate to SHIFT-DR:
        # RUN-TEST/IDLE -> SELECT-DR-SCAN (TMS=1)
        self._clock_cycle(0, 0, 1)
        # SELECT-DR-SCAN -> CAPTURE-DR (TMS=0)
        self._clock_cycle(0, 0, 0)
        # CAPTURE-DR -> SHIFT-DR (TMS=0)
        self._clock_cycle(0, 0, 0)

        # Step 4: Shift 8 DR bits (LSB first), TMS=0 for all but last
        dr_data = 0xA5
        for i in range(7):
            bit = (dr_data >> i) & 1
            self._clock_cycle(bit, bit, 0)  # TMS=0, stay in SHIFT-DR

        # Last bit: TMS=1 to exit SHIFT-DR -> EXIT1-DR
        bit = (dr_data >> 7) & 1
        self._clock_cycle(bit, bit, 1)

        # EXIT1-DR -> UPDATE-DR: TMS=1
        self._clock_cycle(0, 0, 1)

        # UPDATE-DR -> RUN-TEST/IDLE: TMS=0
        self._clock_cycle(0, 0, 0)

        # Step 5: Navigate to SHIFT-IR:
        # RUN-TEST/IDLE -> SELECT-DR-SCAN (TMS=1)
        self._clock_cycle(0, 0, 1)
        # SELECT-DR-SCAN -> SELECT-IR-SCAN (TMS=1)
        self._clock_cycle(0, 0, 1)
        # SELECT-IR-SCAN -> CAPTURE-IR (TMS=0)
        self._clock_cycle(0, 0, 0)
        # CAPTURE-IR -> SHIFT-IR (TMS=0)
        self._clock_cycle(0, 0, 0)

        # Step 6: Shift 4 IR bits, TMS=0 for all but last
        ir_data = 0x02
        for i in range(3):
            bit = (ir_data >> i) & 1
            self._clock_cycle(bit, bit, 0)

        # Last bit: TMS=1 -> EXIT1-IR
        bit = (ir_data >> 3) & 1
        self._clock_cycle(bit, bit, 1)

        # EXIT1-IR -> UPDATE-IR: TMS=1
        self._clock_cycle(0, 0, 1)

        # UPDATE-IR -> RUN-TEST/IDLE: TMS=0
        self._clock_cycle(0, 0, 0)

        # Idle for a while
        self.builder.set_level(self.tck, 0, self.half_period * 20)
