import math
from .base import *

class OOKGenerator:
    """On-Off Keying generator with Manchester encoding.
    1 channel: DATA(ch0).
    The OOK decoder needs:
    1. A preamble of 7+ Manchester pulses to lock onto the signal
    2. Manchester-encoded data bits
    3. A timeout (5x pulse period of low) to trigger DECODE_TIMEOUT and emit annotations
    Default: Manchester encoding with 1111 preamble (all-high pattern)"""
    def __init__(self, builder, channel=0, bitrate=1000, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.bit_width = max(2, int(samplerate / bitrate))
        self.half_width = self.bit_width // 2
        builder.set_idle(channel, 0)

    def _manchester_bit_1111(self, bit):
        """Manchester encoding for 1111 preamble mode.
        Each bit is 2 half-periods. 1=high-then-low, 0=low-then-high."""
        if bit:
            self.builder.set_level(self.ch, 1, self.half_width)
            self.builder.set_level(self.ch, 0, self.half_width)
        else:
            self.builder.set_level(self.ch, 0, self.half_width)
            self.builder.set_level(self.ch, 1, self.half_width)

    def _manchester_bit_1010(self, bit):
        """Manchester encoding for 1010 preamble mode (clock-rate transitions).
        Same encoding, but the preamble pattern differs."""
        if bit:
            self.builder.set_level(self.ch, 1, self.half_width)
            self.builder.set_level(self.ch, 0, self.half_width)
        else:
            self.builder.set_level(self.ch, 0, self.half_width)
            self.builder.set_level(self.ch, 1, self.half_width)

    def send_bits(self, bits):
        """Send a sequence of Manchester-encoded bits with preamble and timeout.
        Generates: 8-bit 1111 preamble + data bits + long low for timeout."""
        # Initial idle low so the first rising edge is detected
        self.builder.set_level(self.ch, 0, self.bit_width * 4)
        # Preamble: 8 bits of alternating 1-0-1-0 (creates 16 edges for sync)
        # The decoder needs 7+ valid pulses to lock on
        for _ in range(4):
            self._manchester_bit_1111(1)
            self._manchester_bit_1111(0)
        # Data bits
        for b in bits:
            self._manchester_bit_1111(b)
        # Timeout: long low period (5x bit_width) to trigger DECODE_TIMEOUT
        self.builder.set_level(self.ch, 0, self.bit_width * 6)

