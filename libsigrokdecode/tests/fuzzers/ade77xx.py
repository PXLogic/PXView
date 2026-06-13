import math
from .base import *
from .spi import SPIGenerator

class Ade77xxGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        mosi = self.channels_map.get("mosi", 1)
        miso = self.channels_map.get("miso", 2)
        cs = self.channels_map.get("cs", 3)
        gen = SPIGenerator(self.builder, clk, mosi, miso, cs, speed=100000)
        
        # Write cmd 0x81 (Write to AWATTHR which is a 16 bit reg)
        gen.select()
        gen.write_byte(0x81)
        gen.write_byte(0xAA)
        gen.write_byte(0xBB)
        gen.deselect()
