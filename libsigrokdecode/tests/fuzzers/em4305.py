import math
from .base import *

class EM4305Generator:
    """EM4305 RFID protocol generator.
    1 channel: DATA(ch0).
    The decoder looks for:
    1. First Field Stop (FFS): a gap > 40*field_clock samples
    2. Write gaps: gaps > 12*field_clock samples
    3. Between gaps: short on-time = bit 0, long on-time = bit 1
    field_clock = samplerate / coilfreq (default 125kHz)
    At 1MHz samplerate: field_clock = 8 samples, FFS > 320 samples, write_gap > 96 samples"""
    def __init__(self, builder, channel=0, samplerate=1000000, coilfreq=125000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        self.field_clock = max(1, int(samplerate / coilfreq))
        builder.set_idle(channel, 1)  # Idle high (field present)

    def _send_gap(self, duration_field_clocks):
        """Send a gap (low) for the specified number of field clocks."""
        self.builder.set_level(self.ch, 0, duration_field_clocks * self.field_clock)

    def _send_field(self, duration_field_clocks):
        """Send field (high) for the specified number of field clocks."""
        self.builder.set_level(self.ch, 1, duration_field_clocks * self.field_clock)

    def send_write_word(self, address=2, data=0x00000000):
        """Send an EM4305 write word command.
        First Field Stop + Write gaps encoding bits.
        Write word format: 0 + cmd(3) + addr(4+2) + data(32) + col_parity(8) + stop = 50 bits
        Simplified: send FFS then a sequence of gap-encoded bits."""
        fc = self.field_clock
        # First Field Stop: gap > 40 field clocks
        self._send_field(100)  # Field before FFS
        self._send_gap(50)     # FFS: > 40 field clocks
        self._send_field(20)   # Field after FFS

        # Now send write-gap encoded bits
        # Each bit: field on for some time, then a gap
        # Bit 0: short on (15-27 fc) + gap
        # Bit 1: long on (> 27 fc, < 300 fc) + gap
        # Write gap: > 12 field clocks

        # Send a simple sequence: logic 0 + cmd 010 (Write) + address + data
        # For simplicity, send alternating 0s and 1s
        bits = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
                0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0]
        for b in bits:
            if b:
                # Bit 1: long on time (30 field clocks) then gap
                self._send_field(30)
            else:
                # Bit 0: short on time (20 field clocks) then gap
                self._send_field(20)
            # Write gap: 15 field clocks
            self._send_gap(15)

        # End: long field (no gap = write mode exit)
        self._send_field(400)

