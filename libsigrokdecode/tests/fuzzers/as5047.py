import math
from .base import *
from .spi import SPIGenerator

class As5047Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        mosi = self.channels_map.get("mosi", 1)
        miso = self.channels_map.get("miso", 2)
        cs = self.channels_map.get("csn", 3)
        gen = SPIGenerator(self.builder, clk, mosi, miso, cs, speed=100000)
        
        # 16-bit word: 0x0018 (SETTINGS1), write mode
        # Parity = 0 (even number of 1s in 0x0018)
        gen.select()
        gen.write_byte(0x00)
        gen.write_byte(0x18)
        gen.deselect()
        
        # Second 16-bit word: 0x0000 (Data)
        gen.select()
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.deselect()
