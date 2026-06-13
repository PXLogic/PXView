import math
from .base import *

class St7735Generator:
    """ST7735 LCD controller generator (SPI-based).
    4 channels: cs(0), clk(1), mosi(2), dc(3).
    CS low selects device. CLK edges sample MOSI. DC low=command, DC high=data.
    Data is MSB-first, sampled on CLK rising edge."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        cs = self.channels_map.get("cs", 0)
        clk = self.channels_map.get("clk", 1)
        mosi = self.channels_map.get("mosi", 2)
        dc = self.channels_map.get("dc", 3)

        half_period = max(2, self.samplerate // 200000)  # 5us half-period

        # Idle: CS high, CLK low, MOSI low, DC low
        self.builder.set_level(cs, 1, half_period * 2)
        self.builder.set_level(clk, 0, 0)   # overlay
        self.builder.set_level(mosi, 0, 0)  # overlay
        self.builder.set_level(dc, 0, 0)    # overlay

        # CS low: select device
        self.builder.set_level(cs, 0, half_period)

        # Send command byte 0x29 (DISPON) with DC=low
        self.builder.set_level(dc, 0, 0)  # overlay: DC low for command
        cmd_byte = 0x29
        for i in range(7, -1, -1):  # MSB first
            bit = (cmd_byte >> i) & 1
            self.builder.set_level(mosi, bit, 0)  # overlay
            self.builder.set_level(clk, 1, half_period)
            self.builder.set_level(clk, 0, half_period)

        # Send data byte 0x55 with DC=high
        self.builder.set_level(dc, 1, 0)  # overlay: DC high for data
        data_byte = 0x55
        for i in range(7, -1, -1):  # MSB first
            bit = (data_byte >> i) & 1
            self.builder.set_level(mosi, bit, 0)  # overlay
            self.builder.set_level(clk, 1, half_period)
            self.builder.set_level(clk, 0, half_period)

        # CS high: deselect
        self.builder.set_level(cs, 1, half_period * 2)
