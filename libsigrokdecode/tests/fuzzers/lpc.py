import math
from .base import *

class LPCGenerator:
    """LPC (Low Pin Count) bus protocol generator.
    Channels: LFRAME(ch0), LCLK(ch1), LAD0-LAD3(ch2-5)
    The decoder samples on rising LCLK edge.
    LFRAME# low starts cycle, then START field on LAD, then CT/DR, ADDR, TAR, SYNC, DATA, TAR2.
    For I/O write: START=0x0, CT/DR=0x2, ADDR(4 nibbles MSN-first), TAR(0xF x2), SYNC=0x0, DATA(2 nibbles LSN-first), TAR2(0xF x2)."""
    def __init__(self, builder, bit_width=50):
        self.builder = builder
        self.bw = bit_width
        builder.set_idle(0, 1)  # LFRAME# idle high (inactive)
        builder.set_idle(1, 0)  # LCLK idle low
        for ch in range(2, 6):
            builder.set_idle(ch, 0)  # LAD idle low

    def _lad_clock(self, lframe_val, val4):
        """Set LFRAME + LAD value, then generate one LCLK pulse (rising then falling).
        Decoder samples LAD on rising LCLK edge."""
        bw = self.bw
        # Set LFRAME and LAD for the full clock cycle (2*bw = high + low)
        levels = {0: lframe_val, 1: 1}  # LCLK=1 for first half
        for i in range(4):
            levels[2 + i] = (val4 >> i) & 1
        self.builder.write_channels(levels, bw)
        # LCLK goes low for second half (LFRAME and LAD stay the same)
        levels[1] = 0
        self.builder.write_channels(levels, bw)

    def io_write(self, addr, data):
        """Generate an I/O write cycle.
        Sequence: LFRAME# low + START(0x0) + LFRAME# high + CT/DR(0x2) +
        ADDR(4 nibbles, MSN-first) + DATA(2 nibbles, LSN-first) + TAR2(0xF x3).
        Note: For writes, the decoder goes directly from GET_ADDR to GET_DATA
        (no TAR or SYNC between ADDR and DATA). TAR2 needs 3 clock edges
        because the decoder uses oldlad (previous cycle's LAD value)."""
        # Idle time before first clock edge so decoder can detect rising edge
        self.builder.write_channels({0: 1, 1: 0}, self.bw)
        # LFRAME# asserted (low) with START field on LAD
        self._lad_clock(0, 0x0)  # START = 0x0 (Start of cycle for a target)
        # LFRAME# de-asserted (high) for remaining fields
        # CT/DR field: I/O write = 0x2
        self._lad_clock(1, 0x2)
        # ADDR field: 4 nibbles for I/O (16-bit address), MSN-first
        self._lad_clock(1, (addr >> 12) & 0xF)
        self._lad_clock(1, (addr >> 8) & 0xF)
        self._lad_clock(1, (addr >> 4) & 0xF)
        self._lad_clock(1, addr & 0xF)
        # DATA: 2 nibbles, LSN-first (no TAR/SYNC for writes)
        self._lad_clock(1, data & 0xF)
        self._lad_clock(1, (data >> 4) & 0xF)
        # TAR2 (turn-around): 3 cycles of 0xF
        # (decoder uses oldlad, so needs 3 edges for 2 TAR2 cycles)
        self._lad_clock(1, 0xF)
        self._lad_clock(1, 0xF)
        self._lad_clock(1, 0xF)

