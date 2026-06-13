import math
from .base import *

class AdatGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.ch = channels_map.get("adat", 0)
        self.samplerate = samplerate
        # ADAT uses a fixed bit time. 
        # The samplerate is e.g. 100MHz, and bit rate is 256 * 48000 = 12.288MHz
        # so bit time is ~81.3ns (or 8 samples at 100MHz).
        # We will dynamically calculate bit_time_samples.
        self.bit_time_samples = int(self.samplerate / (256 * 48000))
        if self.bit_time_samples < 2:
            self.bit_time_samples = 2
        
    def generate_testdata(self):
        # 1. Sync pad: 1 followed by 10 zeros
        self.send_bit(1)
        for _ in range(10):
            self.send_bit(0)
            
        # 2. User bits: 1 followed by 4 user bits
        self.send_bit(1)
        self.send_bit(1)
        self.send_bit(0)
        self.send_bit(1)
        self.send_bit(0)
        
        # 3. Channel data: 8 channels, 6 nibbles each.
        # Nibble = 1 followed by 4 bits
        for ch in range(8):
            for nibble in range(6):
                self.send_bit(1)
                self.send_bit(1)
                self.send_bit(0)
                self.send_bit(0)
                self.send_bit(1)
                
    def send_bit(self, bit):
        self.builder.set_level(self.ch, bit, self.bit_time_samples)
