import math
from .base import *

class SAEJ1850VPWGenerator:
    """SAE J1850 VPW (Variable Pulse Width) protocol generator.
    1 channel: DATA(ch0).

    VPW encoding: each symbol is a single pulse whose width encodes the value.
    Pulses alternate between active (LOW, level=0) and inactive (HIGH, level=1).

    The decoder measures time between consecutive edges. After c_wait() returns
    at an edge, c_pin() returns the NEW value (after the transition). So when
    the decoder checks pin == active(0), it means the line just went LOW — i.e.
    the pulse that ended was HIGH (inactive). The decoder's logic accounts for
    this inversion:
      - pin == active(0) => pulse was inactive(HIGH): short->bit1, long->bit0
      - pin != active(1) => pulse was active(LOW):   short->bit0, long->bit1

    Therefore the waveform must be generated so that:
      - SOF: a 200us HIGH pulse (ends with falling edge, pin=0==active => SOF)
      - Bit pulses alternate LOW/HIGH starting with LOW after SOF
      - EOF: a pulse >= 240us followed by a transition edge
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.sr = samplerate
        if isinstance(channels_map, dict):
            self.channel = channels_map.get('data', 0)
        else:
            self.channel = channels_map
        # Idle LOW so the first edge is a rising edge (start of SOF HIGH pulse)
        self.builder.set_idle(self.channel, 0)
        # Fill initial idle period with LOW
        saved_pos = self.builder.pos
        self.builder.pos = 0
        self.builder.set_level(self.channel, 0, saved_pos)
        self.builder.pos = saved_pos

    def _us(self, us):
        return max(1, int(us * self.sr / 1e6))

    def generate_testdata(self):
        """Generate a J1850 VPW frame: SOF + data bytes + EOF.

        Each bit is ONE pulse (not a pair). Pulses alternate between
        active (LOW) and inactive (HIGH). After SOF (which ends with a
        falling edge to LOW), the first data pulse is active (LOW).

        Active (LOW) pulses:  short(64us)->bit0, long(128us)->bit1
        Inactive (HIGH) pulses: short(64us)->bit1, long(128us)->bit0
        """
        data_bytes = [0x01, 0x02, 0x03]

        # SOF: 200us HIGH pulse (inactive).
        # Ends with falling edge => pin=0==active => SOF detected.
        self.builder.set_level(self.channel, 1, self._us(200))

        # After SOF the line is LOW (falling edge ended the SOF pulse).
        # First data pulse is active (LOW).
        current_level = 0  # LOW = active

        for byte_val in data_bytes:
            for i in range(7, -1, -1):
                bit = (byte_val >> i) & 1
                if current_level == 0:  # active (LOW) pulse
                    # long active(128us)->bit1, short active(64us)->bit0
                    duration = 128 if bit == 1 else 64
                else:  # inactive (HIGH) pulse
                    # short inactive(64us)->bit1, long inactive(128us)->bit0
                    duration = 64 if bit == 1 else 128
                self.builder.set_level(self.channel, current_level, self._us(duration))
                current_level = 1 - current_level  # alternate polarity

        # EOF: a pulse >= 240us. Add a short transition after it so the
        # decoder can measure the pulse width (it needs an edge to do so).
        self.builder.set_level(self.channel, current_level, self._us(280))
        # Transition to create the measuring edge for EOF
        self.builder.set_level(self.channel, 1 - current_level, self._us(10))

# Alias for dynamic dispatch naming convention
SaeJ1850VpwGenerator = SAEJ1850VPWGenerator
