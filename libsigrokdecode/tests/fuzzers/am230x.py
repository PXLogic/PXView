import math
from .base import *

class AM230xGenerator:
    """AM2302/DHT22 temperature sensor protocol generator.
    1 channel: DATA(ch0). At 1MHz samplerate.
    Timing requirements (in us):
    - START LOW: 750-25000us (master pulls low)
    - START HIGH: 10-10000us (master releases, sensor pulls low after 20-40us)
    - RESPONSE LOW: 50-90us (sensor response low)
    - RESPONSE HIGH: 50-90us (sensor response high)
    - BIT LOW: 45-90us (before each bit)
    - BIT 0 HIGH: 20-35us (short high = 0)
    - BIT 1 HIGH: 65-80us (long high = 1)"""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        builder.set_idle(channel, 1)

    def _us(self, us):
        return max(1, int(us * self.sr / 1000000))

    def send_reading(self, humidity, temp):
        """Send a 40-bit AM230x reading with precise timing."""
        ch = self.ch
        # Initial idle high so the falling edge of START LOW is detected
        self.builder.set_level(ch, 1, self._us(500))
        # START: Master pulls low for 1000us (must be 750-25000us)
        self.builder.set_level(ch, 0, self._us(1000))
        # START HIGH: Master releases, sensor responds after ~30us
        # Must be 10-10000us
        self.builder.set_level(ch, 1, self._us(30))
        # RESPONSE LOW: Sensor pulls low for ~80us (must be 50-90us)
        self.builder.set_level(ch, 0, self._us(80))
        # RESPONSE HIGH: Sensor releases for ~80us (must be 50-90us)
        self.builder.set_level(ch, 1, self._us(80))
        # 40 bits: 16 humidity + 16 temperature + 8 checksum
        hum_int = int(humidity * 10)
        temp_int = int(temp * 10)
        checksum = ((hum_int >> 8) + (hum_int & 0xFF) + (temp_int >> 8) + (temp_int & 0xFF)) & 0xFF
        bits = []
        for i in range(15, -1, -1):
            bits.append((hum_int >> i) & 1)
        for i in range(15, -1, -1):
            bits.append((temp_int >> i) & 1)
        for i in range(7, -1, -1):
            bits.append((checksum >> i) & 1)
        for b in bits:
            # BIT LOW: 50us low before each bit (must be 45-90us)
            self.builder.set_level(ch, 0, self._us(50))
            if b:
                # BIT 1 HIGH: 70us high (must be 65-80us)
                self.builder.set_level(ch, 1, self._us(70))
            else:
                # BIT 0 HIGH: 26us high (must be 20-35us)
                self.builder.set_level(ch, 1, self._us(26))
        # End: pull low briefly then high so 'WAIT FOR END' sees a rising edge
        self.builder.set_level(ch, 0, self._us(10))
        self.builder.set_level(ch, 1, self._us(50))

