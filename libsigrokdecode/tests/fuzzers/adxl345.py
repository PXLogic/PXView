import math
from .base import *
from .spi import SPIGenerator

class Adxl345Generator:
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
        
        # Write to Reg 0x31 (DATA_FORMAT)
        gen.select()
        gen.write_byte(0x31) # Write, Single, Addr=0x31
        gen.write_byte(0x0B) # Full res, 16g
        gen.deselect()
