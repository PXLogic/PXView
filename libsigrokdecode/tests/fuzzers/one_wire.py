import math
from .base import *

class OneWireGenerator:
    """1 channel"""
    def __init__(self, builder, channel, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.samplerate = samplerate
        self.builder.set_idle(channel, 1)  # Idle high (pull-up)

    def _reset(self):
        """
        Generate reset pulse + presence detect.
        Master pulls low 480us, releases 480us.
        Slave pulls low starting ~60us after release for ~60us.
        """
        # Master pulls low for 480us
        self.builder.set_level(self.channel, 0, 480)
        # Release (high via pull-up)
        self.builder.set_level(self.channel, 1, 15)
        # Slave presence: pulls low for ~60us starting at ~15us after release
        self.builder.set_level(self.channel, 0, 60)
        # Release back to high
        self.builder.set_level(self.channel, 1, 480 - 15 - 60)

    def _write_bit(self, bit):
        """
        Write a bit:
        Write 1: pull low 1-15us, release for rest of time slot (60-120us total)
        Write 0: pull low 60-120us, release for 1-15us
        """
        if bit:
            # Write 1: short low pulse
            self.builder.set_level(self.channel, 0, 10)
            self.builder.set_level(self.channel, 1, 60 - 10)
        else:
            # Write 0: long low pulse
            self.builder.set_level(self.channel, 0, 60)
            self.builder.set_level(self.channel, 1, 10)

    def _read_bit(self, bit):
        """
        Read a bit (master initiates same as write 1, but sample at 15us).
        We simulate the slave driving the line.
        """
        # Master pulls low briefly
        self.builder.set_level(self.channel, 0, 1)
        # Release, slave drives the bit
        self.builder.set_level(self.channel, bit, 15)
        # Rest of time slot
        self.builder.set_level(self.channel, 1, 60 - 1 - 15)

    def _write_byte(self, val):
        """Write byte LSB first."""
        for i in range(8):
            self._write_bit((val >> i) & 1)

    def _read_byte(self, val):
        """Read byte LSB first (simulate slave response)."""
        result = 0
        for i in range(8):
            bit = (val >> i) & 1
            self._read_bit(bit)
        return result

    def read_rom(self, rom_data=None):
        """
        Generate reset + presence + Read ROM command (0x33) + read 8 bytes.
        """
        if rom_data is None:
            rom_data = [0x28, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00]

        # Reset + presence
        self._reset()

        # Command: 0x33 (Read ROM), LSB first
        self._write_byte(0x33)

        # Read 8 bytes of ROM data
        for b in rom_data:
            self._read_byte(b)

