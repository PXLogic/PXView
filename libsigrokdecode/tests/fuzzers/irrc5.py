import math
from .base import *

class IrRc5Generator:
    """RC5 IR protocol generator (baseband Manchester).
    Default polarity is active-low (idle HIGH, burst LOW) to match decoder defaults.
    Class name matches dynamic dispatch: ir_rc5_c -> IrRc5Generator."""

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _us(self, us):
        return int(us * self.samplerate / 1e6)

    def _write_manchester(self, ch, bit):
        # RC5 Manchester (active-low): 1 = high-then-low, 0 = low-then-high
        # In active-low: HIGH = idle/no-signal, LOW = signal
        hp = self._us(889)
        if bit:
            self.builder.set_level(ch, 1, hp)
            self.builder.set_level(ch, 0, hp)
        else:
            self.builder.set_level(ch, 0, hp)
            self.builder.set_level(ch, 1, hp)

    def _send_rc5_frame(self, ch, address=0x00, command=0x01, toggle=0):
        # 2 start bits (1,1), toggle bit, 5 address bits, 6 command bits
        bits = [1, 1, toggle]
        for i in range(4, -1, -1):
            bits.append((address >> i) & 1)
        for i in range(5, -1, -1):
            bits.append((command >> i) & 1)
        for bit in bits:
            self._write_manchester(ch, bit)

    def generate_testdata(self):
        ch = self.channels_map.get('ir', 0)
        # Set idle HIGH (active-low polarity, matching decoder default)
        self.builder.set_idle(ch, 1)
        # Start in idle state (HIGH) so the decoder sees proper idle before data
        self.builder.set_level(ch, 1, self._us(5000))
        # Send an RC5 frame: start bits(1,1) + toggle(0) + address(0x00) + command(0x01)
        self._send_rc5_frame(ch, 0x00, 0x01, 0)
        # Return to idle
        self.builder.set_level(ch, 1, self._us(5000))


# Legacy alias for backward compatibility with generate_testdata.py fallback path
IRRC5Generator = IrRc5Generator
