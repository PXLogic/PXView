import math
from .base import *

class SpiDualQuadGenerator:
    """SPI Dual/Quad protocol generator.
    Channels: CLK(ch0), SIO0(ch1), SIO1(ch2), SIO2(ch3), SIO3(ch4), CS(ch5).
    In standard SPI mode (protocol="spi"), CLK+SIO0+SIO1(+CS) are used.
    SIO0 is MOSI (host->device), SIO1 is MISO (device->host).
    Data is sampled on CLK edges (CPOL=0, CPHA=0 default)."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.clk = channels_map.get('clk', 0)
        self.sio0 = channels_map.get('sio0', 1)
        self.sio1 = channels_map.get('sio1', 2)
        self.sio2 = channels_map.get('sio2', 3)
        self.sio3 = channels_map.get('sio3', 4)
        self.cs = channels_map.get('cs', 5)
        self.half_period = max(2, samplerate // 2000000)
        # Set idle states
        self.builder.set_idle(self.clk, 0)
        self.builder.set_idle(self.sio0, 0)
        self.builder.set_idle(self.sio1, 0)
        self.builder.set_idle(self.sio2, 0)
        self.builder.set_idle(self.sio3, 0)
        if self.cs >= 0:
            self.builder.set_idle(self.cs, 1)
            # Fill initial idle period (positions 0 to current pos) with CS=1 (deasserted)
            # so the C decoder sees a proper CS falling edge when _select() is called
            saved_pos = self.builder.pos
            self.builder.pos = 0
            self.builder.set_level(self.cs, 1, saved_pos)
            self.builder.pos = saved_pos

    def _select(self):
        if self.cs >= 0:
            self.builder.set_level(self.cs, 0, self.half_period)

    def _deselect(self):
        if self.cs >= 0:
            self.builder.set_level(self.cs, 1, self.half_period)

    def _write_bit(self, bit):
        """Write one bit on SIO0 with CLK rising edge sampling (CPOL=0, CPHA=0).
        CS stays asserted (low) throughout."""
        self.builder.set_level(self.sio0, bit, 0)
        self.builder.set_level(self.sio1, 0, 0)
        self.builder.set_level(self.sio2, 0, 0)
        self.builder.set_level(self.sio3, 0, 0)
        # CLK high phase - include CS=0
        levels = {self.clk: 1}
        if self.cs >= 0:
            levels[self.cs] = 0
        self.builder.write_channels(levels, self.half_period)
        # CLK low phase - include CS=0
        levels = {self.clk: 0}
        if self.cs >= 0:
            levels[self.cs] = 0
        self.builder.write_channels(levels, self.half_period)

    def _write_byte(self, val):
        """Write 8 bits MSB first on SIO0."""
        for i in range(7, -1, -1):
            self._write_bit((val >> i) & 1)

    def _read_bit(self, bit_val):
        """Read one bit from SIO1 (MISO) while clocking. SIO0 is 0 (dummy)."""
        self.builder.set_level(self.sio0, 0, 0)
        self.builder.set_level(self.sio1, bit_val, 0)
        self.builder.set_level(self.sio2, 0, 0)
        self.builder.set_level(self.sio3, 0, 0)
        # CLK high phase - include CS=0
        levels = {self.clk: 1}
        if self.cs >= 0:
            levels[self.cs] = 0
        self.builder.write_channels(levels, self.half_period)
        # CLK low phase - include CS=0
        levels = {self.clk: 0}
        if self.cs >= 0:
            levels[self.cs] = 0
        self.builder.write_channels(levels, self.half_period)

    def _read_byte(self, val):
        """Read 8 bits MSB first from SIO1."""
        for i in range(7, -1, -1):
            self._read_bit((val >> i) & 1)

    def generate_testdata(self):
        # Transaction 1: Write + Read in SPI mode
        self._select()
        self._write_byte(0x03)  # Command byte
        self._write_byte(0x00)  # Address high
        self._write_byte(0x10)  # Address mid
        self._write_byte(0x20)  # Address low
        self._read_byte(0xAB)   # Data read
        self._read_byte(0xCD)   # Data read
        self._deselect()

        # Transaction 2: Simple write
        self._select()
        self._write_byte(0x55)  # Data byte
        self._write_byte(0xAA)  # Data byte
        self._deselect()
