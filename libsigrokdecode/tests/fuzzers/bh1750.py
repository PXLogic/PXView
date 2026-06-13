import math
from .base import *
from .i2_c import I2CGenerator

class Bh1750Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        scl = self.channels_map.get("scl", 0)
        sda = self.channels_map.get("sda", 1)
        gen = I2CGenerator(self.builder, scl, sda, speed=100000, samplerate=self.samplerate)
        
        # PWRDOWN
        gen.start()
        gen.write_byte(0x46) # Address 0x23, Write
        gen.write_byte(0x00) # PWRDOWN
        gen.stop()
