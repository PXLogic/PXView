import math
from .base import *

class SdcardSdGenerator:
    """SD card (SD mode) protocol generator.
    2 channels: CMD(ch0), CLK(ch1).
    Command token: 48 bits = start(0) + transmission(1) + 6-bit cmd + 32-bit arg + 7-bit CRC + end(1).
    CMD is sampled on CLK rising edge."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.cmd = channels_map.get('cmd', 0)
        self.clk = channels_map.get('clk', 1)
        self.half_period = max(2, samplerate // 400000)
        self.builder.set_idle(self.clk, 0)
        self.builder.set_idle(self.cmd, 1)

    def _crc7(self, bits_list):
        """Compute CRC7 for SD command."""
        crc = 0
        for b in bits_list:
            di = b ^ ((crc >> 6) & 1)
            crc = (crc & 0x07) | ((crc & 0x38) << 1) | ((di ^ ((crc >> 2) & 1)) << 3)
            crc = (crc & 0x78) | ((crc & 0x03) << 1) | di
        return crc & 0x7F

    def _send_bit(self, bit):
        """Send one bit on CMD with CLK rising edge sampling."""
        period = self.half_period * 2
        # Set CLK low for full period, overlay CMD at same time
        self.builder.set_level(self.clk, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.cmd, bit, period)
        self.builder.pos -= period
        # CLK high then low (rising edge samples CMD)
        self.builder.set_level(self.clk, 1, self.half_period)
        self.builder.set_level(self.clk, 0, self.half_period)

    def send_command(self, cmd, arg=0x00000000):
        """Send an SD command token (48 bits).
        Format: start(0) + transmission(1) + 6-bit cmd + 32-bit arg + 7-bit CRC + end(1)."""
        bits = []
        # Start bit = 0
        bits.append(0)
        # Transmission bit = 1 (host)
        bits.append(1)
        # 6-bit command (MSB first)
        for i in range(5, -1, -1):
            bits.append((cmd >> i) & 1)
        # 32-bit argument (MSB first)
        for i in range(31, -1, -1):
            bits.append((arg >> i) & 1)
        # 7-bit CRC (over start+transmission+cmd+arg = 40 bits)
        crc = self._crc7(bits[:40])
        for i in range(6, -1, -1):
            bits.append((crc >> i) & 1)
        # End bit = 1
        bits.append(1)

        # CMD idle high before command
        self.builder.set_level(self.cmd, 1, self.half_period * 4)
        for b in bits:
            self._send_bit(b)
        # CMD idle high after command
        self.builder.set_level(self.cmd, 1, self.half_period * 4)

    def generate_testdata(self):
        # Send CMD0 (GO_IDLE_STATE) - no response needed
        self.send_command(0, 0x00000000)
        # Send CMD8 (SEND_IF_COND)
        self.send_command(8, 0x000001AA)
