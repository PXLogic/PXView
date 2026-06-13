import math
from .base import *

class CANGenerator:
    def __init__(self, builder, channel, bitrate=500000):
        self.builder = builder
        self.channel = channel
        self.bit_width = int(builder.samplerate / bitrate)
        self.builder.set_idle(channel, 1) # Recessive idle

    def _crc15_can(self, bits):
        crc = 0
        for bit in bits:
            if (bit ^ (crc >> 14)) & 1:
                crc = (crc << 1) ^ 0x4599
            else:
                crc <<= 1
        return crc & 0x7fff

    def send_frame(self, ident, data, ide=False, rtr=False):
        raw_bits = [0] # SOF
        # ID
        if not ide:
            for i in range(10, -1, -1): raw_bits.append((ident >> i) & 1)
            raw_bits.extend([rtr, 0, 0]) # RTR, IDE, r0
        else:
            # Extended ID not implemented in this snippet for brevity, but same logic
            pass
        
        # DLC
        dlc = len(data)
        for i in range(3, -1, -1): raw_bits.append((dlc >> i) & 1)
        # Data
        for b in data:
            for i in range(7, -1, -1): raw_bits.append((b >> i) & 1)
        
        # CRC
        crc = self._crc15_can(raw_bits[1:])
        for i in range(14, -1, -1): raw_bits.append((crc >> i) & 1)
        
        # Stuffing (SOF to CRC)
        stuffed = [raw_bits[0]]
        consecutive = 1
        last = raw_bits[0]
        for b in raw_bits[1:]:
            if b == last:
                consecutive += 1
            else:
                consecutive = 1
                last = b
            stuffed.append(b)
            if consecutive == 5:
                stuffed.append(1 - last)
                last = 1 - last
                consecutive = 1
        
        # Trailer (Fixed form)
        stuffed.extend([1, 0, 1]) # CRC Delim (R), ACK (D), ACK Delim (R)
        stuffed.extend([1]*7) # EOF
        stuffed.extend([1]*3) # IFS

        # Write stuffed bits to channel (NRZ: recessive=1, dominant=0)
        for bit in stuffed:
            self.builder.set_level(self.channel, bit, self.bit_width)
        
