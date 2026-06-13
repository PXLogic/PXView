import math
from .base import *

class BeanGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        data = self.channels_map.get("data", 0)
        
        samples_per_bit = int(100e-6 * self.samplerate)
        
        # set_level advances pos automatically
        self.builder.set_pos(self.builder.get_pos() + 10 * samples_per_bit)
        
        def send_bits(val, count):
            self.builder.set_level(data, val, count * samples_per_bit)

            
        # Initial state 0
        send_bits(0, 10)
        
        # SOF: 1 bit of 1 (not added to bits)
        send_bits(1, 1)
        
        # We need to send bits without ever having a pulse >= 5 bits (500 samples)
        # to avoid bit stuffing or aborting.
        # We will just alternate 1 and 0 for all bytes.
        # PRI (4 bits): 0101
        send_bits(0, 1)
        send_bits(1, 1)
        send_bits(0, 1)
        send_bits(1, 1)
        
        # ML (4 bits): 0011 (length 3, so frame_length=3, expecting 6 bytes total: PRI/ML, DST, MES, DATA, CRC, EOM)
        # Wait, if we send 0011, we have two 0s and two 1s. This is fine.
        send_bits(0, 2)
        send_bits(1, 2)
        
        # DST-ID (8 bits): 01010101
        for _ in range(4):
            send_bits(0, 1)
            send_bits(1, 1)
            
        # MES-ID (8 bits): 01010101
        for _ in range(4):
            send_bits(0, 1)
            send_bits(1, 1)
            
        # DATA (8 bits): 01010101
        for _ in range(4):
            send_bits(0, 1)
            send_bits(1, 1)
            
        # CRC (8 bits): 01010101
        for _ in range(4):
            send_bits(0, 1)
            send_bits(1, 1)
            
        # EOM byte (8 bits): 01010101. This is the 5th byte, making len(self.bits) = 48.
        for _ in range(4):
            send_bits(0, 1)
            send_bits(1, 1)
            
        # Now we need to trigger self.EOM = 1.
        # We do this by sending a pulse of length 600 samples (count == 6).
        # We'll use state 1 for this pulse (since the last bit was 1, wait, we must transition to 0 then to 1).
        send_bits(0, 1)  # Extra bit just to transition
        send_bits(1, 6)  # The 600 sample pulse to set EOM
        
        # Now we need to trigger self.draw = 1.
        # It happens on the NEXT edge if self.EOM == 1, and the pulse is <= 150 (count == 1).
        send_bits(0, 1)
        
        # Wait > 650 samples to trigger any remaining state resets
        send_bits(1, 10)
