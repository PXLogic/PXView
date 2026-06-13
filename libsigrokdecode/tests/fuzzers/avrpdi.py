import math
from .base import *

class AVRPDIGenerator:
    """AVR PDI (Program and Debug Interface) generator.
    2 channels: RESET/PDI_CLK(ch0), PDI_DATA(ch1).
    The decoder samples DATA on CLK rising edge, processes on CLK falling edge.
    It looks for BREAK condition (11+ zero bits) then UART frames.
    UART frame: start(0) + 8 data(LSB first) + parity(even) + stop(1)"""
    def __init__(self, builder, clk_ch=0, data_ch=1, samplerate=1000000):
        self.builder = builder
        self.clk = clk_ch
        self.data = data_ch
        self.half_period = max(2, int(samplerate / 200000))  # 5us per half-period
        builder.set_idle(clk_ch, 0)
        builder.set_idle(data_ch, 1)

    def _clock_bit(self, data_val):
        """One CLK cycle: set DATA while CLK low, CLK rises (sample), CLK falls (process)."""
        period = self.half_period * 2
        # Set CLK low for full period, overlay DATA
        self.builder.set_level(self.clk, 0, period)
        self.builder.pos -= period
        self.builder.set_level(self.data, data_val, period)
        self.builder.pos -= period
        # CLK rising edge (decoder samples DATA here)
        self.builder.set_level(self.clk, 1, self.half_period)
        # CLK falling edge (decoder processes bit here)
        self.builder.set_level(self.clk, 0, self.half_period)

    def _send_uart_byte(self, val):
        """Send a byte as UART frame: start(0) + 8 data(LSB first) + parity(even) + stop(1)."""
        # Start bit
        self._clock_bit(0)
        # 8 data bits LSB first
        p = 0
        for i in range(8):
            bit = (val >> i) & 1
            self._clock_bit(bit)
            p ^= bit
        # Parity bit (even)
        self._clock_bit(p)
        # Stop bit
        self._clock_bit(1)

    def send_break(self):
        """Send BREAK condition (12 zero bits) followed by LDCS instruction.
        The decoder detects BREAK when 11+ consecutive zero bits are seen."""
        # Initial idle: CLK low, DATA high (idle state)
        self.builder.set_level(self.clk, 0, self.half_period * 4)
        self.builder.set_level(self.data, 1, self.half_period * 4)
        # Send 12 zero bits (BREAK condition)
        for _ in range(12):
            self._clock_bit(0)
        # After BREAK, send a valid instruction: LDCS (opcode 0b100 = 4)
        # LDCS format: 1 0 0 reg[3:0] = 0x80 + reg
        # LDCS status register (reg=0): byte = 0b10000000 = 0x80
        self._send_uart_byte(0x80)

