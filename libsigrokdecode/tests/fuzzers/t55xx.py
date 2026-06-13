import math
from .base import *

class T55xxGenerator:
    """T55xx RFID protocol generator.
    1 channel: DATA(ch0).
    The decoder uses field clock gap encoding:
    1. START_GAP: gap (low) > 20*field_clock triggers transition to WRITE_GAP
    2. WRITE_GAP: between gaps, on-time determines bit value
       - w_zero: 16-31 field_clocks = bit 0
       - w_one: 48-63 field_clocks = bit 1
    3. Write mode exit: no gap for > 64*field_clock samples
    field_clock = samplerate / coilfreq (default 125kHz)
    At 1MHz samplerate: field_clock = 8 samples"""
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

    def send_write_command(self, opcode=0b10, lock=0, data=0x00000000, address=0):
        """Send a T55xx write command with 70 bits (standard write).
        Format: opcode(2) + lock(1) + data(32) + address(3) = 38 bits
        Or: opcode(2) + password(32) + lock(1) + data(32) + address(3) = 70 bits
        Uses the 38-bit format (no password)."""
        fc = self.field_clock
        # Initial field before start gap
        self._send_field(100)
        # Start gap: > 20 field clocks
        self._send_gap(30)
        # Now in WRITE_GAP state - send bits via on-time between gaps
        # Bit 0: on-time 16-31 fc, Bit 1: on-time 48-63 fc
        # Write gap: > 20 field clocks between bits
        bits = []
        # Opcode: 2 bits
        for i in range(1, -1, -1):
            bits.append((opcode >> i) & 1)
        # Lock: 1 bit
        bits.append(lock)
        # Data: 32 bits
        for i in range(31, -1, -1):
            bits.append((data >> i) & 1)
        # Address: 3 bits
        for i in range(2, -1, -1):
            bits.append((address >> i) & 1)

        for b in bits:
            if b:
                # Bit 1: long on-time (55 field clocks)
                self._send_field(55)
            else:
                # Bit 0: short on-time (22 field clocks)
                self._send_field(22)
            # Write gap: 25 field clocks (> 20)
            self._send_gap(25)

        # Write mode exit: long field (> 64 field clocks with no gap)
        self._send_field(400)

