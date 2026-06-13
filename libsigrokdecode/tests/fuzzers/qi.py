import math
from .base import *

class QiGenerator:
    """Qi wireless charging ASK generator.
    1 channel: DATA(ch0). Differential bi-phase encoding."""
    def __init__(self, builder, channel=0, bitrate=2000, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.bit_width = int(samplerate / bitrate)
        self.half_bit = self.bit_width // 2
        builder.set_idle(channel, 1)

    def _diff_biphase_bit(self, bit, last_level):
        new_level = 1 - last_level
        self.builder.set_level(self.ch, new_level, self.half_bit)
        if bit:
            mid_level = 1 - new_level
            self.builder.set_level(self.ch, mid_level, self.half_bit)
            return mid_level
        else:
            self.builder.set_level(self.ch, new_level, self.half_bit)
            return new_level

    def send_packet(self, header, data_bytes):
        """Send a Qi packet: preamble [1,1,1,1,0] + header byte (11-bit UART) + data bytes (11-bit UART each) + checksum byte (11-bit UART)."""
        last = 1
        # Preamble: [1,1,1,1,0] triggers DATA state in decoder
        for b in [1, 1, 1, 1, 0]:
            last = self._diff_biphase_bit(b, last)
        # Header byte: 11-bit UART (start=0 + 8 data LSB first + parity + stop=1)
        last = self._send_qi_byte(header, last)
        # Data bytes
        for byte in data_bytes:
            last = self._send_qi_byte(byte, last)
        # Checksum: XOR of header + all data bytes
        chk = header
        for byte in data_bytes:
            chk ^= byte
        last = self._send_qi_byte(chk, last)

    def _send_qi_byte(self, val, last):
        """Send one 11-bit Qi byte: start(0) + 8 data(LSB first) + odd parity + stop(1)."""
        p = 1
        bits = [0]  # start bit
        for i in range(8):
            bit = (val >> i) & 1
            bits.append(bit)
            p ^= bit
        bits.append(p)  # odd parity
        bits.append(1)  # stop bit
        for b in bits:
            last = self._diff_biphase_bit(b, last)
        return last

