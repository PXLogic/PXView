from .base import *

class IRRC6Generator:
    """
    Philips RC6 Mode 0 generator.

    Baseband output:
        active-low
        idle  = HIGH (1)
        burst = LOW  (0)

    Frame format:
        Leader
        Start bit (1)
        Mode bits (000)
        Toggle bit (double width)
        Address (8 bits)
        Command (8 bits)

    Timing:
        T  = 444 us
        Bit = 2T = 889 us
        Leader = 6T burst + 2T space
    """

    RC6_T_US = 444.0

    def __init__(self, builder, channel, samplerate=1000000):
        self.builder = builder
        self.channel = channel
        self.sr = samplerate

        self.half_period = round(self.RC6_T_US * samplerate / 1e6)

        # active-low idle
        self.builder.set_idle(channel, 1)

    def _us(self, us):
        return round(us * self.sr / 1e6)

    def _write_manchester(self, bit):
        # RC6 Manchester: 1 = burst then space
        if bit:
            self.builder.set_level(self.channel, 0, self.half_period)
            self.builder.set_level(self.channel, 1, self.half_period)
        else:
            self.builder.set_level(self.channel, 1, self.half_period)
            self.builder.set_level(self.channel, 0, self.half_period)

    def _write_toggle(self, bit):
        double_half = self._us(889)
        half = self.half_period

        if bit:
            self.builder.set_level(self.channel, 0, double_half)
            self.builder.set_level(self.channel, 1, double_half)
        else:
            # Cater to irmp.c's toggle logic:
            # 1. Start with space to prevent merging with previous mode bit 0's burst.
            # 2. Burst must be > 800us to trigger irmp's toggle logic.
            # 3. End with space > 600us so that when combined with the next bit's 
            #    space, the total pause is > 1066us. This forces last_value = 0.
            self.builder.set_level(self.channel, 1, half)
            self.builder.set_level(self.channel, 0, double_half)
            self.builder.set_level(self.channel, 1, double_half)

    def send_rc6(self,
                 address=0x00,
                 command=0x01,
                 toggle=0):
        """
        Send RC6 Mode 0 frame.
        """

        #
        # Leader
        #
        # 6T burst
        # 2T space
        #
        self.builder.set_level(
            self.channel,
            0,
            self._us(2666)
        )

        self.builder.set_level(
            self.channel,
            1,
            self._us(889)
        )

        #
        # Start bit = 1
        #
        self._write_manchester(1)

        #
        # Mode bits = 000
        #
        self._write_manchester(0)
        self._write_manchester(0)
        self._write_manchester(0)

        #
        # Toggle bit
        #
        self._write_toggle(toggle)

        #
        # Address (8-bit, MSB first)
        #
        for i in range(7, -1, -1):
            self._write_manchester(
                (address >> i) & 1
            )

        #
        # Command (8-bit, MSB first)
        #
        for i in range(7, -1, -1):
            self._write_manchester(
                (command >> i) & 1
            )

        # Return to idle
        self.builder.set_level(
            self.channel,
            1,
            self._us(3000)
        )
        
        # Dummy pulse to trigger got_light before timeout
        self.builder.set_level(
            self.channel,
            0,
            self._us(100)
        )
        self.builder.set_level(
            self.channel,
            1,
            self._us(3000)
        )