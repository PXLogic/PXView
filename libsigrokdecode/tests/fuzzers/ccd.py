import math
from .base import *

class CCDGenerator:
    """CCD (Chrysler Collision Detection) Data Bus generator.
    1 channel: bus(ch0). UART at 7812.5 bps, idle high.
    Messages separated by idle periods > 10 bit widths of high."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.bit_width = max(1, int(samplerate / 7812.5))
        builder.set_idle(channel, 1)

    def _send_byte(self, val):
        """Send one UART byte: start(0) + 8 data(LSB first) + stop(1)."""
        # Start bit
        self.builder.set_level(self.ch, 0, self.bit_width)
        # Data bits LSB first
        for i in range(8):
            self.builder.set_level(self.ch, (val >> i) & 1, self.bit_width)
        # Stop bit
        self.builder.set_level(self.ch, 1, self.bit_width)

    def send_message(self, bytes_list):
        """Send a CCD message: idle gap + bytes + idle gap.
        Checksum (sum mod 256) is appended automatically."""
        # Idle gap (>10 bit widths of high to trigger IDLE->BUSY transition)
        self.builder.set_level(self.ch, 1, self.bit_width * 12)
        # Send bytes
        for b in bytes_list:
            self._send_byte(b)
        # Append checksum
        chk = sum(bytes_list) % 256
        self._send_byte(chk)
        # Idle gap after message (>10 bit widths for IDLE detection)
        self.builder.set_level(self.ch, 1, self.bit_width * 12)

