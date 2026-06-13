import math
from .base import *

class C2Generator:
    """C2 interface (Silabs) protocol generator.
    2 channels: C2CK(ch0), C2D(ch1).
    Reset is detected when C2CK is high for >20us.
    After reset: Start (1 C2CK rising), INS (2 bits on C2CK rising),
    then address/data depending on instruction.
    The decoder samples C2D on C2CK rising edge."""
    def __init__(self, builder, c2ck_ch=0, c2d_ch=1, samplerate=1000000):
        self.builder = builder
        self.c2ck = c2ck_ch
        self.c2d = c2d_ch
        self.sr = samplerate
        # C2CK period ~5us (200kHz), half-period ~2.5us
        self.half_period = max(2, int(samplerate / 400000))  # 2.5us per half-period
        builder.set_idle(c2ck_ch, 0)
        builder.set_idle(c2d_ch, 1)

    def _clock_pulse(self, c2d_val):
        """One C2CK cycle: set C2D before rising edge, then clock high, then low.
        Decoder samples C2D on C2CK rising edge."""
        hp = self.half_period
        # Set C2D value (must be stable before rising edge)
        self.builder.set_level(self.c2d, c2d_val, hp * 2)
        self.builder.pos -= hp * 2
        # C2CK low half (already low from previous or idle)
        self.builder.set_level(self.c2ck, 0, hp)
        # C2CK rising edge - decoder samples C2D here
        self.builder.set_level(self.c2ck, 1, hp)
        # C2CK falling edge
        self.builder.set_level(self.c2ck, 0, 0)

    def send_reset(self):
        """Send a C2 reset: C2CK high for >20us, then low.
        The decoder detects reset when C2CK high interval > 20us.
        Then send a complete Data Read transaction."""
        hp = self.half_period
        # Ensure C2CK starts low
        self.builder.set_level(self.c2ck, 0, hp)
        # C2CK high for 25us (must be >20us for reset detection)
        reset_samples = max(1, int(25 * self.sr / 1e6))
        self.builder.set_level(self.c2ck, 1, reset_samples)
        # C2CK goes low - falling edge ends reset
        self.builder.set_level(self.c2ck, 0, hp)

        # Start: 1 C2CK rising edge with C2D=1
        self._clock_pulse(1)

        # INS: 2 bits LSB first - instruction 0b00 = Data Read
        self._clock_pulse(0)  # INS bit 0
        self._clock_pulse(0)  # INS bit 1

        # Data Read Length: 2 bits LSB first (0b00 = 1 byte)
        self._clock_pulse(0)  # Length bit 0
        self._clock_pulse(0)  # Length bit 1

        # Wait: C2D goes high (slave drives C2D high to indicate ready)
        self._clock_pulse(1)

        # Data Read: 8 bits LSB first
        data_val = 0x55
        for i in range(8):
            self._clock_pulse((data_val >> i) & 1)

        # End: 1 C2CK cycle
        self._clock_pulse(1)

