import math
from .base import *

class HDLCGenerator:
    """Synchronous HDLC generator with clock and data"""
    def __init__(self, builder, clk_ch, data_ch, en_ch=None, bitrate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.en = en_ch
        self.bit_width = int(builder.samplerate / bitrate)
        self.builder.set_idle(self.clk, 1)  # CPOL=1
        self.builder.set_idle(self.data, 1)
        if self.en is not None:
            self.builder.set_idle(self.en, 1)  # Active high

    def _crc16_ccitt(self, data):
        """CRC-16-CCITT (polynomial 0x8408, init 0xFFFF)."""
        crc = 0xFFFF
        for byte in data:
            crc ^= byte
            for _ in range(8):
                if crc & 1:
                    crc = (crc >> 1) ^ 0x8408
                else:
                    crc >>= 1
        return crc ^ 0xFFFF

    def _send_bit_nrz(self, bit):
        """Generate clock edge and data bit."""
        half_bit = self.bit_width // 2
        
        # Setup data
        self.builder.set_level(self.data, bit, half_bit)
        self.builder.pos -= half_bit
        
        # Clock transition (CPOL=1, CPHA=0 -> active low, sample on rising edge)
        self.builder.set_level(self.clk, 0, half_bit)
        
        # Hold data, clock returns high
        self.builder.set_level(self.data, bit, half_bit)
        self.builder.pos -= half_bit
        self.builder.set_level(self.clk, 1, half_bit)

    def send_frame(self, address, control, data):
        """
        Generate HDLC frame with bit-stuffing:
        Flag | Address | Control | Data | FCS | Flag
        """
        # Collect all raw bits (excluding flags) for CRC
        frame_bytes = [address, control] + list(data)
        fcs = self._crc16_ccitt(frame_bytes)
        # FCS is LSB first (little-endian)
        frame_bytes.append(fcs & 0xFF)
        frame_bytes.append((fcs >> 8) & 0xFF)

        # Opening flag: 0x7E = 01111110
        flag_bits = []
        for i in range(7, -1, -1):
            flag_bits.append((0x7E >> i) & 1)
        for b in flag_bits:
            self._send_bit_nrz(b)

        # Frame content with bit-stuffing
        consecutive_ones = 0
        for byte in frame_bytes:
            for i in range(8):
                bit = (byte >> i) & 1  # LSB first
                self._send_bit_nrz(bit)
                if bit == 1:
                    consecutive_ones += 1
                    if consecutive_ones == 5:
                        # Stuff a 0
                        self._send_bit_nrz(0)
                        consecutive_ones = 0
                else:
                    consecutive_ones = 0

        # Closing flag: 0x7E = 01111110
        for b in flag_bits:
            self._send_bit_nrz(b)

