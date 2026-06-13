import math
from .base import *
from .spi import SPIGenerator

class AvrIspGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("sck", 0)
        mosi = self.channels_map.get("mosi", 1)
        miso = self.channels_map.get("miso", 2)
        cs = self.channels_map.get("rst", 3)
        gen = SPIGenerator(self.builder, clk, mosi, miso, cs, speed=100000)
        
        gen.select()
        # Programming enable command (4 bytes)
        gen.write_byte(0xac, 0x00)
        gen.write_byte(0x53, 0xac)
        gen.write_byte(0x00, 0x53)
        gen.write_byte(0x00, 0x00)
        gen.deselect()
