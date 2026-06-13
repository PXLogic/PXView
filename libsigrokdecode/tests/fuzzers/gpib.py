import math
from .base import *

class GPIBGenerator:
    """GPIB (IEEE-488) protocol generator.
    Channels: DIO1-DIO8(ch0-7), EOI(ch8), DAV(ch9), NRFD(ch10), NDAC(ch11), IFC(ch12), SRQ(ch13), ATN(ch14), REN(ch15)
    All handshake signals active low.
    The decoder waits for falling DAV edge, then processes data at rising DAV edge.
    Source handshake: set data → NRFD high → assert DAV low → wait NDAC high → release DAV high."""
    def __init__(self, builder, bit_width=100):
        self.builder = builder
        self.bw = bit_width
        # Set idle states - all handshake lines inactive (high), data low
        for ch in range(8):
            builder.set_idle(ch, 0)
        builder.set_idle(8, 1)   # EOI inactive (high)
        builder.set_idle(9, 1)   # DAV inactive (high)
        builder.set_idle(10, 1)  # NRFD inactive (high)
        builder.set_idle(11, 1)  # NDAC inactive (high)
        builder.set_idle(12, 1)  # IFC inactive (high)
        builder.set_idle(13, 1)  # SRQ inactive (high)
        builder.set_idle(14, 1)  # ATN inactive (high)
        builder.set_idle(15, 0)  # REN active (low) - remote enable

    def _send_byte_handshake(self, val, atn=False, eoi=False):
        """Send one byte with proper GPIB handshake using write_channels.
        1. Set data on DIO lines (inverted) + ATN + EOI + NRFD high
        2. Assert DAV low (data valid)
        3. NDAC high (data accepted)
        4. Release DAV high (decoder processes data on rising DAV)"""
        bw = self.bw
        inverted = val ^ 0xFF  # GPIB data is inverted (active low)
        # Phase 1: Data on bus, ATN/EOI set, NRFD high (acceptor ready), DAV high (not yet valid)
        levels = {i: (inverted >> i) & 1 for i in range(8)}
        levels[8] = 0 if eoi else 1   # EOI
        levels[9] = 1                  # DAV high (not yet valid)
        levels[10] = 1                 # NRFD high (acceptor ready)
        levels[11] = 1                 # NDAC high
        levels[12] = 1                 # IFC
        levels[13] = 1                 # SRQ
        levels[14] = 0 if atn else 1   # ATN (active low for commands)
        levels[15] = 0                 # REN
        self.builder.write_channels(levels, bw)
        # Phase 2: Assert DAV low (data valid) - decoder detects falling DAV
        levels[9] = 0  # DAV low
        self.builder.write_channels(levels, bw)
        # Phase 3: NDAC high (data accepted by all listeners)
        levels[11] = 1  # NDAC already high
        self.builder.write_channels(levels, bw)
        # Phase 4: Release DAV high - decoder processes data on rising DAV edge
        levels[9] = 1  # DAV high
        self.builder.write_channels(levels, bw)

    def send_command(self, cmd_byte):
        """Send a command byte with ATN asserted."""
        self._send_byte_handshake(cmd_byte, atn=True)

    def send_data(self, data_byte, eoi=False):
        """Send a data byte with ATN not asserted."""
        self._send_byte_handshake(data_byte, atn=False, eoi=eoi)

    def command_sequence(self):
        """Send UNL + LAG 0 command sequence."""
        # UNL (Unlisten) = 0x3F with ATN
        self.send_command(0x3F)
        # LAG 0 (Listen Address Group 0) = 0x20 with ATN
        self.send_command(0x20)
        # Send a data byte
        self.send_data(0x41)  # 'A'

