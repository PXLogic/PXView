import math
from .base import *

class SPIGenerator:
    def __init__(self, builder, clk, mosi, miso, cs, speed=1000000, cpol=0, cpha=0):
        self.builder = builder
        self.clk = clk
        self.mosi = mosi
        self.miso = miso
        self.cs = cs
        self.half_period = int(builder.samplerate / (speed * 2))
        self.cpol = cpol
        self.cpha = cpha
        self.builder.set_idle(cs, 1)
        self.builder.set_idle(clk, cpol)

    def select(self):
        self.builder.set_level(self.cs, 0, self.half_period)

    def deselect(self):
        self.builder.set_level(self.cs, 1, self.half_period)

    def write_byte(self, val):
        for i in range(7, -1, -1):
            bit = (val >> i) & 1
            if self.cpol == 0:
                # Clock idles low
                self.builder.set_level(self.mosi, bit, 0)
                self.builder.set_level(self.clk, 1, self.half_period)
                self.builder.set_level(self.clk, 0, self.half_period)
            else:
                # Clock idles high
                self.builder.set_level(self.mosi, bit, 0)
                self.builder.set_level(self.clk, 0, self.half_period)
                self.builder.set_level(self.clk, 1, self.half_period)

