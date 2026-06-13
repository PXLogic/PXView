import math
from .base import *

class MCS48Generator:
    """MCS-48 (8048 family) bus protocol generator.
    Channels: ALE(ch0), PSEN(ch1), D0-D7(ch2-9), A8-A11(ch10-13)
    Matches C decoder channel mapping.
    The decoder samples address on falling ALE edge, data on rising PSEN edge.
    ALE is active high, PSEN is active low."""
    def __init__(self, builder, bit_width=100):
        self.builder = builder
        self.bw = bit_width
        builder.set_idle(0, 0)   # ALE idle low
        builder.set_idle(1, 1)   # PSEN idle high (active low)
        for ch in range(2, 14):
            builder.set_idle(ch, 0)  # Data/address idle low

    def opcode_fetch(self, addr, opcode):
        """Generate an opcode fetch cycle.
        1. Put address on D0-D7 (low byte) and A8-A11 (high nibble)
        2. ALE goes high then low (falling edge latches address)
        3. Put opcode on D0-D7
        4. PSEN goes low then high (rising edge reads data)"""
        bw = self.bw
        # Phase 1: Address on bus, ALE high
        levels = {0: 1}  # ALE=1
        for i in range(8):
            levels[2 + i] = (addr >> i) & 1  # D0-D7 = addr low byte
        for i in range(4):
            levels[10 + i] = (addr >> (8 + i)) & 1  # A8-A11
        self.builder.write_channels(levels, bw)
        # Phase 2: ALE falling edge (address latched), ALE low
        levels[0] = 0  # ALE=0
        self.builder.write_channels(levels, bw)
        # Phase 3: Opcode on data bus, PSEN low
        levels = {1: 0}  # PSEN=0
        for i in range(8):
            levels[2 + i] = (opcode >> i) & 1  # D0-D7 = opcode
        for i in range(4):
            levels[10 + i] = (addr >> (8 + i)) & 1  # A8-A11 stay
        self.builder.write_channels(levels, bw)
        # Phase 4: PSEN rising edge (data read), PSEN high
        levels[1] = 1  # PSEN=1
        self.builder.write_channels(levels, bw)

