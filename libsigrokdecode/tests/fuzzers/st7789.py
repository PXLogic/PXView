import math
from .base import *

class St7789Generator:
    """ST7789 LCD controller generator.
    4 channels: csx(0), dcx(1), sdo(2), wrx(3).
    CSX falling selects device. DCX high samples SDO bit, DCX low completes bit.
    WRX high=data, WRX low=command."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def generate_testdata(self):
        csx = self.channels_map.get("csx", 0)
        dcx = self.channels_map.get("dcx", 1)
        sdo = self.channels_map.get("sdo", 2)
        wrx = self.channels_map.get("wrx", 3)

        half_period = max(2, self.samplerate // 200000)  # 5us half-period

        # Idle: CSX high, DCX low, SDO low, WRX low
        self.builder.set_level(csx, 1, half_period * 2)
        self.builder.set_level(dcx, 0, 0)   # overlay
        self.builder.set_level(sdo, 0, 0)   # overlay
        self.builder.set_level(wrx, 0, 0)   # overlay

        # CSX falling: start transaction
        self.builder.set_level(csx, 0, half_period)

        # Send command byte 0x29 (DISPON) with WRX=low
        self.builder.set_level(wrx, 0, 0)  # overlay: WRX low = command
        cmd_byte = 0x29
        for i in range(7, -1, -1):  # MSB first
            bit = (cmd_byte >> i) & 1
            # DCX high: sample SDO bit
            self.builder.set_level(sdo, bit, 0)  # overlay
            self.builder.set_level(dcx, 1, half_period)
            # DCX low: complete bit
            self.builder.set_level(dcx, 0, half_period)

        # Send data byte 0x55 with WRX=high
        self.builder.set_level(wrx, 1, 0)  # overlay: WRX high = data
        data_byte = 0x55
        for i in range(7, -1, -1):  # MSB first
            bit = (data_byte >> i) & 1
            # DCX high: sample SDO bit
            self.builder.set_level(sdo, bit, 0)  # overlay
            self.builder.set_level(dcx, 1, half_period)
            # DCX low: complete bit
            self.builder.set_level(dcx, 0, half_period)

        # CSX rising: end transaction
        self.builder.set_level(csx, 1, half_period * 2)
