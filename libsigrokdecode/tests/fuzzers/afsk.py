import math
from .base import *

class AfskGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.ch = channels_map.get("afsk", 0)
        self.samplerate = samplerate
        
    def generate_testdata(self):
        mark_hz = 2000
        space_hz = 4000
        
        mark_half_cycle = int(self.samplerate * ((1 / mark_hz) / 2))
        space_half_cycle = int(self.samplerate * ((1 / space_hz) / 2))
        
        # SPACE (0) -> two space half cycles
        self.builder.set_level(self.ch, 1, space_half_cycle)
        self.builder.set_level(self.ch, 0, space_half_cycle)
        
        # MARK (1) -> two mark half cycles
        self.builder.set_level(self.ch, 1, mark_half_cycle)
        self.builder.set_level(self.ch, 0, mark_half_cycle)
