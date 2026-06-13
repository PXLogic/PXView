import math
from .base import *

class RpmGenerator:
    """RPM measurement generator.
    The decoder waits for falling edges (default), counts num_pulses (default 2)
    consecutive edges within 0.5s, then outputs RPM = 60/t."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        data = self.channels_map.get("data", 0)

        # Generate periodic pulses with falling edges.
        # At 1MHz samplerate, 10ms period = 100Hz signal.
        # With num_pulses=2, RPM = 60 / (2 * period_between_edges)
        # But RPM = 60 / t where t is time between first and Nth edge.
        # For 100Hz: period=10ms, so 2 edges 10ms apart → RPM = 60/0.01 = 6000
        samples_per_pulse = int(10e-3 * self.samplerate)  # 10ms at given samplerate

        # Start low
        self.builder.set_level(data, 0, samples_per_pulse)

        # Generate several pulse cycles (each cycle: high then low = 2 falling edges)
        for _ in range(6):
            self.builder.set_level(data, 1, samples_per_pulse // 2)
            self.builder.set_level(data, 0, samples_per_pulse // 2)
