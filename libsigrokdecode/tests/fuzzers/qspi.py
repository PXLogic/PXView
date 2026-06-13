import math
from .base import *

class QspiGenerator:
    """QSPI (Quad SPI) protocol generator.
    Channels: CLK(ch0), IO0(ch1), IO1(ch2), IO2(ch3), IO3(ch4), CS(ch5).
    In single-SPI mode, CLK+IO0+IO1(+CS) are used.
    IO0 is MOSI (host->device), IO1 is MISO (device->host).
    Data is sampled on CLK rising edge (CPOL=0, CPHA=0 default)."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.clk = channels_map.get('clk', 0)
        self.io0 = channels_map.get('io0', 1)
        self.io1 = channels_map.get('io1', 2)
        self.io2 = channels_map.get('io2', 3)
        self.io3 = channels_map.get('io3', 4)
        self.cs = channels_map.get('cs', 5)
        self.half_period = max(2, samplerate // 2000000)
        # Set idle states (fills from current pos to end)
        self.builder.set_idle(self.clk, 0)
        self.builder.set_idle(self.io0, 0)
        self.builder.set_idle(self.io1, 0)
        self.builder.set_idle(self.io2, 0)
        self.builder.set_idle(self.io3, 0)
        if self.cs >= 0:
            self.builder.set_idle(self.cs, 1)
            # Fill initial idle period (positions 0 to current pos) with CS=1 (deasserted)
            # so the C decoder sees a proper CS falling edge when _select() is called
            saved_pos = self.builder.pos
            self.builder.pos = 0
            self.builder.set_level(self.cs, 1, saved_pos)
            self.builder.pos = saved_pos

    def _select(self):
        """Assert CS (active-low)."""
        if self.cs >= 0:
            self.builder.set_level(self.cs, 0, self.half_period)

    def _deselect(self):
        """Deassert CS (active-low)."""
        if self.cs >= 0:
            self.builder.set_level(self.cs, 1, self.half_period)

    def _write_bit(self, bit):
        """Write one bit on IO0 with CLK rising edge sampling (CPOL=0, CPHA=0).
        CS stays asserted (low) throughout."""
        # Set IO0 data while CLK is low (overlay mode)
        self.builder.set_level(self.io0, bit, 0)
        self.builder.set_level(self.io1, 0, 0)
        self.builder.set_level(self.io2, 0, 0)
        self.builder.set_level(self.io3, 0, 0)
        # CLK high phase - include CS=0 to keep it asserted
        levels = {self.clk: 1}
        if self.cs >= 0:
            levels[self.cs] = 0
        self.builder.write_channels(levels, self.half_period)
        # CLK low phase - include CS=0 to keep it asserted
        levels = {self.clk: 0}
        if self.cs >= 0:
            levels[self.cs] = 0
        self.builder.write_channels(levels, self.half_period)

    def _write_byte(self, val):
        """Write 8 bits MSB first on IO0."""
        for i in range(7, -1, -1):
            self._write_bit((val >> i) & 1)

    def _read_bit(self, bit_val):
        """Read one bit from IO1 (MISO) while clocking. IO0 is 0 (dummy)."""
        self.builder.set_level(self.io0, 0, 0)
        self.builder.set_level(self.io1, bit_val, 0)
        self.builder.set_level(self.io2, 0, 0)
        self.builder.set_level(self.io3, 0, 0)
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
        """Read 8 bits MSB first from IO1."""
        for i in range(7, -1, -1):
            self._read_bit((val >> i) & 1)

    def generate_testdata(self):
        # Transaction 1: Read Status Register (0x05) + 1 read byte
        # cmd_05_data = {PROCESS_READ_BYTE, MODE_SINGLE}
        self._select()
        self._write_byte(0x05)  # RDSR command
        self._read_byte(0x5A)   # Status register value
        self._deselect()

        # Transaction 2: Write Enable (0x06) - no data after command
        self._select()
        self._write_byte(0x06)  # WREN command
        self._deselect()

        # Transaction 3: Read QE Register (0x35) + 1 read byte
        self._select()
        self._write_byte(0x35)  # RDCR command
        self._read_byte(0x02)   # QE register value
        self._deselect()
