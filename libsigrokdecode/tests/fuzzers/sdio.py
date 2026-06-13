import math
from .base import *

class SDIOGenerator:
    """SDIO (Secure Digital I/O) generator.
    2 channels: CMD(ch0), CLK(ch1).
    Command token: 48 bits (start=0 + transmission=1 + 6-bit cmd + 32-bit arg + 7-bit CRC + end=1).
    CMD sampled on CLK rising edge (default polarity)."""
    def __init__(self, builder, cmd_ch=0, clk_ch=1, samplerate=1000000):
        self.builder = builder
        self.cmd = cmd_ch
        self.clk = clk_ch
        self.half_period = max(2, samplerate // 400000)
        builder.set_idle(clk_ch, 0)
        builder.set_idle(cmd_ch, 1)

    def _crc7(self, bits_list):
        """Compute CRC7 for SD command."""
        data = 0
        for b in bits_list:
            di = b ^ ((data >> 6) & 1)
            data = (data & 0x07) | ((data & 0x38) << 1) | ((di ^ ((data >> 2) & 1)) << 3)
            data = (data & 0x78) | ((data & 0x03) << 1) | di
        return data & 0x7F

    def _send_bit(self, bit):
        """Send one bit on CMD with CLK rising edge sampling."""
        period = self.half_period * 2
        # Set CLK low for full period, overlay CMD
        self.builder.set_level(self.clk, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.cmd, bit, period)
        self.builder.pos -= period
        # CLK high then low
        self.builder.set_level(self.clk, 1, self.half_period)
        self.builder.set_level(self.clk, 0, self.half_period)

    def send_command(self, cmd, arg=0x00000000):
        """Send an SDIO command token (48 bits).
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

        assert len(bits) == 48, f"Command token must be 48 bits, got {len(bits)}"

        # CMD idle high before command
        self.builder.set_level(self.cmd, 1, self.half_period * 4)
        for b in bits:
            self._send_bit(b)
        # CMD idle high after command
        self.builder.set_level(self.cmd, 1, self.half_period * 4)

