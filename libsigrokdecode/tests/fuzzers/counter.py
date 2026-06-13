import math
from .base import *

class CounterGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        data = self.channels_map.get("data", 0)
        
        samples_per_pulse = int(10e-6 * self.samplerate)
        
        state = 0
        self.builder.set_level(data, state, samples_per_pulse * 10)
        
        for _ in range(10):
            state = 1 - state
            self.builder.set_level(data, state, samples_per_pulse)
