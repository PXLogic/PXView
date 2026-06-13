import math
from .base import *
from .spi import SPIGenerator

class Adf435xGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        mosi = self.channels_map.get("data", 1)
        cs = self.channels_map.get("le", 2)
        gen = SPIGenerator(self.builder, clk, mosi, -1, cs, speed=100000)
        
        # Write 32 bits (4 bytes).
        gen.select()
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.deselect()
