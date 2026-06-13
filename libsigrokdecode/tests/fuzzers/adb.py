import math
from .base import *

class AdbGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.ch = channels_map.get("data", 0)
        self.samplerate = samplerate
        self.builder.set_idle(self.ch, 1)

    def to_samples(self, us):
        return int(us * self.samplerate / 1000000)

    def generate_testdata(self):
        # Attention (560-1040us)
        self.builder.set_level(self.ch, 0, self.to_samples(800))
        # Start bit after attention: High for e.g. 200us
        self.builder.set_level(self.ch, 1, self.to_samples(200))

        # Send Listen(addr=2, reg=3) = 0x2B (Wait, Listen is 0x08, addr is top 4 bits. 0x28 + 0x03 = 0x2B)
        self.send_byte(0x2B)
        
        # Stop bit (0): low < 100, high >= 100.
        self.builder.set_level(self.ch, 0, self.to_samples(70))
        self.builder.set_level(self.ch, 1, self.to_samples(120))

    def send_byte(self, val):
        for i in range(7, -1, -1):
            bit = (val >> i) & 1
            if bit == 0:
                self.builder.set_level(self.ch, 0, self.to_samples(65))
                self.builder.set_level(self.ch, 1, self.to_samples(35))
            else:
                self.builder.set_level(self.ch, 0, self.to_samples(35))
                self.builder.set_level(self.ch, 1, self.to_samples(65))
