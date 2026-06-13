import zlib
from .base import *


class Ipv4Generator:
    """IPv4 stack decoder fuzzer.

    IPv4 is a stack decoder (input: ethernet). This fuzzer generates the
    root-level NRZI waveform that, when decoded through nrzi_c -> 4b5b_c ->
    ethernet_c, produces an Ethernet frame containing an IPv4 packet.

    The IPv4 C decoder requires:
    - cmd == "PAYLOAD" with >= 2 fields
    - payload_len >= 20 bytes
    - block_count >= 20
    - IHL == 20 (standard header only)

    We generate an Ethernet II frame with EtherType 0x0800 (IPv4) containing
    a valid 20-byte IPv4 header with correct checksum.
    """

    DATA_ENCODE = {
        0x0: 0b11110, 0x1: 0b01001, 0x2: 0b10100, 0x3: 0b10101,
        0x4: 0b01010, 0x5: 0b01011, 0x6: 0b01110, 0x7: 0b01111,
        0x8: 0b10010, 0x9: 0b10011, 0xA: 0b10110, 0xB: 0b10111,
        0xC: 0b11010, 0xD: 0b11011, 0xE: 0b11100, 0xF: 0b11101,
    }

    CTRL_J = 0b11000
    CTRL_K = 0b10001
    CTRL_T = 0b01101
    CTRL_R = 0b00111
    CTRL_I = 0b11111

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.bit_width = max(2, samplerate // 100000)
        self.current_level = 0

    def _send_nrzi_bit(self, data_bit):
        if data_bit == 1:
            self.current_level = 1 - self.current_level
        self.builder.set_level(self.channel, self.current_level, self.bit_width)

    def _send_4b5b_symbol(self, symbol_5bit):
        for i in range(4, -1, -1):
            self._send_nrzi_bit((symbol_5bit >> i) & 1)

    def _send_data_byte(self, byte_val):
        low_nibble = byte_val & 0x0F
        high_nibble = (byte_val >> 4) & 0x0F
        self._send_4b5b_symbol(self.DATA_ENCODE[low_nibble])
        self._send_4b5b_symbol(self.DATA_ENCODE[high_nibble])

    def _ip_checksum(self, header_bytes):
        """Compute IP header checksum."""
        while len(header_bytes) % 2 != 0:
            header_bytes += b'\x00'
        total = 0
        for i in range(0, len(header_bytes), 2):
            total += (header_bytes[i] << 8) | header_bytes[i + 1]
        while total >> 16:
            total = (total & 0xFFFF) + (total >> 16)
        return (~total) & 0xFFFF

    def generate_testdata(self):
        self.channel = self.channels_map.get('data', 0)
        self.builder.set_idle(self.channel, 0)

        # Brief idle before preamble
        self.builder.set_level(self.channel, 0, self.bit_width * 4)

        # NRZI preamble: 32 toggles for clock sync
        for _ in range(32):
            self._send_nrzi_bit(1)

        # 4B5B IDLE symbols
        for _ in range(5):
            self._send_4b5b_symbol(self.CTRL_I)

        # JK start sequence
        self._send_4b5b_symbol(self.CTRL_J)
        self._send_4b5b_symbol(self.CTRL_K)

        # Ethernet preamble: 7 bytes of 0x55
        for _ in range(7):
            self._send_data_byte(0x55)

        # SFD: 0xD5
        self._send_data_byte(0xD5)

        # DST MAC: FF:FF:FF:FF:FF:FF (broadcast)
        for b in [0xFF] * 6:
            self._send_data_byte(b)

        # SRC MAC: 00:11:22:33:44:55
        for b in [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]:
            self._send_data_byte(b)

        # EtherType: 0x0800 (IPv4)
        self._send_data_byte(0x08)
        self._send_data_byte(0x00)

        # IPv4 header (20 bytes, IHL=5):
        # Version=4, IHL=5 -> 0x45
        # DSCP=0, ECN=0 -> 0x00
        # Total length: 20 (header only, no payload) -> 0x0014
        # Identification: 0x1234
        # Flags=0, Fragment offset=0 -> 0x0000
        # TTL=64 -> 0x40
        # Protocol=17 (UDP) -> 0x11
        # Header checksum: computed below
        # Source IP: 192.168.1.1
        # Destination IP: 192.168.1.2
        ip_header_no_checksum = bytes([
            0x45, 0x00,              # Version, IHL, DSCP, ECN
            0x00, 0x28,              # Total length: 40 (20 header + 20 data)
            0x12, 0x34,              # Identification
            0x00, 0x00,              # Flags, Fragment offset
            0x40,                    # TTL = 64
            0x11,                    # Protocol = UDP (17)
            0x00, 0x00,              # Header checksum (placeholder)
            192, 168, 1, 1,         # Source IP
            192, 168, 1, 2,         # Destination IP
        ])
        checksum = self._ip_checksum(ip_header_no_checksum)
        ip_header = bytearray(ip_header_no_checksum)
        ip_header[10] = (checksum >> 8) & 0xFF
        ip_header[11] = checksum & 0xFF

        # IP payload: 20 bytes of data (to make total length 40)
        ip_payload = bytes([i & 0xFF for i in range(20)])

        for b in ip_header:
            self._send_data_byte(b)
        for b in ip_payload:
            self._send_data_byte(b)

        # FCS: CRC32 of Ethernet frame
        frame = bytes([0xFF] * 6 + [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]
                      + [0x08, 0x00] + list(ip_header) + list(ip_payload))
        crc = zlib.crc32(frame) & 0xFFFFFFFF
        fcs = crc.to_bytes(4, byteorder='little')
        for b in fcs:
            self._send_data_byte(b)

        # End sequence: T + R
        self._send_4b5b_symbol(self.CTRL_T)
        self._send_4b5b_symbol(self.CTRL_R)

        # Trailing IDLE
        for _ in range(5):
            self._send_4b5b_symbol(self.CTRL_I)
