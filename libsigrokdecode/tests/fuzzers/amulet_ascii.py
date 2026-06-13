import math
from .base import *
from .uart import UARTGenerator

class AmuletAsciiGenerator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        tx = self.channels_map.get("tx", 0)
        rx = self.channels_map.get("rx", 1)
        
        # 115200 is typical UART speed, but it uses 9600 by default in options usually?
        # Amulet default might be 115200.
        gen = UARTGenerator(self.builder, tx, baudrate=115200, samplerate=self.samplerate)
        
        # 0xF0 = ACK
        gen.write_byte(0xF0)
        
        gen_rx = UARTGenerator(self.builder, rx, baudrate=115200, samplerate=self.samplerate)
        gen_rx.write_byte(0xF0)
