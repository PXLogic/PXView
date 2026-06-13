import math
from .base import *

class WiegandGenerator:
    """Wiegand 26-bit protocol generator (2 channels: DATA0, DATA1)."""
    def __init__(self, builder, ch0, ch1, samplerate=1000000):
        self.builder = builder
        self.data0 = ch0  # Bit 0 channel
        self.data1 = ch1  # Bit 1 channel
        self.sr = samplerate
        self.pulse_width = int(50 * samplerate / 1e6)   # 50us pulse
        self.inter_gap = int(1000 * samplerate / 1e6)    # 1ms gap
        # Both idle high
        self.builder.set_idle(ch0, 1)
        self.builder.set_idle(ch1, 1)

    def _send_bit(self, bit):
        if bit == 0:
            # Pulse DATA0 low for pulse_width, DATA1 stays high
            self.builder.set_level(self.data0, 0, self.pulse_width)
            self.builder.pos -= self.pulse_width
            self.builder.set_level(self.data1, 1, self.pulse_width)
            # Both high for inter-bit gap
            self.builder.set_level(self.data0, 1, self.inter_gap)
            self.builder.pos -= self.inter_gap
            self.builder.set_level(self.data1, 1, self.inter_gap)
        else:
            # Pulse DATA1 low for pulse_width, DATA0 stays high
            self.builder.set_level(self.data1, 0, self.pulse_width)
            self.builder.pos -= self.pulse_width
            self.builder.set_level(self.data0, 1, self.pulse_width)
            # Both high for inter-bit gap
            self.builder.set_level(self.data0, 1, self.inter_gap)
            self.builder.pos -= self.inter_gap
            self.builder.set_level(self.data1, 1, self.inter_gap)

    def send_wiegand26(self, facility=0x01, card=0x0001):
        # Initial idle gap
        self.builder.set_level(self.data0, 1, 2000)
        self.builder.pos -= 2000
        self.builder.set_level(self.data1, 1, 2000)
        
        # Bits 1-8: facility code MSB first
        fac_bits = []
        for i in range(7, -1, -1):
            fac_bits.append((facility >> i) & 1)
        # Bits 9-24: card number MSB first
        card_bits = []
        for i in range(15, -1, -1):
            card_bits.append((card >> i) & 1)
        # Even parity over bits 1-12 (facility + first 4 card bits)
        parity_bits = fac_bits + card_bits
        even_count = sum(parity_bits[:12])
        ep = 1 if even_count % 2 == 1 else 0  # Make total even
        # Odd Parity over bits 13-24 (last 12 card bits)
        odd_count = sum(parity_bits[12:24])
        op = 1 if odd_count % 2 == 0 else 0  # Make total odd
        # Full 26 bits: even_parity + 8 facility + 16 card + odd_parity
        all_bits = [ep] + fac_bits + card_bits + [op]
        for bit in all_bits:
            self._send_bit(bit)

