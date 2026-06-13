import math
from .base import *

class IrSircGenerator:
    """SIRC IR protocol generator (baseband).
    Default polarity is active-low (idle HIGH, burst LOW) to match decoder defaults.
    Class name matches dynamic dispatch: ir_sirc_c -> IrSircGenerator."""

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _us(self, us):
        return int(us * self.samplerate / 1e6)

    def _send_bit(self, ch, bit):
        # Burst is LOW (active-low), space is HIGH
        if bit:
            self.builder.set_level(ch, 0, self._us(1200))
        else:
            self.builder.set_level(ch, 0, self._us(600))
        self.builder.set_level(ch, 1, self._us(600))

    def _send_sirc_frame(self, ch, command=0x15, address=0x01):
        # Start: 2400us burst (LOW) + 600us space (HIGH)
        self.builder.set_level(ch, 0, self._us(2400))
        self.builder.set_level(ch, 1, self._us(600))
        # 12 bits: 7 command + 5 address, LSB first
        bits = []
        for i in range(7):
            bits.append((command >> i) & 1)
        for i in range(5):
            bits.append((address >> i) & 1)
        for bit in bits:
            self._send_bit(ch, bit)

    def generate_testdata(self):
        ch = self.channels_map.get('ir', 0)
        # Set idle HIGH (active-low polarity, matching decoder default)
        self.builder.set_idle(ch, 1)
        # Start in idle state (HIGH) so the decoder sees proper idle before data
        self.builder.set_level(ch, 1, self._us(5000))
        # Send a SIRC 12-bit frame
        self._send_sirc_frame(ch, 0x15, 0x01)
        # Return to idle (HIGH) - the decoder needs a gap after the last bit
        # to detect the end of frame (read_bit timeout on the pause)
        self.builder.set_level(ch, 1, self._us(10000))


# Legacy alias for backward compatibility with generate_testdata.py fallback path
IRSIRCGenerator = IrSircGenerator
