import math
from .base import *

class DSIGenerator:
    """DSI (Digital Serial Interface) lighting protocol generator.
    1 channel: DSI(ch0). Biphase encoding, active-high polarity (default).
    Generates a backward frame (9 bits: start + 8 data + stop) that both
    the Python and C decoders can decode.

    Signal pattern: toggle raw signal every halfbit for 17 halfbit periods,
    ending with raw=0 (dsi=1). Then hold raw=0 (dsi=1) for the stop bit
    timeout. The decoder detects 9 bits then a stop condition (phase0=1,
    bit=1) via two consecutive timeouts.

    The 17 toggles produce:
    - 1st edge: triggers IDLE->PHASE1
    - 16 subsequent edges: 8 PHASE1 (append bit) + 8 PHASE0 (set phase0)
    - After last edge: old_dsi=1, state=PHASE0
    - 1st timeout: PHASE0 sets phase0=1
    - 2nd timeout: PHASE1 checks stop (bit=1, phase0=1) -> STOP with 9 bits
    """

    def __init__(self, builder, channels_map, samplerate=1000000):
        self.builder = builder
        self.channels_map = channels_map if isinstance(channels_map, dict) else {'dsi': 0}
        self.samplerate = samplerate
        self.ch = self.channels_map.get('dsi', 0) if isinstance(channels_map, dict) else (channels_map if isinstance(channels_map, int) else 0)
        self.halfbit = max(2, int((samplerate * 0.0016667) / 2.0))

    def generate_testdata(self):
        hb = self.halfbit
        ch = self.ch

        # Fill the initial gap (before builder.pos) with raw=1 (dsi=0)
        # so the decoder's old_dsi=0 matches the signal at sample 0
        saved_pos = self.builder.get_pos()
        if saved_pos > 0:
            self.builder.set_pos(0)
            self.builder.set_level(ch, 1, saved_pos)
            self.builder.set_pos(saved_pos)

        # Set idle to raw=1 (dsi=0)
        self.builder.set_idle(ch, 1)
        self.builder.set_level(ch, 1, hb * 4)  # Idle period: raw=1, dsi=0

        # Toggle raw every halfbit for 17 periods.
        # Starting from raw=1 (idle), first toggle to raw=0 creates the
        # edge that triggers IDLE->PHASE1 in the decoder.
        # Pattern: 0,1,0,1,... (17 toggles, odd count ends with raw=0)
        for i in range(17):
            level = i % 2  # 0,1,0,1,...,0
            self.builder.set_level(ch, level, hb)

        # After 17 toggles: raw=0 (dsi=1), old_dsi=1, state=PHASE0
        # Hold raw=0 (dsi=1) for stop bit timeout (2 consecutive timeouts)
        self.builder.set_level(ch, 0, hb * 4)  # dsi=1 for stop timeout

        # Return to idle (raw=1, dsi=0)
        self.builder.set_level(ch, 1, hb * 4)

    def send_backward_frame(self, value=0x80):
        self.generate_testdata()


# P1 dynamic dispatch alias: dsi_c -> DsiGenerator
DsiGenerator = DSIGenerator
