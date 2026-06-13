import math
from .base import *

class Z80Generator:
    """Z80 CPU bus protocol generator.
    Channels: D0-D7(ch0-7), M1(ch8), RD(ch9), WR(ch10)
    Optional: MREQ(ch11), IORQ(ch12)
    Matches C decoder channel mapping.
    All control signals active low. Decoder detects cycles by control signal combinations.
    FETCH: M1=0 + MREQ=0 + RD=0
    MEMRD: MREQ=0 + RD=0 (M1=1)
    MEMWR: MREQ=0 + WR=0"""
    def __init__(self, builder, bit_width=100):
        self.builder = builder
        self.bw = bit_width
        # Data bus idle: don't care (set to 0)
        for ch in range(8):
            builder.set_idle(ch, 0)
        # Control signals idle high (inactive)
        for ch in range(8, 13):
            builder.set_idle(ch, 1)

    def m1_cycle(self, addr, opcode):
        """Generate an M1 (opcode fetch) cycle.
        M1=0 + MREQ=0 + RD=0, data bus has opcode.
        Then M1 goes high, RD goes high, then RFSH phase with MREQ=0."""
        bw = self.bw
        # Idle period: all control signals inactive, data bus 0
        levels = {ch: 1 for ch in range(8, 13)}
        self.builder.write_channels(levels, bw)
        # Assert phase: M1=0, MREQ=0, RD=0, data=opcode
        levels = {i: (opcode >> i) & 1 for i in range(8)}
        levels[8] = 0   # M1=0
        levels[9] = 0   # RD=0
        levels[10] = 1  # WR=1 (inactive)
        levels[11] = 0  # MREQ=0
        levels[12] = 1  # IORQ=1 (inactive)
        self.builder.write_channels(levels, bw * 2)
        # De-assert M1, RD; MREQ stays low for RFSH
        levels = {8: 1, 9: 1, 10: 1, 11: 0, 12: 1}
        self.builder.write_channels(levels, bw)
        # End RFSH: MREQ goes high
        levels = {ch: 1 for ch in range(8, 13)}
        self.builder.write_channels(levels, bw)

    def mem_read_cycle(self, addr, data):
        """Generate a memory read cycle (MREQ=0 + RD=0, M1=1)."""
        bw = self.bw
        # Idle period
        levels = {ch: 1 for ch in range(8, 13)}
        self.builder.write_channels(levels, bw)
        # Assert phase: MREQ=0, RD=0, M1=1, data on bus
        levels = {i: (data >> i) & 1 for i in range(8)}
        levels[8] = 1   # M1=1 (not fetch)
        levels[9] = 0   # RD=0
        levels[10] = 1  # WR=1
        levels[11] = 0  # MREQ=0
        levels[12] = 1  # IORQ=1
        self.builder.write_channels(levels, bw * 2)
        # De-assert
        levels = {ch: 1 for ch in range(8, 13)}
        self.builder.write_channels(levels, bw)

