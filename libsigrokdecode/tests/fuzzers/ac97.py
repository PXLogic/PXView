import math
from .base import *

class AC97Generator:
    """AC97 audio codec protocol generator.
    Channels: SYNC(ch0), BIT_CLK(ch1), SDATA_OUT(ch2), SDATA_IN(ch3).
    AC97 uses 256 BIT_CLK cycles per frame.
    SYNC goes high for 16 CLK cycles (tag phase), then low for 240 cycles.
    Data is sampled at falling CLK edges.
    The decoder detects frame start when SYNC goes from 0 to 1."""
    def __init__(self, builder, sync_ch=0, clk_ch=1, out_ch=2, in_ch=3, samplerate=24000000):
        self.builder = builder
        self.sync = sync_ch
        self.clk = clk_ch
        self.out = out_ch
        self.in_ = in_ch
        self.sr = samplerate
        # BIT_CLK = 12.288MHz, one half-clock = sr/12.288e6 samples
        self.half_clk = max(1, int(samplerate / 12288000))
        builder.set_idle(sync_ch, 0)
        builder.set_idle(clk_ch, 0)
        if out_ch >= 0:
            builder.set_idle(out_ch, 0)
        if in_ch >= 0:
            builder.set_idle(in_ch, 0)

    def _clk_half(self):
        """One half BIT_CLK cycle: high then low."""
        self.builder.set_level(self.clk, 1, self.half_clk)
        self.builder.set_level(self.clk, 0, self.half_clk)

    def _send_bit(self, sync_val, out_val=0, in_val=0):
        """Send one BIT_CLK cycle with given SYNC, SDATA_OUT, SDATA_IN values.
        Data is latched at falling CLK edge. SYNC changes at rising CLK edge."""
        # Rising CLK edge: SYNC changes
        self.builder.set_level(self.sync, sync_val, 0)
        if self.out >= 0:
            self.builder.set_level(self.out, out_val, 0)
        if self.in_ >= 0:
            self.builder.set_level(self.in_, in_val, 0)
        # CLK high half
        self.builder.set_level(self.clk, 1, self.half_clk)
        # Falling CLK edge: data is sampled
        # CLK low half
        self.builder.set_level(self.clk, 0, self.half_clk)

    def send_frame(self, tag_out=0x9800, slot_data=None):
        """Send one AC97 frame (256 BIT_CLK cycles).
        tag_out: 16-bit TAG for SDATA_OUT (bit 15=ready, bits 14:3=valid, bits 2:0=codec)
        slot_data: list of up to 12 slot values (20 bits each) for SDATA_OUT."""
        if slot_data is None:
            slot_data = [0x1234] * 12

        # Build SDATA_OUT bit stream: slot0(16 bits) + slot1-12(20 bits each) = 256 bits
        out_bits = []
        # Slot 0: TAG (16 bits) MSB first
        for i in range(15, -1, -1):
            out_bits.append((tag_out >> i) & 1)
        # Slots 1-12: 20 bits each MSB first
        for slot in slot_data[:12]:
            for i in range(19, -1, -1):
                out_bits.append((slot >> i) & 1)

        # Build SDATA_IN bit stream (same structure, simple response)
        in_bits = [0] * 256
        # Slot 0 TAG for input: ready=1, valid for slots 1-2
        in_bits[0] = 1  # ready
        in_bits[1] = 1  # slot 1 valid
        in_bits[2] = 1  # slot 2 valid

        # Send 256 BIT_CLK cycles
        # First 16 cycles: SYNC high (tag phase)
        for i in range(16):
            self._send_bit(1, out_bits[i] if i < len(out_bits) else 0,
                          in_bits[i] if i < len(in_bits) else 0)
        # Remaining 240 cycles: SYNC low (data phase)
        for i in range(16, 256):
            self._send_bit(0, out_bits[i] if i < len(out_bits) else 0,
                          in_bits[i] if i < len(in_bits) else 0)

