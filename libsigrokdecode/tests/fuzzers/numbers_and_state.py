import math
from .base import *


class NumbersAndStateGenerator:
    """Numbers and State decoder protocol generator.

    0 required + 17 optional channels: CLK(ch0), bit0(ch1), bit1(ch2), ..., bit15(ch16).
    The decoder reads bit patterns as numbers/enums. With a CLK channel,
    it samples data on CLK rising edge (default). Without CLK, it triggers
    on any data change.

    We generate CLK + 8 data bit channels (bit0-bit7) and clock through
    several different values to produce annotations.
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.clk = channels_map.get('clk', 0)
        self.half_period = max(2, samplerate // 200000)

    def _set_data_bits(self, value, num_bits=8):
        """Set data bit channels (bit0=ch1, bit1=ch2, ..., bit7=ch8)."""
        for i in range(num_bits):
            ch = self.channels_map.get(f'bit{i}', i + 1)
            bit = (value >> i) & 1
            self.builder.set_level(ch, bit, 0)

    def _clock_pulse(self):
        """Generate one CLK pulse (rising then falling)."""
        hp = self.half_period
        self.builder.set_level(self.clk, 1, hp)
        self.builder.set_level(self.clk, 0, hp)

    def _send_value(self, value, num_bits=8):
        """Set data bits and clock once."""
        self._set_data_bits(value, num_bits)
        self._clock_pulse()

    def generate_testdata(self):
        hp = self.half_period

        # Set idle states
        self.builder.set_idle(self.clk, 0)
        for i in range(8):
            ch = self.channels_map.get(f'bit{i}', i + 1)
            self.builder.set_idle(ch, 0)

        # Initial idle: CLK low, all data bits low
        channel_levels = {self.clk: 0}
        for i in range(8):
            ch = self.channels_map.get(f'bit{i}', i + 1)
            channel_levels[ch] = 0
        self.builder.write_channels(channel_levels, hp * 10)

        # Send several values with clock
        values = [0x55, 0xAA, 0xFF, 0x00, 0x12, 0x34, 0xDE, 0xAD]
        for val in values:
            self._send_value(val)

        # Final idle
        self.builder.write_channels(channel_levels, hp * 10)
