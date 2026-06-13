import math
from .base import *


class GuessBitrateGenerator:
    """Guess bitrate protocol generator.

    1 channel: data(ch0).
    The decoder waits for edges on the data line and measures the minimum
    bitwidth, then outputs bitrate = samplerate / bitwidth.

    We generate a signal with known edge spacing to produce a bitrate
    annotation. The decoder needs at least 2 edges to compute a bitrate.
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('data', 0)

    def generate_testdata(self):
        # Generate a signal at a known bitrate (9600 baud)
        # Each bit period = samplerate / 9600 samples
        bit_width = max(2, self.samplerate // 9600)

        # Start with idle high
        self.builder.set_idle(self.ch, 1)
        self.builder.set_level(self.ch, 1, bit_width * 5)

        # Generate alternating 0/1 pattern to create edges
        # This creates edges at regular intervals = bit_width
        for _ in range(20):
            self.builder.set_level(self.ch, 0, bit_width)
            self.builder.set_level(self.ch, 1, bit_width)
