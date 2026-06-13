import math
from .base import *

class FlexrayGenerator:
    """1 channel: FlexRay bus.
    Generates a valid FlexRay frame with TSS, FSS, BSS, header, CRC, and FES.
    The decoder expects NRZ-encoded data with BSS (10) before each byte."""
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('channel', 0)
        # Default bitrate 10Mbps; bit_width must be >= 1 for decoder to work
        self.bitrate = 10000000
        self.bit_width = max(1, int(self.samplerate / self.bitrate))
        self.builder.set_idle(self.ch, 1)  # Idle high

    @staticmethod
    def _crc(data, data_len_bits, polynom, crc_len_bits, iv=0, xor_val=0):
        """FlexRay CRC calculation (same algorithm as the decoder)."""
        reg = iv ^ xor_val
        for i in range(data_len_bits - 1, -1, -1):
            bit = ((reg >> (crc_len_bits - 1)) & 1) ^ ((data >> i) & 1)
            reg <<= 1
            if bit:
                reg ^= polynom
        mask = (1 << crc_len_bits) - 1
        return (reg & mask) ^ xor_val

    def _send_bit(self, level):
        """Send one bit on the channel."""
        self.builder.set_level(self.ch, level, self.bit_width)

    def _send_byte_with_bss(self, byte_val):
        """Send BSS (1, 0) followed by 8 data bits (MSB first)."""
        self._send_bit(1)  # BSS high
        self._send_bit(0)  # BSS low
        for i in range(7, -1, -1):
            self._send_bit((byte_val >> i) & 1)

    def generate_testdata(self):
        bw = self.bit_width

        # --- Build frame content (bits excluding BSS) ---
        # bit 0: FSS = 1
        # bit 1: reserved = 0
        # bit 2: PPI = 0
        # bit 3: NFI = 1 (data frame)
        # bit 4: sync = 0
        # bit 5: startup = 0
        # bits 6-16: ID = 1 (11 bits MSB first)
        # bits 17-23: length = 2 (7 bits MSB first) -> 2 payload words = 4 bytes
        # bits 24-34: HCRC (11 bits)
        # bits 35-40: cycle = 0 (6 bits MSB first)
        # bits 41-56: 2 data bytes (16 bits) -- payload_length=2 means 2*2=4 bytes
        # Actually payload_length in header = half of real payload size
        # So length=2 means 4 data bytes

        # Use length=0 for simplicity (null data frame, no payload)
        frame_id = 1
        payload_length = 0  # 0 payload half-words = 0 data bytes
        cycle = 0

        # Construct header bits 4-23 for HCRC calculation (20 bits)
        # bits 4-5: sync=0, startup=0
        # bits 6-16: ID (11 bits)
        # bits 17-23: length (7 bits)
        header_bits_4_23 = 0
        header_bits_4_23 |= (0 << 18)  # sync = 0
        header_bits_4_23 |= (0 << 17)  # startup = 0
        header_bits_4_23 |= (frame_id << 6)  # ID (11 bits, shifted to position)
        header_bits_4_23 |= payload_length  # length (7 bits)

        # Calculate HCRC
        hcrc = self._crc(header_bits_4_23, 20, 0x385, 11, 0x01A, 0)

        # Now construct the full frame bits for frame CRC
        # bits 1-40 (40 bits): reserved + PPI + NFI + sync + startup + ID + length + HCRC + cycle
        # Build as a single integer, MSB first
        frame_bits = 0
        frame_bits |= (0 << 39)  # reserved = 0 (bit 1)
        frame_bits |= (0 << 38)  # PPI = 0 (bit 2)
        frame_bits |= (1 << 37)  # NFI = 1 (data frame) (bit 3)
        frame_bits |= (0 << 36)  # sync = 0 (bit 4)
        frame_bits |= (0 << 35)  # startup = 0 (bit 5)
        # ID: 11 bits (bits 6-16)
        for i in range(11):
            frame_bits |= (((frame_id >> (10 - i)) & 1) << (34 - i))
        # Length: 7 bits (bits 17-23)
        for i in range(7):
            frame_bits |= (((payload_length >> (6 - i)) & 1) << (23 - i))
        # HCRC: 11 bits (bits 24-34)
        for i in range(11):
            frame_bits |= (((hcrc >> (10 - i)) & 1) << (16 - i))
        # Cycle: 6 bits (bits 35-40)
        for i in range(6):
            frame_bits |= (((cycle >> (5 - i)) & 1) << (5 - i))

        # Calculate frame CRC over bits 1-40 (40 bits)
        fcrc = self._crc(frame_bits, 40, 0x5D6DCB, 24, 0xFEDCBA, 0)

        # --- Generate waveform ---
        # TSS: low for 5 bit times (minimum)
        self.builder.set_level(self.ch, 0, bw * 5)

        # FSS: 1 bit high (bit 0)
        self._send_bit(1)

        # Header byte 0: reserved(0) + PPI(0) + NFI(1) + sync(0) + startup(0) + ID[10:7]
        byte0 = (0 << 7) | (0 << 6) | (1 << 5) | (0 << 4) | (0 << 3) | ((frame_id >> 8) & 0x07)
        self._send_byte_with_bss(byte0)

        # Header byte 1: ID[6:0] + length[6]
        byte1 = ((frame_id & 0x7F) << 1) | ((payload_length >> 6) & 1)
        self._send_byte_with_bss(byte1)

        # Header byte 2: length[5:0] + HCRC[10:7]
        byte2 = ((payload_length & 0x3F) << 2) | ((hcrc >> 9) & 0x03)
        self._send_byte_with_bss(byte2)

        # Header byte 3: HCRC[6:0] + cycle[5]
        byte3 = ((hcrc & 0x7F) << 1) | ((cycle >> 5) & 1)
        self._send_byte_with_bss(byte3)

        # Header byte 4: cycle[4:0] + FCRC[23:20]
        byte4 = ((cycle & 0x1F) << 3) | ((fcrc >> 21) & 0x07)
        self._send_byte_with_bss(byte4)

        # FCRC bytes (3 bytes for 24-bit CRC) - no data payload since length=0
        # After header byte 4, the next bits are frame CRC (24 bits)
        # Byte 5: FCRC[19:12]
        byte5 = (fcrc >> 12) & 0xFF
        self._send_byte_with_bss(byte5)

        # Byte 6: FCRC[11:4]
        byte6 = (fcrc >> 4) & 0xFF
        self._send_byte_with_bss(byte6)

        # Byte 7: FCRC[3:0] + FES high + DTS + CID[7:5]
        # After FCRC, end_of_frame is set, so no more BSS
        # FCRC remaining 4 bits
        for i in range(3, -1, -1):
            self._send_bit((fcrc >> i) & 1)

        # FES: 1, 0 (2 bits)
        self._send_bit(1)
        self._send_bit(0)

        # DTS bit: 1 (static frame, not dynamic)
        self._send_bit(1)

        # CID: 11 bits all high (channel idle delimiter)
        for _ in range(11):
            self._send_bit(1)
