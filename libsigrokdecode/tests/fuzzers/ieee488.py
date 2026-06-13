import math
from .base import *

class Ieee488Generator:
    """IEEE-488 IEC serial mode generator.
    17 channels: DIO1(ch0)=DATA, DIO2-DIO8(ch1-7), EOI(ch8), DAV(ch9),
    NRFD(ch10), NDAC(ch11), IFC(ch12), SRQ(ch13), ATN(ch14), REN(ch15), CLK(ch16).
    When CLK is connected, the decoder takes the serial/IEC path.

    IEC serial protocol (all pins are read as-is, only ATN is inverted by decoder):
    1. WAIT READY TO SEND: DATA low (pin=0) AND CLK high (pin=1)
    2. WAIT READY FOR DATA: DATA high (pin=1) AND CLK high (pin=1)
    3. PREP DATA / TEST EOI: CLK goes low (pin=0) -> start clocking
       Optional: DATA goes low while CLK high = EOI confirm
    4. CLOCK DATA BITS: CLK rising (pin=1) = latch DATA, CLK falling (pin=0) = end of bit
       8 bits per byte, LSB first
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.data_ch = channels_map.get('dio1', 0)
        self.clk_ch = channels_map.get('clk', 16)
        self.atn_ch = channels_map.get('atn', 14)
        self.eoi_ch = channels_map.get('eoi', 8)
        self.bit_width = max(2, samplerate // 10000)  # ~10kHz serial bus

        # Set idle states - CLK and DATA idle high (inactive for IEC bus)
        # ATN idle high (inactive, not asserted)
        for ch_name, ch_idx in channels_map.items():
            builder.set_idle(ch_idx, 1)  # All idle high

    def generate_testdata(self):
        b = self.builder
        data = self.data_ch
        clk = self.clk_ch
        atn = self.atn_ch
        bw = self.bit_width

        # All lines idle high initially
        # ATN is active-low: physical low = ATN asserted

        # Send UNL command (0x3F) with ATN asserted
        self._send_serial_byte(0x3F, atn_assert=True)

        # Send LAG 0 (0x20) with ATN asserted
        self._send_serial_byte(0x20, atn_assert=True)

        # Send a data byte (0x41 = 'A') with ATN not asserted
        self._send_serial_byte(0x41, atn_assert=False)

    def _send_serial_byte(self, val, atn_assert=False):
        """Send one byte via IEC serial protocol."""
        b = self.builder
        data = self.data_ch
        clk = self.clk_ch
        atn = self.atn_ch
        bw = self.bit_width

        # Assert ATN if needed (physical low = asserted)
        if atn_assert:
            b.set_level(atn, 0, bw)

        # Step 1: WAIT READY TO SEND
        # Need: DATA low (pin=0) AND CLK high (pin=1)
        b.set_level(data, 0, bw)
        b.pos -= bw
        b.set_level(clk, 1, bw)

        # Step 2: WAIT READY FOR DATA
        # Need: DATA high (pin=1) AND CLK high (pin=1)
        b.set_level(data, 1, bw)
        b.pos -= bw
        b.set_level(clk, 1, bw)

        # Step 3: PREP DATA / TEST EOI (skip EOI)
        # CLK goes low (pin=0) -> start clocking
        b.set_level(clk, 0, bw)

        # Step 4: CLOCK DATA BITS - 8 bits, LSB first
        for bit_idx in range(8):
            bit_val = (val >> bit_idx) & 1

            # Set DATA before CLK rises
            b.set_level(data, bit_val, bw)
            b.pos -= bw
            b.set_level(clk, 0, bw)  # CLK still low

            # CLK rises (pin=1): latch DATA
            b.set_level(clk, 1, bw)

            # CLK falls (pin=0): end of bit
            b.set_level(clk, 0, bw)

        # Release CLK and DATA to idle high
        b.set_level(clk, 1, bw * 2)
        b.pos -= bw * 2
        b.set_level(data, 1, bw * 2)

        # Release ATN if it was asserted
        if atn_assert:
            b.set_level(atn, 1, bw * 2)
