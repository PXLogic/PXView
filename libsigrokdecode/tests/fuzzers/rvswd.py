import math
from .base import *

class RVSWDGenerator:
    """RVSWD (RISC-V Serial Wire Debug) generator.
    2 channels: CLK(ch0), DIO(ch1).
    START: CLK high + DIO falling.
    STOP: CLK high + DIO rising (Python requires matched==(False,False,True),
    meaning ONLY the STOP condition matches, not CLK rising/falling simultaneously).
    Bits sampled on CLK rising, terminated on CLK falling.
    Short packet: 52 bits. Long packet: 84 bits."""
    def __init__(self, builder, clk_ch=0, dio_ch=1, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.dio = dio_ch
        self.half_period = max(2, samplerate // 200000)
        self._last_dio = 0
        builder.set_idle(clk_ch, 0)
        builder.set_idle(dio_ch, 0)

    def _start_condition(self):
        """START: CLK high, DIO falling edge.
        First set both high, then DIO falls while CLK stays high.
        Then bring CLK low so DIO can change safely for the first bit."""
        # Both idle low initially, bring both high first
        self.builder.set_level(self.clk, 1, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.dio, 1, self.half_period)
        # Now DIO falls while CLK stays high (START condition detected by decoder)
        self.builder.set_level(self.clk, 1, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.dio, 0, self.half_period)
        # Bring CLK low so DIO can change safely for the first bit
        self.builder.set_level(self.clk, 0, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.dio, 0, self.half_period)

    def _stop_condition(self):
        """STOP: CLK high, DIO rising edge.
        The Python decoder requires matched==(False,False,True), meaning ONLY
        the STOP condition (CLK high + DIO rising) matches, with no simultaneous
        CLK rising edge. So CLK must already be stable high when DIO rises.

        Strategy: the last bit's CLK rising edge is used, but CLK stays high
        (doesn't fall). Then DIO can rise while CLK is already stable high.
        The Python decoder will see the CLK rising edge as a bit sample, but
        since CLK never falls, the bit is never terminated and not added to
        self.bits. Then when DIO rises (STOP), process_packet() is called with
        the correct number of terminated bits."""
        # CLK is currently low after the last bit's falling edge.
        # Set DIO low while CLK is still low (safe to change)
        self.builder.set_level(self.dio, 0, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.clk, 0, self.half_period)
        # Now raise CLK - decoder sees CLK rising edge and pushes DIO value,
        # but this bit won't be terminated (CLK stays high), so it won't be
        # added to self.bits
        self.builder.set_level(self.clk, 1, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.dio, 0, self.half_period)
        # CLK is now stable high. DIO rises while CLK is high = STOP condition
        self.builder.set_level(self.clk, 1, self.half_period)
        self.builder.pos -= self.half_period
        self.builder.set_level(self.dio, 1, self.half_period)

    def _send_bit(self, bit):
        """Send one bit: set DIO while CLK is low, CLK rises (sample), CLK falls (terminate).
        Must ensure DIO doesn't change while CLK is high (would trigger START/STOP)."""
        self._last_dio = bit
        # CLK is currently low. Set DIO while CLK is low (safe to change)
        period = self.half_period * 2
        self.builder.set_level(self.dio, bit, period)
        self.builder.pos -= period
        # CLK low for first half (already low, reinforce)
        self.builder.set_level(self.clk, 0, self.half_period)
        # CLK rises - decoder samples DIO here
        self.builder.set_level(self.clk, 1, self.half_period)
        # CLK falls - decoder terminates bit here
        self.builder.set_level(self.clk, 0, self.half_period)

    def send_short_packet(self, addr_host=0x01, operation=0, data_target=0x00000001):
        """Send a short (52-bit) RVSWD packet.
        Layout: 7 addr_host + 1 op + 1 parity_host + 5 skip + 32 data_target + 1 parity_target + 5 skip = 52 bits."""
        # Build bit list
        bits = []
        # 7-bit address host (MSB first)
        for i in range(6, -1, -1):
            bits.append((addr_host >> i) & 1)
        # 1-bit operation
        bits.append(operation & 1)
        # 1-bit parity (odd parity of addr_host + operation)
        parity_host = 0
        for b in bits:
            parity_host ^= b
        bits.append(parity_host)
        # 5 skip bits (don't care, use 0)
        bits.extend([0] * 5)
        # 32-bit data target (MSB first)
        for i in range(31, -1, -1):
            bits.append((data_target >> i) & 1)
        # 1-bit parity target (odd parity of data_target)
        parity_target = 0
        for i in range(31, -1, -1):
            parity_target ^= (data_target >> i) & 1
        bits.append(parity_target)
        # 5 skip bits
        bits.extend([0] * 5)

        assert len(bits) == 52, f"Short packet must be 52 bits, got {len(bits)}"

        self._start_condition()
        for b in bits:
            self._send_bit(b)
        self._stop_condition()

