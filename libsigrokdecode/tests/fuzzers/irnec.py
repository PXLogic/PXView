import math
from .base import *

class IrNecGenerator:
    """NEC IR protocol generator (baseband).
    Default polarity is active-low (idle HIGH, burst LOW) to match decoder defaults.
    Class name matches dynamic dispatch: ir_nec_c -> IrNecGenerator."""

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _us(self, us):
        return int(us * self.samplerate / 1e6)

    def _send_bit(self, ch, bit):
        # 562.5us burst (LOW for active-low) then space (HIGH)
        self.builder.set_level(ch, 0, self._us(562.5))
        if bit:
            self.builder.set_level(ch, 1, self._us(1687.5))
        else:
            self.builder.set_level(ch, 1, self._us(562.5))

    def _send_nec_frame(self, ch, address, command):
        # Leader: 9ms burst (LOW) + 4.5ms space (HIGH)
        self.builder.set_level(ch, 0, self._us(9000))
        self.builder.set_level(ch, 1, self._us(4500))
        # 32 bits: address + ~address + command + ~command
        for byte_val in [address, (~address) & 0xFF, command, (~command) & 0xFF]:
            for i in range(7, -1, -1):
                self._send_bit(ch, (byte_val >> i) & 1)
        # Stop bit: 562.5us burst (LOW)
        self.builder.set_level(ch, 0, self._us(562.5))
        # Return to idle (HIGH)
        self.builder.set_level(ch, 1, self._us(5000))

    def _send_repeat_code(self, ch):
        # Repeat: 9ms burst (LOW) + 2.25ms space (HIGH)
        self.builder.set_level(ch, 0, self._us(9000))
        self.builder.set_level(ch, 1, self._us(2250))
        # Stop bit: 562.5us burst (LOW)
        self.builder.set_level(ch, 0, self._us(562.5))
        # Return to idle (HIGH)
        self.builder.set_level(ch, 1, self._us(5000))

    def generate_testdata(self):
        ch = self.channels_map.get("ir", 0)
        # Set idle HIGH (active-low polarity, matching decoder default)
        self.builder.set_idle(ch, 1)
        # Start in idle state
        self.builder.set_level(ch, 1, self._us(5000))
        # Send a valid NEC frame: address=0x04, command=0x08
        self._send_nec_frame(ch, 0x04, 0x08)
        # Send a repeat code
        self._send_repeat_code(ch)
        # Send another valid NEC frame: address=0x00, command=0x00
        self._send_nec_frame(ch, 0x00, 0x00)


# Legacy alias for backward compatibility with generate_testdata.py fallback path
IRNECGenerator = IrNecGenerator

