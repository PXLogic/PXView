import math
from .base import *

class OneSingleWireGenerator:
    """OneSingleWire custom bus protocol generator.
    2 channels: OSW(ch0), Start Pulse(ch1).
    The decoder:
    1. Waits for Start Pulse (ch1) rising edge
    2. Waits for OSW (ch0) falling edge
    3. Then measures period between OSW edges
    4. Period < threshold = bit 1, period >= threshold = bit 0
    5. 9 bits per byte (8 data + 1 parity)
    Default threshold: 8us at 1MHz = 8 samples."""
    def __init__(self, builder, data_ch=0, start_ch=1, samplerate=1000000):
        self.builder = builder
        self.osw = data_ch
        self.start = start_ch
        self.sr = samplerate
        self.threshold_us = 8  # default threshold in us
        builder.set_idle(data_ch, 1)
        builder.set_idle(start_ch, 0)

    def _us(self, us):
        return max(1, int(us * self.sr / 1e6))

    def send_byte(self, val):
        """Send a byte: Start Pulse rising edge, then OSW falling edge,
        then 9 bits (8 data LSB first + 1 parity) via period encoding.
        Short period (< threshold) = 1, long period (>= threshold) = 0."""
        # Initial idle on both channels so edges are detected
        self.builder.set_level(self.start, 0, self._us(50))
        self.builder.set_level(self.osw, 1, self._us(50))
        # Start Pulse: rising edge on ch1
        self.builder.set_level(self.start, 1, self._us(10))
        self.builder.set_level(self.start, 0, self._us(5))
        # OSW: falling edge to start bit detection
        self.builder.set_level(self.osw, 1, self._us(5))
        self.builder.set_level(self.osw, 0, self._us(2))
        # Send 9 bits: 8 data LSB first + 1 parity (even)
        parity = 0
        bits = []
        for i in range(8):
            bit = (val >> i) & 1
            bits.append(bit)
            parity ^= bit
        bits.append(0 if parity == 0 else 1)  # parity bit (even parity means parity bit makes total even)
        # Actually: parity_bit = parity of data bits, decoder checks parity_bit ^ sum == 0
        # So parity bit should make total XOR = 0
        bits[8] = parity  # parity bit = XOR of data bits for even parity
        for b in bits:
            if b:
                # Bit 1: short period (< threshold = 8us)
                # OSW goes high briefly then low
                self.builder.set_level(self.osw, 1, self._us(3))
                self.builder.set_level(self.osw, 0, self._us(3))
            else:
                # Bit 0: long period (>= threshold = 8us)
                self.builder.set_level(self.osw, 1, self._us(10))
                self.builder.set_level(self.osw, 0, self._us(10))

