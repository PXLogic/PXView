import math
from .base import *
from .spi import SPIGenerator

class Ad79x0Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        miso = self.channels_map.get("miso", 1)
        cs = self.channels_map.get("cs", 2)
        gen = SPIGenerator(self.builder, clk, -1, miso, cs, speed=100000)
        
        # Shift out 16 bits (2 bytes)
        gen.select()
        gen.write_byte(0x12)
        gen.write_byte(0x34)
        gen.deselect()
