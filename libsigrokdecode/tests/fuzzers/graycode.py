import math
from .base import *

class GraycodeGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        d0 = self.channels_map.get("d0", 0)
        d1 = self.channels_map.get("d1", 1)
        
        samples_per_step = int(10e-6 * self.samplerate)
        
        seq = [(0, 0), (0, 1), (1, 1), (1, 0), (0, 0)]
        
        self.builder.write_channels({d0: 0, d1: 0}, samples_per_step * 5)
        
        # Go forward
        for v0, v1 in seq:
            self.builder.write_channels({d0: v0, d1: v1}, samples_per_step)
            
        # Go backward
        for v0, v1 in reversed(seq):
            self.builder.write_channels({d0: v0, d1: v1}, samples_per_step)
