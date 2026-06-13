import math
from .base import *

class SevenSegmentGenerator:
    """7-segment display generator.
    7 channels (a-g) + optional dp.
    The decoder waits for any edge on any channel, then looks up the digit
    from the previous state of the segment lines.
    To trigger: set segments to a valid pattern, then change any segment."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        a = self.channels_map.get("a", 0)
        b = self.channels_map.get("b", 1)
        c = self.channels_map.get("c", 2)
        d = self.channels_map.get("d", 3)
        e = self.channels_map.get("e", 4)
        f = self.channels_map.get("f", 5)
        g = self.channels_map.get("g", 6)

        # Digit '0' = segments a,b,c,d,e,f on, g off = 0x3F
        # Digit '1' = segments b,c on = 0x06
        # Digit '5' = segments a,c,d,f,g on = 0x6D

        digits = [
            # (a, b, c, d, e, f, g) for each digit
            (1, 1, 1, 1, 1, 1, 0),  # '0'
            (0, 1, 1, 0, 0, 0, 0),  # '1'
            (1, 1, 0, 1, 1, 0, 1),  # '2'
            (1, 1, 1, 1, 0, 0, 1),  # '3'
        ]

        step = max(2, self.samplerate // 10000)  # 100us per step

        for segs in digits:
            # Set all segment lines simultaneously
            self.builder.write_channels({
                a: segs[0], b: segs[1], c: segs[2],
                d: segs[3], e: segs[4], f: segs[5], g: segs[6],
            }, step)

        # Change one segment to trigger the last digit annotation
        self.builder.write_channels({
            a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0,
        }, step)
