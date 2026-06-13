import math
from .base import *

class CECGenerator:
    """HDMI CEC protocol generator."""
    def __init__(self, builder, channel, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.sr = samplerate
        self.builder.set_idle(channel, 0)

    def _us(self, us):
        return int(us * self.sr / 1e6)

    def _send_start(self):
        # Start bit: high for 3.7ms, low for 0.8ms
        self.builder.set_level(self.channel, 1, self._us(3700))
        self.builder.set_level(self.channel, 0, self._us(800))

    def _send_bit(self, bit):
        if bit:
            # 1 = high 0.6ms + low 0.6ms
            self.builder.set_level(self.channel, 1, self._us(600))
            self.builder.set_level(self.channel, 0, self._us(600))
        else:
            # 0 = high 1.5ms + low 0.6ms
            self.builder.set_level(self.channel, 1, self._us(1500))
            self.builder.set_level(self.channel, 0, self._us(600))

    def send_frame(self, initiator=0x0, follower=0x4):
        # Start bit
        self._send_start()
        # Header: 4-bit initiator + 4-bit follower
        header = (initiator << 4) | follower
        for i in range(7, -1, -1):
            self._send_bit((header >> i) & 1)
        # EOM = 1
        self._send_bit(1)
        # ACK = 1
        self._send_bit(1)

