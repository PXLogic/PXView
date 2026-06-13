import math
from .base import *

class Ds2408Generator:
    """1 channel: 1-Wire bus (owr).
    Generates 1-Wire waveform for DS2408 8-channel addressable switch.
    The DS2408 decoder sits on top of the onewire_network stack and expects
    RESET/PRESENCE, ROM, and DATA packets from the upstream decoders."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('owr', 0)
        self.builder.set_idle(self.ch, 1)  # Idle high (pull-up)

    def _reset(self):
        """Generate reset pulse + presence detect."""
        self.builder.set_level(self.ch, 0, 480)
        self.builder.set_level(self.ch, 1, 15)
        self.builder.set_level(self.ch, 0, 60)
        self.builder.set_level(self.ch, 1, 480 - 15 - 60)

    def _write_bit(self, bit):
        if bit:
            self.builder.set_level(self.ch, 0, 10)
            self.builder.set_level(self.ch, 1, 60 - 10)
        else:
            self.builder.set_level(self.ch, 0, 60)
            self.builder.set_level(self.ch, 1, 10)

    def _read_bit(self, bit):
        self.builder.set_level(self.ch, 0, 1)
        self.builder.set_level(self.ch, bit, 15)
        self.builder.set_level(self.ch, 1, 60 - 1 - 15)

    def _write_byte(self, val):
        for i in range(8):
            self._write_bit((val >> i) & 1)

    def _read_byte(self, val):
        for i in range(8):
            self._read_bit((val >> i) & 1)

    def generate_testdata(self):
        # Transaction 1: Skip ROM + Channel Access Read (0xF5)
        self._reset()
        self._write_byte(0xCC)  # Skip ROM command
        self._write_byte(0xF5)  # Channel Access Read
        # Read 2 PIO sample bytes
        self._read_byte(0x55)   # PIO sample
        self._read_byte(0xAA)   # PIO complement

        # Transaction 2: Skip ROM + Read PIO Registers (0xF0)
        self._reset()
        self._write_byte(0xCC)  # Skip ROM
        self._write_byte(0xF0)  # Read PIO Registers
        self._write_byte(0x00)  # Target address TA1
        self._write_byte(0x00)  # Target address TA2
        self._read_byte(0xFF)   # Data byte
