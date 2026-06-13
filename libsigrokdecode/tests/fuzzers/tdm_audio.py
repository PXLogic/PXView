import math
from .base import *

class TDMAudioGenerator:
    """TDM (Time Division Multiplexed) audio generator.
    3 channels: CLK(ch0), SYNC(ch1), DATA(ch2).
    The decoder waits for a CLK edge (rising by default), then samples DATA.
    Frame sync (SYNC) marks the start of a frame. When SYNC is high, a new frame starts.
    The decoder needs: proper CLK toggling, SYNC pulse at frame start, and data bits."""
    def __init__(self, builder, clk_ch=0, sync_ch=1, data_ch=2, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.sync = sync_ch
        self.data = data_ch
        self.half_period = int(samplerate / 200000)
        builder.set_idle(clk_ch, 0)
        builder.set_idle(sync_ch, 0)
        builder.set_idle(data_ch, 0)

    def send_frame(self, num_channels=8, bits_per_channel=16):
        """Send one TDM frame with multiple channels.
        CLK toggles continuously. SYNC goes high for 1 CLK period at frame start.
        DATA changes on falling CLK edge (setup for rising edge sampling)."""
        hp = self.half_period
        total_bits = num_channels * bits_per_channel
        # Initial idle period
        self.builder.set_level(self.clk, 0, hp * 4)
        self.builder.set_level(self.sync, 0, hp * 4)
        self.builder.set_level(self.data, 0, hp * 4)
        # Send frame
        for bit_idx in range(total_bits):
            # Data changes on falling CLK edge (before rising edge)
            # Channel 0 has some data, others are 0
            ch_idx = bit_idx // bits_per_channel
            bit_in_ch = bit_idx % bits_per_channel
            data_bit = 1 if (ch_idx == 0 and bit_in_ch == bits_per_channel - 1) else 0
            # Set data while CLK is low
            period = hp * 2
            self.builder.set_level(self.clk, 0, period)
            self.builder.pos -= period
            self.builder.set_level(self.data, data_bit, period)
            self.builder.pos -= period
            # SYNC high for first bit of frame (must be high at CLK rising edge)
            if bit_idx == 0:
                self.builder.set_level(self.sync, 1, period + hp)
                self.builder.pos -= period
            elif bit_idx == 1:
                self.builder.set_level(self.sync, 0, period + hp)
                self.builder.pos -= period
            # CLK rising edge (decoder samples DATA here)
            self.builder.set_level(self.clk, 1, hp)
            # CLK falling edge
            self.builder.set_level(self.clk, 0, hp)

