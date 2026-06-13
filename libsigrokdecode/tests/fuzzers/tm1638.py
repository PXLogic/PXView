import math
from .base import *

class Tm1638Generator:
    """TM1638 LED driver generator.
    TM1638 is a stack decoder (input: tmc), so this generator produces
    TMC protocol waveforms that the tmc_c decoder will decode and pass up.
    Uses Wire3 mode: STB falling = START, CLK rising samples DIO (LSB-first),
    after 8 bits CLK falling = ACK, STB rising = STOP."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _send_byte_wire3(self, clk, dio, stb, byte_val, half_period):
        """Send a byte in Wire3 mode (LSB-first)."""
        for i in range(8):
            bit = (byte_val >> i) & 1
            self.builder.set_level(dio, bit, 0)  # overlay
            self.builder.set_level(clk, 1, half_period)
            self.builder.set_level(clk, 0, half_period)
        # ACK: DIO goes low, CLK falling
        self.builder.set_level(dio, 0, 0)  # overlay
        self.builder.set_level(clk, 1, half_period)
        self.builder.set_level(clk, 0, half_period)

    def generate_testdata(self):
        clk = self.channels_map.get("clk", 0)
        dio = self.channels_map.get("dio", 1)
        stb = self.channels_map.get("stb", 2) if "stb" in self.channels_map else -1

        half_period = max(2, self.samplerate // 100000)  # 10us half-period

        # Idle: CLK low, DIO high, STB high
        self.builder.set_level(clk, 0, 0)   # overlay
        self.builder.set_level(dio, 1, 0)   # overlay
        if stb >= 0:
            self.builder.set_level(stb, 1, 0)  # overlay

        self.builder.set_pos(self.builder.get_pos() + half_period * 4)

        if stb >= 0:
            # Wire3 mode: STB falling = START
            self.builder.set_level(stb, 0, half_period)

            # Send command byte: 0x40 (Data command: write, auto-address, normal)
            self._send_byte_wire3(clk, dio, stb, 0x40, half_period)

            # STB rising = STOP
            self.builder.set_level(stb, 1, half_period * 2)

            # Second transaction: Address command 0xC0 (address 0)
            self.builder.set_level(stb, 0, half_period)
            self._send_byte_wire3(clk, dio, stb, 0xC0, half_period)
            # Data byte: 0x7F (all segments on = '8')
            self._send_byte_wire3(clk, dio, stb, 0x7F, half_period)
            self.builder.set_level(stb, 1, half_period * 2)

            # Third transaction: Display command 0x8F (display on, contrast 14/16)
            self.builder.set_level(stb, 0, half_period)
            self._send_byte_wire3(clk, dio, stb, 0x8F, half_period)
            self.builder.set_level(stb, 1, half_period * 2)
        else:
            # Wire2 mode: CLK high + DIO falling = START
            self.builder.set_level(clk, 1, half_period)
            self.builder.set_level(dio, 0, half_period)

            # Send command byte: 0x40 (LSB-first)
            for i in range(8):
                bit = (0x40 >> i) & 1
                self.builder.set_level(dio, bit, 0)  # overlay
                self.builder.set_level(clk, 1, half_period)
                self.builder.set_level(clk, 0, half_period)

            # ACK
            self.builder.set_level(dio, 0, 0)  # overlay
            self.builder.set_level(clk, 1, half_period)
            self.builder.set_level(clk, 0, half_period)

            # STOP: CLK high + DIO rising
            self.builder.set_level(clk, 1, half_period)
            self.builder.set_level(dio, 1, half_period)
