import math
from .base import *

class SPDIFGenerator:
    """S/PDIF biphase mark code generator.
    The decoder needs 3 distinct pulse widths to establish clock recovery:
    - Short pulses (1 UI) for '1' bits
    - Medium pulses (2 UI) for '0' bits
    - Long pulses (3 UI) for preamble start
    At 6MHz samplerate with ~100kHz bitrate: bit_samples = 60 samples per bit.
    Preambles: W=[2,0,1,0], M=[2,2,1,1], B=[2,1,1,2] (in pulse_type units)"""
    def __init__(self, builder, channel, bit_samples=0):
        self.builder = builder
        self.channel = channel
        # Use larger bit_samples for reliable decoding
        # At 6MHz, S/PDIF at 48kHz stereo = ~3.072MHz bitrate, each bit ~2 samples
        # That's too few. Use a much lower effective bitrate for test data.
        if bit_samples == 0:
            bit_samples = max(30, int(builder.samplerate / 100000))
        self.bit_samples = bit_samples
        self.half_bit = bit_samples // 2
        self.builder.set_idle(channel, 0)

    def _send_bmc_bit(self, bit, current_level):
        # Always transition at start of bit period
        level = 1 - current_level
        if bit:
            # '1': additional transition at mid-period
            self.builder.set_level(self.channel, level, self.half_bit)
            level = 1 - level
            self.builder.set_level(self.channel, level, self.half_bit)
        else:
            # '0': no mid-period transition
            self.builder.set_level(self.channel, level, self.bit_samples)
        return level

    def _send_preamble_b(self, current_level):
        """Preamble B (block start): violates BMC rules to create 3 distinct pulse widths.
        Pattern: 3UI high, 3UI low, 2UI high (or inverted).
        This creates pulse widths of 3UI, 3UI, 2UI which are all different from
        normal BMC pulses (1UI or 2UI)."""
        # Force a transition to create the preamble pattern
        # Preamble B: 11100010 in BMC violation coding
        # We need 3 distinct pulse widths: long(3UI), medium(2UI), short(1UI)
        # Start with transition
        level = 1 - current_level
        # 3UI pulse
        self.builder.set_level(self.channel, level, self.bit_samples * 3)
        level = 1 - level
        # 3UI pulse
        self.builder.set_level(self.channel, level, self.bit_samples * 3)
        level = 1 - level
        # 2UI pulse
        self.builder.set_level(self.channel, level, self.bit_samples * 2)
        return level

    def _send_preamble_m(self, current_level):
        """Preamble M (channel 1): 3UI, 2UI, 1UI, 1UI pattern."""
        level = 1 - current_level
        self.builder.set_level(self.channel, level, self.bit_samples * 3)
        level = 1 - level
        self.builder.set_level(self.channel, level, self.bit_samples * 3)
        level = 1 - level
        self.builder.set_level(self.channel, level, self.bit_samples)
        level = 1 - level
        self.builder.set_level(self.channel, level, self.bit_samples)
        return level

    def _send_preamble_w(self, current_level):
        """Preamble W (channel 2): 3UI, 2UI, 1UI, 1UI pattern (same as M for simplicity)."""
        return self._send_preamble_m(current_level)

    def send_frame(self, subframe_data=0x00000000):
        """Send a complete S/PDIF frame: preamble B + 32 BMC-encoded bits."""
        current_level = 0
        # Preamble B (block start)
        current_level = self._send_preamble_b(current_level)
        # Subframe data: 32 bits BMC encoded
        # Bits: 4 auxiliary + 20 audio + V U C P
        for i in range(31, -1, -1):
            bit = (subframe_data >> i) & 1
            current_level = self._send_bmc_bit(bit, current_level)

    def send_two_subframes(self, ch1_data=0x00000000, ch2_data=0x00000000):
        """Send two subframes (one sample per channel) with proper preambles."""
        current_level = 0
        # Preamble B for channel 1 (block start)
        current_level = self._send_preamble_b(current_level)
        # Channel 1 data: 32 bits
        for i in range(31, -1, -1):
            bit = (ch1_data >> i) & 1
            current_level = self._send_bmc_bit(bit, current_level)
        # Preamble W for channel 2
        current_level = self._send_preamble_w(current_level)
        # Channel 2 data: 32 bits
        for i in range(31, -1, -1):
            bit = (ch2_data >> i) & 1
            current_level = self._send_bmc_bit(bit, current_level)

