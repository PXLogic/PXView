import math
from .base import *

class MorseGenerator:
    """Morse code generator.
    1 channel: DATA(ch0)."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        self.unit = int(samplerate * 0.1)
        builder.set_idle(channel, 0)

    def _dit(self):
        self.builder.set_level(self.ch, 1, self.unit)
        self.builder.set_level(self.ch, 0, self.unit)

    def _dah(self):
        self.builder.set_level(self.ch, 1, self.unit * 3)
        self.builder.set_level(self.ch, 0, self.unit)

    def _letter_gap(self):
        self.builder.set_level(self.ch, 0, self.unit * 2)

    def _word_gap(self):
        self.builder.set_level(self.ch, 0, self.unit * 6)

    def send_sos(self):
        """Send SOS: ... --- ..."""
        self._dit(); self._dit(); self._dit()
        self._letter_gap()
        self._dah(); self._dah(); self._dah()
        self._letter_gap()
        self._dit(); self._dit(); self._dit()

