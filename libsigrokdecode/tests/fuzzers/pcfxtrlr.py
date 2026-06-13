import math
from .base import *

class PcfxtrlrGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        # TODO: Reverse engineer the protocol state machine and generate valid waveform
        # For now, this does nothing, which will result in 0 annotations (WARN).
        pass
