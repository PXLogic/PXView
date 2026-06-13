import math
from .base import *

class MapleBusGenerator:
    """Maple Bus (SEGA Dreamcast) generator.
    2 channels: SDCKA(ch0), SDCKB(ch1).
    Start: SDCKA low + SDCKB high, then 4 SDCKB falls before SDCKA rise = Start.
    Byte: 4 bit-pairs via SDCKA fall->read SDCKB, SDCKB fall->read SDCKA.
    Both lines idle high (open-drain with pull-ups).
    The Python decoder waits for SDCKA falling OR SDCKB falling, and uses
    if/elif (SDCKA fell checked first). When both fall simultaneously,
    only SDCKA fell is processed. The generator must ensure proper edge ordering."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.sdcka = channels_map.get('sdcka', 0)
        self.sdckb = channels_map.get('sdckb', 1)
        self.bit_width = max(2, samplerate // 2000000)  # ~2MHz bus
        builder.set_idle(self.sdcka, 1)
        builder.set_idle(self.sdckb, 1)

    def _set_both(self, a, b, duration):
        """Set both channels simultaneously."""
        self.builder.set_level(self.sdcka, a, duration)
        self.builder.pos -= duration
        self.builder.set_level(self.sdckb, b, duration)

    def _send_start(self):
        """Send start pattern: SDCKA=low, SDCKB=high, then 4 SDCKB falling edges
        before SDCKA rises."""
        # Both idle high initially
        # SDCKA goes low while SDCKB stays high
        self._set_both(0, 1, self.bit_width)
        # 4 SDCKB falling edges while SDCKA stays low
        for i in range(4):
            # SDCKB high, SDCKA low
            self._set_both(0, 1, self.bit_width)
            # SDCKB low, SDCKA low (SDCKB falling edge)
            self._set_both(0, 0, self.bit_width)
        # SDCKA rises (end of start pattern), SDCKB stays high
        self._set_both(1, 1, self.bit_width)

    def _send_bit_pair(self, bit_a, bit_b):
        """Send one bit-pair: SDCKA falls -> read SDCKB (bit_a), SDCKB falls -> read SDCKA (bit_b).
        Both lines start high and end high.

        The decoder waits for SDCKA falling OR SDCKB falling (if/elif order).
        When both fall simultaneously, only SDCKA fell is processed.

        Sequence:
        1. SDCKA falls. SDCKB is at bit_a level (simultaneous transition if bit_a=0).
           Decoder reads SDCKB = bit_a. counta++.
        2. Set SDCKA to bit_b level (rising edge if bit_b=1, decoder ignores rising).
        3. If SDCKB is low (bit_a=0), release SDCKB high (rising edge, decoder ignores).
        4. Drive SDCKB low (falling edge). Decoder reads SDCKA = bit_b. countb++.
        5. Both lines go high (release).
        """
        # Step 1: SDCKA falls, SDCKB set to bit_a
        # If bit_a=1: SDCKB stays high (no SDCKB edge)
        # If bit_a=0: SDCKB goes low simultaneously (SDCKB fell is ignored by if/elif)
        self._set_both(0, bit_a, self.bit_width)

        # Step 2: Set SDCKA to bit_b level
        if bit_b == 1:
            # SDCKA goes high (rising edge, decoder ignores)
            self._set_both(1, bit_a, self.bit_width)
        # If bit_b=0, SDCKA stays low (no change needed)

        # Step 3: If SDCKB is low (bit_a=0), release it high
        if bit_a == 0:
            # SDCKB goes high (rising edge, decoder ignores)
            if bit_b == 1:
                self._set_both(1, 1, self.bit_width)
            else:
                self._set_both(0, 1, self.bit_width)

        # Step 4: Drive SDCKB low (falling edge). Decoder reads SDCKA = bit_b.
        self._set_both(bit_b, 0, self.bit_width)

        # Step 5: Both lines go high (release)
        self._set_both(1, 1, self.bit_width)

    def _send_byte(self, val):
        """Send one byte: 4 bit-pairs.
        Bits are collected MSB first (bit0 first, bit1 second per pair)."""
        for bitpair in range(4):
            bit_a = (val >> (7 - bitpair * 2)) & 1
            bit_b = (val >> (6 - bitpair * 2)) & 1
            self._send_bit_pair(bit_a, bit_b)

    def _send_end(self):
        """Send end pattern: both high, then SDCKA falls with SDCKB=0,
        then both high again. The Python decoder detects end when:
        counta==1, countb==0, data==0, sdckb==0 on SDCKA fell, then
        SDCKA rises while SDCKB is high."""
        # Both high briefly
        self._set_both(1, 1, self.bit_width)
        # SDCKA falls with SDCKB=0 (decoder sees: counta=1,countb=0,data=0,sdckb=0)
        self._set_both(0, 0, self.bit_width)
        # Wait for both high (decoder waits for SDCKA=high, SDCKB=high)
        self._set_both(1, 1, self.bit_width)
        # SDCKA falls again (decoder checks: matched & 0b1 -> got_end)
        self._set_both(0, 1, self.bit_width)
        # Both high
        self._set_both(1, 1, self.bit_width)

    def send_frame(self, size_byte, src_ap, dst_ap, command, data_bytes=None):
        """Send a complete Maple Bus frame: start + size + src + dst + cmd + data + checksum + end."""
        if data_bytes is None:
            data_bytes = []
        all_bytes = [size_byte, src_ap, dst_ap, command] + list(data_bytes)
        # Calculate checksum: XOR of all bytes
        chk = 0
        for b in all_bytes:
            chk ^= b
        all_bytes.append(chk)

        self._send_start()
        for b in all_bytes:
            self._send_byte(b)
        self._send_end()

    def generate_testdata(self):
        """Generate test data: a simple Maple Bus frame."""
        # size_byte=0 means 1 word (4 bytes) of data after header
        # Frame: size(0) + src(0) + dst(0) + cmd(1=Device request) + data(0x00000001) + checksum
        self.send_frame(
            size_byte=0,    # 1 word of data
            src_ap=0,       # Source address
            dst_ap=0,       # Destination address
            command=1,      # Device request
            data_bytes=[0x01, 0x00, 0x00, 0x00]  # 4 bytes of data
        )
