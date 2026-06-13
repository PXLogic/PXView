import math
from .base import *
from .i2_c import I2CGenerator

class Atsha204aGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        scl = self.channels_map.get("scl", 0)
        sda = self.channels_map.get("sda", 1)
        gen = I2CGenerator(self.builder, scl, sda, speed=100000, samplerate=self.samplerate)
        
        # WORD_ADDR_RESET = 0x00
        # Send an ADDRESS WRITE
        # Send WORD_ADDR
        # Stop
        gen.start()
        gen.write_byte(0xA0) # Address write
        gen.write_byte(0x00) # WORD_ADDR_RESET
        gen.stop()
