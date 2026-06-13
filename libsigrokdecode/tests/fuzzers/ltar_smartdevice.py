import math
from .base import *

class LtarSmartdeviceGenerator:
    """1 channel: AFSK signal.
    Generates AFSK waveform that, when decoded through afsk_c,
    produces valid LTAR SmartDevice frames and blocks.
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('afsk', 0)
        self.builder.set_idle(self.ch, 1)

    def _generate_afsk_bit(self, bit_val):
        bit_period = self.samplerate // 1200
        freq = 4000 if bit_val else 2000
        half_cycle = self.samplerate // (2 * freq)
        pos = 0
        level = 1
        while pos < bit_period:
            duration = min(half_cycle, bit_period - pos)
            self.builder.set_level(self.ch, level, duration)
            level = 1 - level
            pos += duration

    def _send_frame(self, byte_val):
        self._generate_afsk_bit(0)  # Start bit
        for i in range(8):
            self._generate_afsk_bit((byte_val >> i) & 1)
        self._generate_afsk_bit(1)  # Stop bit

    def _send_spacer(self, count=3):
        for _ in range(count):
            self._generate_afsk_bit(1)

    def _send_block_end(self):
        for _ in range(15):
            self._generate_afsk_bit(1)

    def generate_testdata(self):
        # Generate a simple block with 3 frames
        frames = [0x02, 0x09, 0x05]
        for i, byte_val in enumerate(frames):
            self._send_frame(byte_val)
            if i < len(frames) - 1:
                self._send_spacer(3)
        self._send_block_end()
