import math
from .base import *

class AudGenerator:
    """AUD (Advanced User Debugger) protocol generator.
    6 channels: audck(0), naudsync(1), audata3(2), audata2(3), audata1(4), audata0(5).
    The decoder samples on AUDCK rising edge.
    nAUDSYNC high = sync/command nibble, nAUDSYNC low = data nibble.
    Command nibbles: 0x08=1 word, 0x09=2 words, 0x0A=4 words, 0x0B=8 words.
    After command, data nibbles shift in to build the address (LSB-first).
    Annotation triggers when ncnt == nmax and sync goes high again."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _set_nibble(self, nib_val):
        """Set the 4 data lines to the given nibble value."""
        audata3 = self.channels_map.get("audata3", 2)
        audata2 = self.channels_map.get("audata2", 3)
        audata1 = self.channels_map.get("audata1", 4)
        audata0 = self.channels_map.get("audata0", 5)
        self.builder.set_level(audata3, (nib_val >> 3) & 1, 0)
        self.builder.set_level(audata2, (nib_val >> 2) & 1, 0)
        self.builder.set_level(audata1, (nib_val >> 1) & 1, 0)
        self.builder.set_level(audata0, nib_val & 1, 0)

    def _clock_pulse(self, high_dur, low_dur):
        """One AUDCK cycle: high then low."""
        audck = self.channels_map.get("audck", 0)
        self.builder.set_level(audck, 1, high_dur)
        self.builder.set_level(audck, 0, low_dur)

    def _send_nibble(self, nib_val, sync_level, half_period):
        """Send a nibble with the specified nAUDSYNC level, clocked by AUDCK."""
        naudsync = self.channels_map.get("naudsync", 1)
        # Set nAUDSYNC
        self.builder.set_level(naudsync, sync_level, 0)  # overlay
        # Set data lines
        self._set_nibble(nib_val)
        # AUDCK rising edge (decoder samples here)
        self.builder.set_level(self.channels_map.get("audck", 0), 1, half_period)
        # AUDCK falling edge
        self.builder.set_level(self.channels_map.get("audck", 0), 0, half_period)

    def generate_testdata(self):
        audck = self.channels_map.get("audck", 0)
        naudsync = self.channels_map.get("naudsync", 1)

        half_period = max(2, self.samplerate // 200000)  # 5us half-period

        # Idle: AUDCK low, nAUDSYNC high, data lines low
        self.builder.set_level(audck, 0, half_period * 2)
        self.builder.set_level(naudsync, 1, 0)  # overlay
        self._set_nibble(0)

        # Send command nibble 0x0A (4 words) with nAUDSYNC high
        self._send_nibble(0x0A, 1, half_period)

        # Send 4 data nibbles with nAUDSYNC low (address LSB-first)
        address = 0x12345678
        for i in range(4):
            nib = (address >> (i * 4)) & 0xF
            self._send_nibble(nib, 0, half_period)

        # Send another sync high to trigger annotation (ncnt == nmax)
        self._send_nibble(0x08, 1, half_period)
