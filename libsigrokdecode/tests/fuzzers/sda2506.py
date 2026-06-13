import math
from .base import *

class Sda2506Generator:
    """SDA2506 EEPROM generator.
    3 channels: clk(0), d(1), ce(2).
    Command mode: CE=1, data sampled on CLK rising edge.
    CE falling edge triggers command parsing (needs >= 8 command bits).
    Data mode: CE=0, data on CLK falling edge."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        d = self.channels_map.get("d", 1)
        ce = self.channels_map.get("ce", 2)

        half_period = max(2, self.samplerate // 100000)  # 10us half-period

        # Idle: CLK=0, CE=1 (command mode active), D=0
        self.builder.set_level(clk, 0, half_period * 2)
        self.builder.set_level(ce, 1, 0)  # overlay
        self.builder.set_level(d, 0, 0)  # overlay

        # Send 16 command bits (write command): CB=1, addr=0x05, data=0xAA
        # Bits are sent MSB-first on rising CLK edge while CE=1
        # Command: CB(1bit) + addr(7bits) + data(8bits) = 16 bits
        # Write command: CB=1, addr=0x05, data=0xAA
        cmd_bits = [1, 0, 0, 0, 0, 1, 0, 1,  # CB=1, addr=0x05
                    1, 0, 1, 0, 1, 0, 1, 0]   # data=0xAA

        for bit in cmd_bits:
            # Set data line
            self.builder.set_level(d, bit, 0)  # overlay
            # CLK rising edge (decoder samples here)
            self.builder.set_level(clk, 1, half_period)
            # CLK falling edge
            self.builder.set_level(clk, 0, half_period)

        # CE falling edge: triggers command parsing
        # For write command (CB=1, d=0 at CE falling), needs >= 16 bits
        self.builder.set_level(d, 0, 0)  # overlay: d=0 for write command
        self.builder.set_level(ce, 0, half_period * 2)

        # Wait for CE rising edge (required by write command handler)
        self.builder.set_level(ce, 1, half_period * 2)
