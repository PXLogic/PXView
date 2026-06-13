import math
from .base import *

class RinnaiControlPanelGenerator:
    """Rinnai control panel pulse length encoding generator.
    1 channel: DATA(ch0).
    Protocol: pulse-length encoding with SYMBOL_DURATION_US=600us.
    - Short A (15-35% of 600us = 90-210us) + Long B (65-85% = 390-510us) = bit 1
    - Long A (390-510us) + Short B (90-210us) = bit 0
    - Reset: B phase 600-1200us
    State machine: INITIAL->wait low, IDLE->wait rising, PRE->wait falling (check reset),
    SYMBOL->wait rising then falling (check bit values).

    The C decoder measures:
    - timeA = rise - fall (LOW duration before rising edge)
    - timeB = fall - rise (HIGH duration after rising edge)
    So for each bit: set signal LOW for timeA, then HIGH for timeB.
    The falling edge at end of timeB triggers the bit check."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('data', 0)

    def generate_testdata(self):
        ch = self.ch
        sr = self.samplerate
        def us(v): return max(1, int(v * sr / 1e6))

        symbol_us = 600
        short_us = int(symbol_us * 0.25)   # 150us (within 15-35% = 90-210us)
        long_us = int(symbol_us * 0.75)     # 450us (within 65-85% = 390-510us)
        reset_us = int(symbol_us * 1.5)     # 900us (within 100-200% = 600-1200us)

        # Start high (idle)
        self.builder.set_level(ch, 1, us(1000))

        # INITIAL state: wait for low edge
        self.builder.set_level(ch, 0, us(100))
        # Now in IDLE state: wait for rising edge
        self.builder.set_level(ch, 1, us(100))
        # Now in PRE state: wait for falling edge, measure high period
        # Continue high for reset duration, then falling edge
        self.builder.set_level(ch, 1, us(reset_us))
        # Falling edge: time since rise = 100 + reset_us = 100 + 900 = 1000us
        # But PRE measures from the LAST rising edge. The rising edge was at
        # the start of the us(100) high period. Then we continued high for
        # us(reset_us). So total high time = 100 + 900 = 1000us.
        # 1000us is within RESET range (600-1200us). Reset detected!
        # After reset detection, s->fall = current sample, state = SYMBOL
        self.builder.set_level(ch, 0, us(100))

        # Now in SYMBOL state: wait for rising edge
        # Signal is currently LOW. We need to go:
        # LOW for timeA, then HIGH for timeB, then falling edge triggers check.

        # Bit 1: Short A (low) + Long B (high)
        # Signal is already low from the previous set_level(ch, 0, us(100))
        # We need to keep it low for short_us total from the last fall
        # But the fall was at the end of the 100us low period.
        # We've already been low for 100us. We need timeA = short_us total.
        # So we need short_us - 100 more us of low, but that's only 50us.
        # Actually, the decoder sets s->fall at the falling edge, then waits
        # for rising edge. timeA = rise - fall. So we need to be low for
        # exactly short_us from the last falling edge.
        # The last set_level(ch, 0, us(100)) already put us low for 100us.
        # We need total low time = short_us = 150us.
        # So we need 150 - 100 = 50us more of low.
        # But wait - after the reset detection, the decoder sets s->fall = di_samplenum(di)
        # at the falling edge. Then SYMBOL waits for rising edge.
        # The falling edge happened at the end of the 100us low period.
        # We're currently at the position after that 100us low period.
        # We need to stay low for (short_us - 100) more, then go high.
        # Actually, let me reconsider. The 100us low period after the reset
        # detection - the decoder's s->fall is set to the sample at the
        # falling edge. The signal is low for 100us. Then we need to go
        # high. timeA = rise - fall = 100us (if we go high immediately after).
        # 100us is within short range (90-210us). That works for bit 1!

        # Actually, let me simplify: after reset, signal is LOW.
        # For bit 1: timeA=short, timeB=long
        # Go high after short_us of low time (from the fall edge after reset)
        # But we already used 100us of low. So remaining low = short_us - 100
        # For short_us = 150: remaining = 50us
        # For long_us = 450: high for 450us

        # Hmm, this is getting complicated with the 100us gap. Let me restart
        # the approach: after reset detection, immediately start the first symbol
        # without any gap.

        # Let me redo the sequence more carefully.
        # After reset: s->fall is set. SYMBOL waits for rising edge.
        # I need: low for timeA, then high for timeB, then falling edge.

        # The 100us low period after the falling edge is already counted.
        # timeA from the fall = 100us + any additional low time.
        # For bit 1: need timeA in [90, 210]. 100us is already in range!
        # So go high immediately for timeB = long_us.

        # Bit 1: timeA = 100us (short, in range), timeB = 450us (long, in range)
        self.builder.set_level(ch, 1, us(long_us))
        # Falling edge: timeA = 100us (short), timeB = 450us (long) -> bit 1!
        # s->fall = current position

        # Bit 0: timeA = long (390-510us), timeB = short (90-210us)
        # Signal is now LOW. Stay low for long_us.
        self.builder.set_level(ch, 0, us(long_us))
        # Rising edge: timeA = long_us = 450us (in long range 390-510)
        # Now high for short_us
        self.builder.set_level(ch, 1, us(short_us))
        # Falling edge: timeB = short_us = 150us (in short range 90-210)
        # Long A + Short B = bit 0!

        # Bit 1: Short A + Long B
        self.builder.set_level(ch, 0, us(short_us))
        self.builder.set_level(ch, 1, us(long_us))

        # Bit 1: Short A + Long B
        self.builder.set_level(ch, 0, us(short_us))
        self.builder.set_level(ch, 1, us(long_us))

        # Bit 0: Long A + Short B
        self.builder.set_level(ch, 0, us(long_us))
        self.builder.set_level(ch, 1, us(short_us))

        # Bit 0: Long A + Short B
        self.builder.set_level(ch, 0, us(long_us))
        self.builder.set_level(ch, 1, us(short_us))

        # Bit 1: Short A + Long B
        self.builder.set_level(ch, 0, us(short_us))
        self.builder.set_level(ch, 1, us(long_us))

        # End with reset: Long B phase (high for reset duration)
        self.builder.set_level(ch, 0, us(short_us))
        self.builder.set_level(ch, 1, us(reset_us))
        self.builder.set_level(ch, 0, us(100))
