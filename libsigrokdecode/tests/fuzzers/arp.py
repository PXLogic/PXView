import zlib
from .base import *


class ArpGenerator:
    """ARP stack decoder fuzzer.

    ARP is a stack decoder (input: ethernet). This fuzzer generates the
    root-level NRZI waveform that, when decoded through nrzi_c -> 4b5b_c ->
    ethernet_c, produces an Ethernet frame containing an ARP packet.

    The ARP C decoder requires:
    - cmd == "PAYLOAD" with >= 4 fields
    - payload_len >= 28 bytes
    - block_count >= 28

    We generate an Ethernet II frame with EtherType 0x0806 (ARP) containing
    a valid 28-byte ARP Request packet.
    """

    # 4B5B data encoding: nibble value -> 5-bit symbol
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

        # EtherType: 0x0806 (ARP)
        self._send_data_byte(0x08)
        self._send_data_byte(0x06)

        # ARP Request packet (28 bytes):
        # Hardware type: 0x0001 (Ethernet)
        # Protocol type: 0x0800 (IPv4)
        # Hardware address length: 6
        # Protocol address length: 4
        # Operation: 0x0001 (Request)
        # Sender MAC: 00:11:22:33:44:55
        # Sender IP: 192.168.1.1
        # Target MAC: 00:00:00:00:00:00
        # Target IP: 192.168.1.2
        arp_packet = bytes([
            0x00, 0x01,  # Hardware type = Ethernet
            0x08, 0x00,  # Protocol type = IPv4
            0x06,        # Hardware address length
            0x04,        # Protocol address length
            0x00, 0x01,  # Operation = Request
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55,  # Sender MAC
            192, 168, 1, 1,                        # Sender IP
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  # Target MAC
            192, 168, 1, 2,                        # Target IP
        ])
        for b in arp_packet:
            self._send_data_byte(b)

        # Padding to meet minimum Ethernet frame size (64 bytes - 14 header - 4 FCS = 46 payload)
        # ARP is 28 bytes, need 18 more bytes of padding
        for i in range(18):
            self._send_data_byte(0x00)

        # FCS: CRC32 of frame
        frame = bytes([0xFF] * 6 + [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]
                      + [0x08, 0x06] + list(arp_packet) + [0x00] * 18)
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
