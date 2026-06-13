import math
from .base import *

class StepperMotorGenerator:
    """Stepper motor step/dir generator.
    2 channels: step(0), dir(1).
    The decoder waits for step rising edge, reads dir pin.
    Outputs speed and position annotations."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        step = self.channels_map.get("step", 0)
        dir_ch = self.channels_map.get("dir", 1)

        # Step pulse period: 10ms at given samplerate
        samples_per_step = int(10e-3 * self.samplerate)
        pulse_width = max(2, samples_per_step // 4)

        # Start with step low, dir high (forward)
        self.builder.set_level(step, 0, 0)  # overlay
        self.builder.set_level(dir_ch, 1, 0)  # overlay

        # Generate step pulses in forward direction
        for _ in range(5):
            self.builder.set_level(step, 1, pulse_width)
            self.builder.set_level(step, 0, samples_per_step - pulse_width)

        # Change direction (backward)
        self.builder.set_level(dir_ch, 0, 0)  # overlay

        # Generate step pulses in backward direction
        for _ in range(5):
            self.builder.set_level(step, 1, pulse_width)
            self.builder.set_level(step, 0, samples_per_step - pulse_width)
