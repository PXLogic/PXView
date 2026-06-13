import math
from .base import *

class Ad5593rGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        scl = self.channels_map.get("scl", 0)
        sda = self.channels_map.get("sda", 1)
        
        # We need I2CGenerator, let's try to import it from i2c
        # If not, we can just use the builder directly, but we have I2CGenerator in fuzzers.i2c
        from .i2c import I2CGenerator
        gen = I2CGenerator(self.builder, scl, sda, samplerate=self.samplerate)
        
        gen.start()
        gen.write_byte(0x20) # Slave address 0b0010000 + Write(0) = 0x20
        gen.write_byte(0x01) # Pointer byte (e.g., config)
        gen.write_byte(0x12) # Data High
        gen.write_byte(0x34) # Data Low
        gen.stop()
