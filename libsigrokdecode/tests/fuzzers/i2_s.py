import math
from .base import *

class I2SGenerator:
    """3 channels: SCLK(ch0), LRCK(ch1), SD(ch2)"""
    def __init__(self, builder, sclk_ch, lrck_ch, sd_ch, sample_rate=8000, bits_per_sample=16):
        self.builder = builder
        self.sclk = sclk_ch
        self.lrck = lrck_ch
        self.sd = sd_ch
        self.bits_per_sample = bits_per_sample
        self.half_period = max(1, int(builder.samplerate / (sample_rate * bits_per_sample * 2)))
        self.builder.set_idle(sclk_ch, 0)
        self.builder.set_idle(lrck_ch, 0)
        self.builder.set_idle(sd_ch, 0)

    def _send_channel_data(self, data, num_bits, lrck_level):
        """
        Send data MSB first with LRCK held at lrck_level.
        Data changes on falling SCLK edge, sampled on rising edge.
        """
        for i in range(num_bits - 1, -1, -1):
            bit = (data >> i) & 1
            period = self.half_period * 2
            # Set SCLK low for full period, then overlay SD and LRCK
            self.builder.set_level(self.sclk, 0, period)
            self.builder.pos -= period
            self.builder.set_level(self.sd, bit, period)
            self.builder.pos -= period
            self.builder.set_level(self.lrck, lrck_level, period)
            self.builder.pos -= period
            # Rising edge then falling edge
            self.builder.set_level(self.sclk, 1, self.half_period)
            self.builder.set_level(self.sclk, 0, self.half_period)

    def send_frame(self, left_data=0x1234, right_data=0x5678):
        """Send one complete frame: left channel + right channel.
        I2S standard: WS=1 for left channel, WS=0 for right channel.
        Add idle period with WS=0 so decoder can detect first WS edge."""
        # Idle period: SCLK low, WS=0, SD=0 for a few bit periods
        # This ensures the decoder sees the WS 0→1 transition at frame start
        idle_samples = self.half_period * 4
        self.builder.set_level(self.sclk, 0, idle_samples)
        self.builder.pos -= idle_samples
        self.builder.set_level(self.lrck, 0, idle_samples)
        self.builder.pos -= idle_samples
        self.builder.set_level(self.sd, 0, idle_samples)
        # Left channel: LRCK high (WS=1 for left in standard I2S)
        self._send_channel_data(left_data, self.bits_per_sample, 1)
        # Right channel: LRCK low (WS=0 for right)
        self._send_channel_data(right_data, self.bits_per_sample, 0)

