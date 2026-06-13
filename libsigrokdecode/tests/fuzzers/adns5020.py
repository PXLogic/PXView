import math
from .base import *
from .spi import SPIGenerator

class Adns5020Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("sclk", 0)
        mosi = self.channels_map.get("mosi", 1)
        cs = self.channels_map.get("cs", 2)
        gen = SPIGenerator(self.builder, clk, mosi, -1, cs, speed=100000)
        
        # Write 2 bytes (0x00, 0x00) for a read of Product_ID
        gen.select()
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.deselect()
