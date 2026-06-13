import math
from .base import *

class CarreraGenerator:
    """Carrera slot car digital protocol generator.
    1 channel: data.
    The decoder detects edges spaced by ~100us as valid bits.
    Edges spaced >6000us mark word boundaries.
    Bits are read from the data line level at each edge."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def send_controller_word(self):
        """Send a Carrera controller word (Reglerwort).
        A controller word has 9 bits: 3-bit ID + 4-bit gas + 1-bit WT + 1-bit TA.
        ID=0, gas=5, WT=0, TA=0 → dataWord = 0b000010100 = 0x0A."""
        data = self.channels_map.get("data", 0)

        # ~100us between edges for valid bits
        bit_interval = int(100e-6 * self.samplerate)

        # Controller word: ID=0 (3 bits: 000), gas=5 (4 bits: 0101), WT=0, TA=0
        # In the decoder, bits are shifted in MSB-first: dataWord <<= 1; if pin==bit_val: dataWord |= 1
        # bit_val = invert ? 1 : 0 (default invert=nein, so bit_val=0)
        # So a '1' bit = data line at 0 (matching bit_val), a '0' bit = data line at 1
        # Wait, re-reading: bit_val = s->invert ? 1 : 0 = 0 (not inverted)
        # if (pin == bit_val) dataWord |= 1 → pin=0 means bit=1, pin=1 means bit=0
        # So inverted logic: low level = 1, high level = 0

        # Controller word ID=0, gas=5, WT=0, TA=0
        # Binary: 000 0101 0 0 = 000010100
        # With inverted logic: bit=1 → pin=0, bit=0 → pin=1
        bits = [0, 0, 0, 0, 1, 0, 1, 0, 0]  # ID=000, gas=0101, WT=0, TA=0

        # Start with a gap (>6000us)
        gap_samples = int(7000e-6 * self.samplerate)
        self.builder.set_level(data, 1, gap_samples)

        for bit in bits:
            # Toggle data line to create an edge
            # For bit=1: pin should be 0 (low), for bit=0: pin should be 1 (high)
            pin_level = 0 if bit == 1 else 1
            self.builder.set_level(data, pin_level, bit_interval)

        # End with a gap (>6000us) to flush the word
        gap_samples = int(7000e-6 * self.samplerate)
        self.builder.set_level(data, 1, gap_samples)

    def generate_testdata(self):
        self.send_controller_word()
