import math
from .base import *

class LFASTGenerator:
    """LFAST (Low Frequency Addressable Serial Transceiver) generator.
    1 channel: DATA(ch0).
    Edge-based NRZ encoding: both Python and C decoders detect edges and measure
    bit_len from intervals between edges. Rising edge = bit 0, falling edge = bit 1.
    The signal level during an interval = bit value. Edges separate groups of
    same-value bits. The decoder counts bits by dividing interval duration by bit_len.
    Sync pattern: 0xA84B transmitted MSB first.
    Protocol: sync + 8-bit header (3 payload_size + 4 channel_type + 1 CTS) +
    variable payload + sleep bit via timeout."""
    def __init__(self, builder, channel=0, samplerate=1000000):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate
        self.bit_width = max(2, samplerate // 100000)  # 100k bps default
        self.current_level = 0  # Start low
        builder.set_idle(channel, 0)

    def _send_bits_edge_encoded(self, bits):
        """Send a sequence of bits using edge-based NRZ encoding.
        Groups consecutive same-value bits. The signal level during each group
        equals the bit value. Transitions between groups create edges that the
        decoder uses to determine bit values and count bits per interval.
        Rising edge (0→1) = bit 0, Falling edge (1→0) = bit 1."""
        if not bits:
            return

        # Group consecutive same bits
        groups = []
        current_bit = bits[0]
        count = 1
        for b in bits[1:]:
            if b == current_bit:
                count += 1
            else:
                groups.append((current_bit, count))
                current_bit = b
                count = 1
        groups.append((current_bit, count))

        # For each group, set signal to bit value level for the group duration,
        # then transition to opposite level to create an edge for the decoder.
        for bit_value, count in groups:
            target_level = bit_value  # 0=low, 1=high
            duration = count * self.bit_width
            self.builder.set_level(self.ch, target_level, duration)
            self.current_level = target_level

        # After all groups, add a terminating edge so the decoder processes
        # the last interval. Transition to the opposite level.
        if groups:
            last_bit = groups[-1][0]
            next_level = 1 - last_bit
            self.builder.set_level(self.ch, next_level, self.bit_width)
            self.current_level = next_level

    def _send_bits_msb_first(self, value, num_bits):
        """Send bits MSB first using edge-based NRZ encoding."""
        bits = [(value >> i) & 1 for i in range(num_bits - 1, -1, -1)]
        self._send_bits_edge_encoded(bits)

    def send_frame(self, payload_size_id=0b001, channel_type=0b0100, cts=0,
                   payload_bytes=None, sleep_bit=0):
        """Send a complete LFAST frame.

        payload_size_id: 3-bit index into payload_byte_sizes [1,4,8,12,16,32,64,36]
        channel_type: 4-bit channel type (4-11 = data channels)
        cts: 1-bit clear-to-send
        payload_bytes: list of byte values (must match payload_size_id length)
        sleep_bit: 0=active, 1=sleep (implemented via timeout gap)
        """
        payload_byte_sizes = [1, 4, 8, 12, 16, 32, 64, 36]
        expected_len = payload_byte_sizes[payload_size_id]

        if payload_bytes is None:
            payload_bytes = [0x00] * expected_len
        # Pad or truncate to expected length
        payload_bytes = (payload_bytes + [0x00] * expected_len)[:expected_len]

        # Idle: hold low for a while so the first transition is clearly detected
        self.builder.set_level(self.ch, 0, self.bit_width * 4)
        self.current_level = 0

        # Sync pattern: 0xA84B = 1010100001001011, MSB first
        sync = 0xA84B
        self._send_bits_msb_first(sync, 16)

        # Header: 3 bits payload_size + 4 bits channel_type + 1 bit CTS, MSB first
        header = (payload_size_id << 5) | (channel_type << 1) | cts
        self._send_bits_msb_first(header, 8)

        # Payload: bytes MSB first
        for byte in payload_bytes:
            self._send_bits_msb_first(byte, 8)

        # Sleep bit: hold current level for 1.4*bit_width (no edge = timeout = sleep)
        # For sleep_bit=0 (active): make a transition to create an edge (no sleep)
        # For sleep_bit=1 (sleep): hold level for longer (timeout triggers sleep)
        if sleep_bit:
            # Hold level for 1.5 * bit_width (no transition = timeout = sleep)
            self.builder.set_level(self.ch, self.current_level, int(1.5 * self.bit_width))
        else:
            # Send a bit with opposite value to create an edge (no sleep)
            self._send_bits_edge_encoded([0 if self.current_level == 1 else 1])

        # Idle gap after frame
        self.builder.set_level(self.ch, 0, self.bit_width * 4)
        self.current_level = 0

