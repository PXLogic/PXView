import zlib
from .base import *


class UdpGenerator:
    """UDP stack decoder fuzzer.

    UDP is a stack decoder (input: ipv4). This fuzzer generates the
    root-level NRZI waveform that, when decoded through nrzi_c -> 4b5b_c ->
    ethernet_c -> ipv4_c, produces an Ethernet frame containing an IPv4
    packet with a UDP datagram.

    The UDP C decoder requires:
    - cmd == "IP_PAYLOAD" with >= 4 fields
    - payload_len >= 8 bytes
    - block_count >= 8

    We generate an Ethernet II frame with EtherType 0x0800 (IPv4) containing
    a valid IPv4 header with protocol=17 (UDP) and a UDP header + payload.
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
        while len(header_bytes) % 2 != 0:
            header_bytes += b'\x00'
        total = 0
        for i in range(0, len(header_bytes), 2):
            total += (header_bytes[i] << 8) | header_bytes[i + 1]
        while total >> 16:
            total = (total & 0xFFFF) + (total >> 16)
        return (~total) & 0xFFFF

    def _udp_checksum(self, src_ip, dst_ip, udp_data):
        """Compute UDP checksum with IPv4 pseudo header."""
        pseudo = src_ip + dst_ip + b'\x00\x11' + len(udp_data).to_bytes(2, 'big')
        data = pseudo + udp_data
        while len(data) % 2 != 0:
            data += b'\x00'
        total = 0
        for i in range(0, len(data), 2):
            total += (data[i] << 8) | data[i + 1]
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

        # UDP payload: "Hello" (5 bytes)
        udp_payload = b'Hello'

        # UDP header (8 bytes):
        # Source port: 12345 (0x3039)
        # Destination port: 80 (0x0050)
        # Length: 8 + 5 = 13 (0x000D)
        # Checksum: computed with pseudo header
        src_ip = bytes([192, 168, 1, 1])
        dst_ip = bytes([192, 168, 1, 2])
        udp_length = 8 + len(udp_payload)
        udp_header_no_checksum = bytes([
            0x30, 0x39,  # Source port: 12345
            0x00, 0x50,  # Destination port: 80
        ]) + udp_length.to_bytes(2, 'big') + b'\x00\x00'  # Length + placeholder checksum

        udp_data = udp_header_no_checksum + udp_payload
        udp_checksum = self._udp_checksum(src_ip, dst_ip, udp_data)
        udp_data = bytearray(udp_data)
        udp_data[6] = (udp_checksum >> 8) & 0xFF
        udp_data[7] = udp_checksum & 0xFF

        # IPv4 header (20 bytes):
        ip_total_length = 20 + len(udp_data)
        ip_header_no_checksum = bytes([
            0x45, 0x00,                          # Version, IHL, DSCP, ECN
            (ip_total_length >> 8) & 0xFF,       # Total length high byte
            ip_total_length & 0xFF,              # Total length low byte
            0x12, 0x34,                          # Identification
            0x00, 0x00,                          # Flags, Fragment offset
            0x40,                                # TTL = 64
            0x11,                                # Protocol = UDP (17)
            0x00, 0x00,                          # Header checksum (placeholder)
        ]) + src_ip + dst_ip

        checksum = self._ip_checksum(ip_header_no_checksum)
        ip_header = bytearray(ip_header_no_checksum)
        ip_header[10] = (checksum >> 8) & 0xFF
        ip_header[11] = checksum & 0xFF

        for b in ip_header:
            self._send_data_byte(b)
        for b in udp_data:
            self._send_data_byte(b)

        # FCS: CRC32 of Ethernet frame
        frame = bytes([0xFF] * 6 + [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]
                      + [0x08, 0x00] + list(ip_header) + list(udp_data))
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
