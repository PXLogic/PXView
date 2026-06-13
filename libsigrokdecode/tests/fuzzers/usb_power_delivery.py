import math
from .base import *

class USBPowerDeliveryGenerator:
    """USB PD BMC (Biphase Mark Coding) generator.
    1 channel: CC(ch0).
    Protocol: preamble (alternating 0/1) + 4b5b SOP + header + data + CRC32 + EOP.
    Large idle gap (>3*UI) separates packets.

    BMC encoding:
    - Bit '0': one transition at start of bit period, signal stays for 2*UI (full period)
    - Bit '1': transition at start and at mid-bit, each half is UI long
    - Edge interval for '0' = 2*UI, for '1' = UI
    """
    def __init__(self, builder, channel=0, samplerate=None):
        self.builder = builder
        self.ch = channel
        self.sr = samplerate if samplerate is not None else builder.samplerate
        self.ui = int(self.sr / 600000)  # UI = 1/600kHz
        builder.set_idle(channel, 0)

    def _bmc_bit(self, bit):
        """BMC encode: always transition at start, bit=1 has mid-bit transition.
        Bit '0': transition at start, hold for 2*UI (no mid-bit transition)
        Bit '1': transition at start, transition at mid-bit (each half = UI)"""
        current = self.builder.channels[self.ch][max(0, self.builder.pos - 1)] if self.builder.pos > 0 else 0
        new_level = 1 - current
        if bit:
            # '1': transition at start, transition at mid-bit
            self.builder.set_level(self.ch, new_level, self.ui)
            self.builder.set_level(self.ch, 1 - new_level, self.ui)
        else:
            # '0': transition at start, hold for 2*UI
            self.builder.set_level(self.ch, new_level, self.ui * 2)

    def _send_4b5b_symbol(self, data4):
        """Encode a 4-bit nibble using 4b5b and send it via BMC."""
        # 4b5b encode table: maps 4-bit value to 5-bit code
        enc4b5b = {
            0x0: 0b11110, 0x1: 0b01001, 0x2: 0b10100, 0x3: 0b10101,
            0x4: 0b01010, 0x5: 0b01011, 0x6: 0b01110, 0x7: 0b01111,
            0x8: 0b10010, 0x9: 0b10011, 0xA: 0b10110, 0xB: 0b10111,
            0xC: 0b11010, 0xD: 0b11011, 0xE: 0b11100, 0xF: 0b11101,
        }
        code5 = enc4b5b.get(data4 & 0xF, 0b11110)
        for i in range(4, -1, -1):
            self._bmc_bit((code5 >> i) & 1)

    def _send_special_symbol(self, sym):
        """Send a special 4b5b symbol via BMC."""
        # Special symbols
        SYNC1 = 0b11000  # K-code 0x11
        SYNC2 = 0b11001  # K-code 0x12
        SYNC3 = 0b11010  # K-code 0x13
        RST1  = 0b00111  # K-code 0x14
        RST2  = 0b00101  # K-code 0x15
        EOP   = 0b01101  # K-code 0x16
        symbols = {
            'SYNC-1': SYNC1, 'SYNC-2': SYNC2, 'SYNC-3': SYNC3,
            'RST-1': RST1, 'RST-2': RST2, 'EOP': EOP,
        }
        code5 = symbols[sym]
        for i in range(4, -1, -1):
            self._bmc_bit((code5 >> i) & 1)

    def _send_16bit(self, val):
        """Send a 16-bit value using 4b5b encoding (4 nibbles)."""
        for i in range(3, -1, -1):
            self._send_4b5b_symbol((val >> (i * 4)) & 0xF)

    def _send_32bit(self, val):
        """Send a 32-bit value using 4b5b encoding (8 nibbles)."""
        for i in range(7, -1, -1):
            self._send_4b5b_symbol((val >> (i * 4)) & 0xF)

    def _compute_crc32(self, data_bytes):
        """Compute CRC32 for USB PD."""
        crc = 0xFFFFFFFF
        for byte in data_bytes:
            crc ^= byte
            for _ in range(8):
                if crc & 1:
                    crc = (crc >> 1) ^ 0xEDB88320
                else:
                    crc >>= 1
        return crc ^ 0xFFFFFFFF

    def send_packet(self, header=0x1161, data_words=None):
        """Send a complete USB PD packet.
        header: 16-bit header (default: SRC, rev2, GoodCRC)
        data_words: list of 32-bit data words (None for control message)"""
        if data_words is None:
            data_words = []

        # Large idle gap before packet (>3*UI)
        self.builder.set_level(self.ch, 0, self.ui * 4)

        # Preamble: 64 bits alternating 0,1
        for i in range(64):
            self._bmc_bit(i % 2)

        # SOP sequence for "SOP": SYNC-1, SYNC-1, SYNC-1, SYNC-2
        self._send_special_symbol('SYNC-1')
        self._send_special_symbol('SYNC-1')
        self._send_special_symbol('SYNC-1')
        self._send_special_symbol('SYNC-2')

        # Header (16 bits, 4b5b encoded)
        self._send_16bit(header)

        # Data words (32 bits each, 4b5b encoded)
        for word in data_words:
            self._send_32bit(word)

        # CRC32 (over header + data, as bytes)
        crc_data = bytearray()
        crc_data.append(header & 0xFF)
        crc_data.append((header >> 8) & 0xFF)
        for word in data_words:
            crc_data.append(word & 0xFF)
            crc_data.append((word >> 8) & 0xFF)
            crc_data.append((word >> 16) & 0xFF)
            crc_data.append((word >> 24) & 0xFF)
        crc = self._compute_crc32(crc_data)
        self._send_32bit(crc)

        # EOP
        self._send_special_symbol('EOP')

        # Large idle gap after packet
        self.builder.set_level(self.ch, 0, self.ui * 4)

