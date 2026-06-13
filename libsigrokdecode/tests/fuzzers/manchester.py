import math
from .base import *

class ManchesterGenerator:
    """Used for Ethernet (10Mbps) or IR protocols."""
    def __init__(self, builder, channel, bitrate=10e6, invert=False):
        self.builder = builder
        self.channel = channel
        self.half_width = int(builder.samplerate / (bitrate * 2))
        self.invert = invert

    def write_bit(self, bit):
        # Standard Manchester: 1 = Low-to-High, 0 = High-to-Low
        # (or IEEE 802.3: 0 = Low-to-High, 1 = High-to-Low - we follow Standard)
        if bit ^ self.invert:
            self.builder.set_level(self.channel, 0, self.half_width)
            self.builder.set_level(self.channel, 1, self.half_width)
        else:
            self.builder.set_level(self.channel, 1, self.half_width)
            self.builder.set_level(self.channel, 0, self.half_width)

