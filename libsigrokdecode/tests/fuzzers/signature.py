import math
from .base import *

class SignatureGenerator:
    """Signature/statistics decoder generator.
    4 channels: start(0), stop(1), clk(2), data(3).
    The decoder waits for CLK falling edge (default), checks START/STOP edges.
    Flow: START rising -> gate opens -> CLK edges shift in data -> STOP rising -> gate closes -> signature output.

    Key insight: the decoder reads channel values AT the CLK edge position.
    Using overlay mode (duration=0) only sets one sample, but CLK edges are at
    different positions. Must use write_channels() to set all channels simultaneously
    with proper durations so START/STOP/DATA have correct values at each CLK edge.
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        start = self.channels_map.get("start", 0)
        stop = self.channels_map.get("stop", 1)
        clk = self.channels_map.get("clk", 2)
        data = self.channels_map.get("data", 3)

        half_period = max(2, self.samplerate // 100000)  # 10us half-period

        # Phase 1: Idle (all LOW) - decoder waits for first CLK falling edge
        self.builder.write_channels({start: 0, stop: 0, clk: 0, data: 0}, half_period * 4)

        # Phase 2: Data section - START=HIGH, STOP=LOW, CLK toggling, DATA changing
        # At each CLK falling edge, the decoder reads START/STOP/DATA values.
        # START must be 1 at the first CLK falling edge so the decoder detects
        # start != prev_start (0 -> 1) and opens the gate.
        data_bits = [1, 0, 1, 1, 0, 0, 1, 0]
        for bit in data_bits:
            # CLK high phase
            self.builder.write_channels({start: 1, stop: 0, clk: 1, data: bit}, half_period)
            # CLK low phase (falling edge at start of this phase - decoder samples here)
            self.builder.write_channels({start: 1, stop: 0, clk: 0, data: bit}, half_period)

        # Phase 3: STOP section - START=LOW, STOP=HIGH, one more CLK edge
        # At this CLK falling edge, the decoder sees stop != prev_stop (0 -> 1)
        # and gate_is_open=True, so it closes the gate and outputs the signature.
        self.builder.write_channels({start: 0, stop: 1, clk: 1, data: 0}, half_period)
        self.builder.write_channels({start: 0, stop: 1, clk: 0, data: 0}, half_period)

        # Phase 4: Return to idle
        self.builder.write_channels({start: 0, stop: 0, clk: 0, data: 0}, half_period * 4)
