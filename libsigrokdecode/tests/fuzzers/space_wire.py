import math
from .base import *

class SpacewireGenerator:
    """SpaceWire protocol generator.
    2 channels: Data(ch0), Strobe(ch1).
    Data-Strobe encoding: Strobe toggles when Data doesn't toggle.
    The decoder waits for edges on Data or Strobe, then processes bits.
    NULL character = ESC (0b111) + FCT (0b001) control characters.
    After NULL sync, processes control and data characters."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.data = channels_map.get('data', 0)
        self.strobe = channels_map.get('strobe', 1)
        self.half_period = max(2, samplerate // 200000)
        self.builder.set_idle(self.data, 0)
        self.builder.set_idle(self.strobe, 0)
        self._data_level = 0
        self._strobe_level = 0

    def _send_bit(self, bit):
        """Send one bit using Data-Strobe encoding.
        Data carries the bit value. Strobe toggles when Data doesn't toggle."""
        new_data = bit
        if new_data == self._data_level:
            # Data didn't toggle, so Strobe toggles
            new_strobe = 1 - self._strobe_level
        else:
            # Data toggled, Strobe stays the same
            new_strobe = self._strobe_level

        hp = self.half_period
        period = hp * 2
        self.builder.set_level(self.data, new_data, period)
        self.builder.pos -= period
        self.builder.set_level(self.strobe, new_strobe, period)

        self._data_level = new_data
        self._strobe_level = new_strobe

    def _send_control_char(self, char_bits_3):
        """Send a 3-bit control character: 3 data bits + DCF(1) + parity(1).
        DCF=1 for control characters. Parity is odd over data+DCF.
        Total: 5 bits per control character."""
        bits = []
        for i in range(3):
            bits.append((char_bits_3 >> i) & 1)
        # DCF = 1 (control character)
        bits.append(1)
        # Parity: odd parity over all previous bits in this character
        parity = 1  # Start with 1 for odd parity
        for b in bits:
            parity ^= b
        bits.append(parity)

        for b in bits:
            self._send_bit(b)

    def _send_data_char(self, byte_val):
        """Send an 8-bit data character: 8 data bits + DCF(0) + parity(1).
        DCF=0 for data characters. Total: 10 bits per data character."""
        bits = []
        for i in range(8):
            bits.append((byte_val >> i) & 1)
        # DCF = 0 (data character)
        bits.append(0)
        # Parity: odd parity over all previous bits
        parity = 1
        for b in bits:
            parity ^= b
        bits.append(parity)

        for b in bits:
            self._send_bit(b)

    def send_null(self):
        """Send a NULL character (ESC + FCT) to initialize the link.
        The decoder in IDLE state looks for the bit pattern 0b1110100."""
        null_bits = [1, 1, 1, 0, 1, 0, 0]
        for b in null_bits:
            self._send_bit(b)

    def generate_testdata(self):
        # Send NULL to sync the decoder
        self.send_null()
        # After NULL sync, send FCT control character
        self._send_control_char(0b001)  # FCT
        # Send a data character
        self._send_data_char(0x55)
        # Send another NULL to keep link alive
        self.send_null()
        # Send EOP control character
        self._send_control_char(0b101)  # EOP
