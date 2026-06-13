import math
from .base import *
from .uart import UARTGenerator

class BluetoothH4Generator:
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        
    def generate_testdata(self):
        tx = self.channels_map.get("tx", 0)
        
        gen = UARTGenerator(self.builder, tx, baudrate=115200, samplerate=self.samplerate)
        
        # CMD packet: [0x01, 0x01, 0x04, 0x00] (Inquiry CMD, Opcode 0x0401)
        gen.write_byte(0x01)
        gen.write_byte(0x01)
        gen.write_byte(0x04)
        gen.write_byte(0x00)
