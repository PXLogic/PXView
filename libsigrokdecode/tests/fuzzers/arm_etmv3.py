import math
from .base import *
from .uart import UARTGenerator

class ArmEtmv3Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        tx = self.channels_map.get("tx", 0)
        
        gen = UARTGenerator(self.builder, tx, baudrate=115200, samplerate=self.samplerate)
        
        # Sync packet: [0x00, 0x00, 0x00, 0x00, 0x80]
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.write_byte(0x00)
        gen.write_byte(0x80)
