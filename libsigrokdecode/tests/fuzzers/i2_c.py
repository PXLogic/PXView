import math
from .base import *

class I2CGenerator:
    def __init__(self, builder, scl_ch, sda_ch, speed=100000):
        self.builder = builder
        self.scl = scl_ch
        self.sda = sda_ch
        self.half_period = int(builder.samplerate / (speed * 2))
        # Idle state
        self.builder.set_idle(scl_ch, 1)
        self.builder.set_idle(sda_ch, 1)

    def start(self):
        # SCL high, SDA falls
        self.builder.set_level(self.scl, 1, self.half_period)
        self.builder.pos -= self.half_period # Overlap
        self.builder.set_level(self.sda, 1, self.half_period // 2)
        self.builder.set_level(self.sda, 0, self.half_period // 2)

    def stop(self):
        # SCL high, SDA rises
        self.builder.set_level(self.scl, 0, self.half_period)
        self.builder.set_level(self.sda, 0, self.half_period)
        self.builder.set_level(self.scl, 1, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.sda, 0, self.half_period // 2)
        self.builder.set_level(self.sda, 1, self.half_period // 2)

    def write_byte(self, val):
        for i in range(7, -1, -1):
            bit = (val >> i) & 1
            self.builder.set_level(self.scl, 0, self.half_period)
            self.builder.pos -= self.half_period
            self.builder.set_level(self.sda, bit, self.half_period)
            self.builder.set_level(self.scl, 1, self.half_period)
        # ACK slot
        self.builder.set_level(self.scl, 0, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.sda, 0, self.half_period) # Assume ACK
        self.builder.set_level(self.scl, 1, self.half_period)

