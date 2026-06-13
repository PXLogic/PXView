import math
from .base import *

class IrIrmpGenerator:
    """IRMP (Infrared Multi Protocol) decoder generator.
    1 channel: IR(ch0).
    The C decoder loads irmp.dll at runtime and feeds samples to it.
    If the library is not available, it silently consumes data.
    This generator produces NEC protocol data (the simplest IR protocol)
    so that if irmp.dll is present, it will decode successfully.
    NEC protocol (active-low, idle HIGH):
    - Leader: 9ms low + 4.5ms high
    - Bit 0: 562.5us low + 562.5us high
    - Bit 1: 562.5us low + 1687.5us high
    - Frame: 8-bit addr + 8-bit addr-inv + 8-bit cmd + 8-bit cmd-inv (MSB first)"""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.channel = channels_map.get('ir', 0)
        self.sr = samplerate
        # Set ALL samples to idle HIGH (active-low) including the pre-offset area.
        # The builder.pos may already be advanced past 0 by generate_testdata.py,
        # so we need to explicitly fill the initial region too.
        saved_pos = self.builder.get_pos()
        self.builder.set_pos(0)
        self.builder.set_idle(self.channel, 1)  # Idle HIGH (active-low)
        self.builder.set_pos(saved_pos)

    def _us(self, us):
        return int(us * self.sr / 1e6)

    def _send_bit(self, bit):
        # 562.5us burst (LOW for active-low) then space (HIGH)
        self.builder.set_level(self.channel, 0, self._us(562.5))
        if bit:
            self.builder.set_level(self.channel, 1, self._us(1687.5))
        else:
            self.builder.set_level(self.channel, 1, self._us(562.5))

    def send_nec(self, address=0x04, command=0x08):
        """Send a NEC IR frame: leader + 32 bits + stop bit."""
        # Leader: 9ms burst (LOW) + 4.5ms space (HIGH)
        self.builder.set_level(self.channel, 0, self._us(9000))
        self.builder.set_level(self.channel, 1, self._us(4500))
        # 32 bits: address + ~address + command + ~command (MSB first)
        for byte_val in [address, (~address) & 0xFF, command, (~command) & 0xFF]:
            for i in range(7, -1, -1):
                self._send_bit((byte_val >> i) & 1)
        # Stop bit: 562.5us burst (LOW)
        self.builder.set_level(self.channel, 0, self._us(562.5))
        # Return to idle HIGH (IRMP needs idle time to finalize detection)
        self.builder.set_level(self.channel, 1, self._us(20000))

    def generate_testdata(self):
        # Send a NEC frame
        self.send_nec(0x04, 0x08)
        
        # Add 50ms idle
        self.builder.set_level(self.channel, 1, self._us(50000))
        
        # Send RC5 frame
        from .irrc5 import IrRc5Generator
        rc5 = IrRc5Generator(self.builder, self.channels_map, self.samplerate)
        rc5._send_rc5_frame(self.channel, address=0x05, command=0x0A, toggle=0)
        self.builder.set_level(self.channel, 1, self._us(50000))
        
        # Send RC6 frame
        from .irrc6 import IRRC6Generator
        rc6 = IRRC6Generator(self.builder, self.channel, self.samplerate)
        rc6.send_rc6(address=0x06, command=0x0B)
        self.builder.set_level(self.channel, 1, self._us(50000))
        
        # Send SIRC frame
        from .irsirc import IrSircGenerator
        sirc = IrSircGenerator(self.builder, self.channels_map, self.samplerate)
        sirc._send_sirc_frame(self.channel, command=0x15, address=0x01)
        self.builder.set_level(self.channel, 1, self._us(50000))
