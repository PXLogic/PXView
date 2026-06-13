import math
from .base import *

class PcfxCtrlrGenerator:
    """PC-FX controller protocol generator.
    3 channels: TRG(ch0), CLK(ch1), DATA(ch2) + optional DIR(ch3).
    Protocol state machine:
    1. FIND START: Wait for TRG falling edge
    2. CHECK RESET: Wait for {TRG low + CLK falling} OR {TRG rising}
       - If TRG low + CLK falling = Reset (triggertype=1)
       - If TRG rising = Normal Trigger (triggertype=0)
    3. START BIT: Wait for CLK falling, latch DATA value
    4. END BIT: Wait for CLK rising, record bit
       - After 32 bits, output annotations and go back to FIND START

    For a normal trigger (not reset), we need:
    - TRG falls (FIND START -> CHECK RESET)
    - TRG rises while CLK is still high (CHECK RESET -> START BIT, triggertype=0)
    - Then 32 CLK cycles: CLK falls (latch DATA), CLK rises (end of bit)

    Internal value is inverted from electrical value.
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.trg = channels_map.get('trigger', 0)
        self.clk = channels_map.get('clk', 1)
        self.data = channels_map.get('data', 2)
        self.dir_ch = channels_map.get('dir', 3)
        self.half_period = max(2, samplerate // 200000)

    def generate_testdata(self):
        trg = self.trg
        clk = self.clk
        data = self.data
        hp = self.half_period

        # Idle states: TRG high, CLK high, DATA high
        self.builder.set_idle(trg, 1)
        self.builder.set_idle(clk, 1)
        self.builder.set_idle(data, 1)
        if self.dir_ch is not None:
            self.builder.set_idle(self.dir_ch, 0)

        # Initial idle period
        self.builder.set_level(trg, 1, hp * 4)
        self.builder.pos -= hp * 4
        self.builder.set_level(clk, 1, hp * 4)
        self.builder.pos -= hp * 4
        self.builder.set_level(data, 1, hp * 4)

        # TRG falls to start (FIND START -> CHECK RESET)
        self.builder.set_level(trg, 0, hp)

        # TRG rises while CLK is still high (CHECK RESET -> START BIT, normal trigger)
        # The decoder waits for {TRG low + CLK falling} OR {TRG rising}
        # We want TRG rising, so CLK must stay high
        self.builder.set_level(trg, 1, hp)

        # Send 32 bits: electrical value (internal is inverted)
        # For a Joypad (internal type=0xF=1111 at bits 28-31), electrical bits 28-31 = 0000
        # Use internal value 0xFFFFFFF0 (Joypad type)
        internal_val = 0xFFFFFFF0
        for i in range(32):
            internal_bit = (internal_val >> i) & 1
            elec_bit = 1 - internal_bit  # internal is inverted from electrical

            # Set DATA before CLK falls
            self.builder.set_level(data, elec_bit, hp)
            self.builder.pos -= hp
            self.builder.set_level(clk, 1, hp)  # CLK high period

            # CLK falls (decoder latches DATA in START_BIT state)
            self.builder.set_level(clk, 0, hp)

            # CLK rises (decoder records bit in END_BIT state)
            self.builder.set_level(clk, 1, hp)

        # TRG high idle after read
        self.builder.set_level(trg, 1, hp * 4)
