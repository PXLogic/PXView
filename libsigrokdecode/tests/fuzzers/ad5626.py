import math
from .base import *
from .spi import SPIGenerator

class Ad5626Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        mosi = self.channels_map.get("mosi", 1)
        cs = self.channels_map.get("cs", 3)
        gen = SPIGenerator(self.builder, clk, mosi, -1, cs, speed=100000)
        
        gen.select()
        # It expects 12 bits or more. Let's send 2 bytes (16 bits).
        gen.write_byte(0x12)
        gen.write_byte(0x34)
        gen.deselect()
