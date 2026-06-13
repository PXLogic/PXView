import math
from .base import *

class MipiDsiGenerator:
    """MIPI DSI Low Power communication generator.
    2 channels: D0N(ch0), D0P(ch1).
    Protocol state machine:
    1. FIND_START: wait D0N falling + D0P high -> record ss
    2. FIND_MODE_S0: wait D0N low + D0P low
    3. FIND_MODE_S1: wait (D0N high + D0P low) OR (D0N low + D0P high) -> save pins
    4. FIND_MODE_S2: wait D0N low + D0P low -> handle ESC/BTA
    5. FIND_DATA_EDGE: wait D0N high OR D0P high -> save data pins
    6. FIND_DATA_VALID: wait D0P low OR D0P high -> handle data bit or stop"""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.d0n = channels_map.get('D0N', 0)
        self.d0p = channels_map.get('D0P', 1)
        self.half_period = max(2, samplerate // 200000)

    def generate_testdata(self):
        d0n = self.d0n
        d0p = self.d0p
        hp = self.half_period

        # Idle: D0N=1, D0P=1 (LP-11)
        self.builder.set_level(d0n, 1, hp * 10)
        self.builder.set_level(d0p, 1, hp * 10)

        # FIND_START: D0N falls while D0P is high
        # D0N goes low, D0P stays high (LP-01)
        self.builder.set_level(d0n, 0, hp * 2)
        self.builder.set_level(d0p, 1, hp * 2)

        # FIND_MODE_S0: D0N low, D0P goes low (LP-00)
        self.builder.set_level(d0n, 0, hp * 2)
        self.builder.set_level(d0p, 0, hp * 2)

        # FIND_MODE_S1: D0N goes high, D0P stays low (LP-10)
        # This means saved_d0n=1 -> BTA mode
        self.builder.set_level(d0n, 1, hp * 2)
        self.builder.set_level(d0p, 0, hp * 2)

        # FIND_MODE_S2: D0N goes low, D0P stays low (LP-00)
        self.builder.set_level(d0n, 0, hp * 2)
        self.builder.set_level(d0p, 0, hp * 2)

        # Now in DATA phase - send 8 data bits (one byte)
        # Each bit: FIND_DATA_EDGE (one pin high) then FIND_DATA_VALID (both low or both high)
        # Bit value = data_d0p (D0P value at the edge)
        byte_val = 0x55
        for bit_idx in range(8):
            bit = (byte_val >> (7 - bit_idx)) & 1

            # FIND_DATA_EDGE: wait for D0N high OR D0P high
            # For bit=1: D0P goes high (data_d0p=1)
            # For bit=0: D0N goes high (data_d0p=0)
            if bit:
                self.builder.set_level(d0n, 0, hp)
                self.builder.set_level(d0p, 1, hp)
            else:
                self.builder.set_level(d0n, 1, hp)
                self.builder.set_level(d0p, 0, hp)

            # FIND_DATA_VALID: wait for D0P low OR D0P high
            # For data bit: D0P goes low (both low -> data bit)
            # di_matched & (1<<0) means first condition matched -> data bit
            self.builder.set_level(d0n, 0, hp)
            self.builder.set_level(d0p, 0, hp)

        # Stop condition: D0N high + D0P high (LP-11)
        # In FIND_DATA_EDGE: one pin high
        self.builder.set_level(d0n, 1, hp)
        self.builder.set_level(d0p, 0, hp)
        # In FIND_DATA_VALID: D0P goes high (both high -> stop)
        self.builder.set_level(d0n, 1, hp)
        self.builder.set_level(d0p, 1, hp)

        # Idle
        self.builder.set_level(d0n, 1, hp * 10)
        self.builder.set_level(d0p, 1, hp * 10)
