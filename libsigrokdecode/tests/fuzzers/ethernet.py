import zlib
from .base import *


class EthernetGenerator:
    """Ethernet II (IEEE 802.3) frame generator over NRZI + 4B5B encoding chain.

    Generates a raw single-channel NRZI waveform that, when decoded through
    nrzi_c -> 4b5b_c -> ethernet_c, produces a valid Ethernet frame with
    annotations.

    Encoding chain:
      Ethernet frame bytes -> 4B5B symbols (5-bit) -> NRZI bits -> signal levels

    NRZI convention (matches sigrok NRZI decoder):
      data bit 1 = toggle level (creates edge, decoder outputs 1)
      data bit 0 = hold level   (no edge, decoder outputs 0)

    4B5B symbol bit order: MSB first (matches 4B5B decoder accumulation).

    4B5B nibble order: low nibble first, high nibble second
      (matches 4B5B decoder: data_byte = (second << 4) | first).
    """

    # 4B5B data encoding: nibble value -> 5-bit symbol
    DATA_ENCODE = {
        0x0: 0b11110, 0x1: 0b01001, 0x2: 0b10100, 0x3: 0b10101,
        0x4: 0b01010, 0x5: 0b01011, 0x6: 0b01110, 0x7: 0b01111,
        0x8: 0b10010, 0x9: 0b10011, 0xA: 0b10110, 0xB: 0b10111,
        0xC: 0b11010, 0xD: 0b11011, 0xE: 0b11100, 0xF: 0b11101,
    }

    # 4B5B control symbols
    CTRL_J = 0b11000   # Start 1 (SSD first part)
    CTRL_K = 0b10001   # Start 2 (SSD second part)
    CTRL_T = 0b01101   # Terminate (ESD first part)
    CTRL_R = 0b00111   # Reset (ESD second part)
    CTRL_I = 0b11111   # Idle

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.bit_width = max(2, samplerate // 100000)  # 100 kbps
        self.current_level = 0

    def _send_nrzi_bit(self, data_bit):
        """Send one data bit using NRZI encoding.

        Convention: data_bit 1 = toggle (edge), data_bit 0 = hold (no edge).
        This matches the NRZI decoder: edge -> output 1, no edge -> output 0.
        """
        if data_bit == 1:
            self.current_level = 1 - self.current_level
        self.builder.set_level(self.channel, self.current_level, self.bit_width)

    def _send_4b5b_symbol(self, symbol_5bit):
        """Send a 5-bit 4B5B symbol, MSB first."""
        for i in range(4, -1, -1):
            self._send_nrzi_bit((symbol_5bit >> i) & 1)

    def _send_data_byte(self, byte_val):
        """Encode a data byte as two 4B5B data symbols.

        Per 4B5B decoder: first symbol = low nibble, second symbol = high nibble.
        data_byte = (high_nibble_symbol << 4) | low_nibble_symbol
        """
        low_nibble = byte_val & 0x0F
        high_nibble = (byte_val >> 4) & 0x0F
        self._send_4b5b_symbol(self.DATA_ENCODE[low_nibble])
        self._send_4b5b_symbol(self.DATA_ENCODE[high_nibble])

    def _send_preamble(self):
        """Send NRZI preamble for clock recovery.

        The NRZI decoder needs `preamble_len` (default 16) rising edges to sync.
        Sending all-1 bits (toggles) creates a square wave with one rising edge
        every 2 bit periods.  32 toggles = 16 rising edges.
        After sync, the decoder calculates symbol_len and starts decoding.
        """
        for _ in range(32):
            self._send_nrzi_bit(1)  # toggle every bit period

    def _send_idle_symbols(self, count):
        """Send 4B5B IDLE symbols (0b11111 = all 1s = all toggles in NRZI)."""
        for _ in range(count):
            self._send_4b5b_symbol(self.CTRL_I)

    def generate_testdata(self):
        self.channel = self.channels_map.get('data', 0)
        self.builder.set_idle(self.channel, 0)

        # Brief idle period before preamble so the first rising edge is clear
        self.builder.set_level(self.channel, 0, self.bit_width * 4)

        # NRZI preamble: 32 toggles for clock sync (16 rising edges)
        self._send_preamble()

        # 4B5B IDLE symbols - helps 4B5B decoder find symbol boundaries
        # since IDLE (0b11111) is all 1s, any 5-bit window of 1s is valid
        self._send_idle_symbols(5)

        # JK start sequence - triggers 4B5B decoder's JK flag and
        # Ethernet decoder's WAITING state
        self._send_4b5b_symbol(self.CTRL_J)
        self._send_4b5b_symbol(self.CTRL_K)

        # Ethernet preamble: 7 bytes of 0x55
        # The Ethernet decoder accumulates these in WAITING state
        for _ in range(7):
            self._send_data_byte(0x55)

        # Start Frame Delimiter: 0xD5
        # Triggers transition from WAITING to DST_MAC state
        self._send_data_byte(0xD5)

        # Destination MAC: FF:FF:FF:FF:FF:FF (broadcast)
        for b in [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]:
            self._send_data_byte(b)

        # Source MAC: 00:11:22:33:44:55
        for b in [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]:
            self._send_data_byte(b)

        # EtherType: 0x0800 (IPv4)
        self._send_data_byte(0x08)
        self._send_data_byte(0x00)

        # Payload: 46 bytes (minimum Ethernet payload)
        payload = bytes([i & 0xFF for i in range(46)])
        for b in payload:
            self._send_data_byte(b)

        # FCS: CRC32 of frame (DST MAC + SRC MAC + EtherType + Payload)
        # The Ethernet decoder verifies: crc32(frame_with_fcs) == 0x2144DF1C
        frame = bytes([0xFF] * 6 + [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]
                      + [0x08, 0x00] + list(payload))
        crc = zlib.crc32(frame) & 0xFFFFFFFF
        fcs = crc.to_bytes(4, byteorder='little')
        for b in fcs:
            self._send_data_byte(b)

        # End sequence: T (Terminate) + R (Reset)
        # T triggers FCS verification and frame completion
        # R resets the Ethernet decoder state
        self._send_4b5b_symbol(self.CTRL_T)
        self._send_4b5b_symbol(self.CTRL_R)

        # Trailing IDLE symbols
        self._send_idle_symbols(5)
