import math
from .base import *

class CanFdGenerator:
    """Generates CAN FD bus waveforms for the can_fd_c decoder.

    Builds valid CAN FD standard data frames on a single can_rx channel.
    The can_fd_c decoder expects NRZ encoding (recessive=1, dominant=0)
    with dynamic bit stuffing from SOF through the data field, then fixed
    stuffing in the CRC phase.
    """

    # CAN FD DLC-to-length mapping table
    DLC2LEN = [0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64]

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.channel = channels_map['can_rx']
        # Match the can_fd_c decoder default nominal_bitrate (1 Mbps)
        self.nominal_bitrate = 1000000
        self.bit_width = max(1, int(samplerate / self.nominal_bitrate))
        # CAN bus idle is recessive (1)
        self.builder.set_idle(self.channel, 1)

    # ---- CRC helpers ----

    def _crc15_can(self, bits):
        """CRC-15 for classic CAN frames (used when FDF=0)."""
        crc = 0
        for bit in bits:
            if (bit ^ (crc >> 14)) & 1:
                crc = (crc << 1) ^ 0x4599
            else:
                crc <<= 1
        return crc & 0x7fff

    def _crc17_canfd(self, bits):
        """CRC-17 for CAN FD frames with DLC <= 16 bytes.

        Polynomial: x^17 + x^16 + x^14 + x^13 + x^11 + x^6 + x^4 + x^3 + x + 1
        Represented without x^17 term as 0x1685B.
        """
        crc = 0
        poly = 0x1685B
        for bit in bits:
            if (bit ^ (crc >> 16)) & 1:
                crc = (crc << 1) ^ poly
            else:
                crc <<= 1
        return crc & 0x1ffff

    def _crc21_canfd(self, bits):
        """CRC-21 for CAN FD frames with DLC > 16 bytes.

        Polynomial: x^21 + x^20 + x^13 + x^11 + x^7 + x^4 + x^3 + x + 1
        Represented without x^21 term as 0x10289B.
        """
        crc = 0
        poly = 0x10289B
        for bit in bits:
            if (bit ^ (crc >> 20)) & 1:
                crc = (crc << 1) ^ poly
            else:
                crc <<= 1
        return crc & 0x1fffff

    # ---- Bit-stuffing helpers ----

    @staticmethod
    def _binary_to_gray(val):
        """Convert a binary value to Gray code."""
        return val ^ (val >> 1)

    @staticmethod
    def _parity_bit(gray_count):
        """Compute the parity bit for the 3-bit stuff-count Gray code.

        The 4-bit value (3 Gray-code bits + 1 parity bit) must have even
        parity, matching the can_fd_c decoder's ParityCheck table.
        """
        ones = bin(gray_count & 0x7).count('1')
        return ones % 2  # 1 if odd → makes total even; 0 if even → stays even

    @staticmethod
    def _apply_bit_stuffing(bits):
        """Apply CAN bit stuffing: after 5 consecutive same-level bits,
        insert the complement.  Returns (stuffed_bits, stuff_count)."""
        stuffed = [bits[0]]
        consecutive = 1
        last = bits[0]
        stuff_count = 0
        for b in bits[1:]:
            if b == last:
                consecutive += 1
            else:
                consecutive = 1
                last = b
            stuffed.append(b)
            if consecutive == 5:
                stuffed.append(1 - last)
                last = 1 - last
                consecutive = 1
                stuff_count += 1
        return stuffed, stuff_count

    # ---- Frame builders ----

    def _len_to_dlc(self, data_len):
        """Return the smallest DLC value that can hold *data_len* bytes."""
        for dlc, length in enumerate(self.DLC2LEN):
            if length >= data_len:
                return dlc
        return 15

    def _emit_bits(self, bits):
        """Write a list of bit values to the can_rx channel."""
        for bit in bits:
            self.builder.set_level(self.channel, bit, self.bit_width)

    def _add_crc_with_fixed_stuffing(self, stuffed, crc, crc_len):
        """Append *crc_len* CRC bits to *stuffed* with a fixed stuff bit
        inserted after every 4 CRC data bits (CAN FD CRC phase rule)."""
        crc_bits = []
        for i in range(crc_len - 1, -1, -1):
            crc_bits.append((crc >> i) & 1)

        crc_with_stuff = []
        for i, bit in enumerate(crc_bits):
            if i > 0 and i % 4 == 0:
                # Fixed stuff bit: complement of the preceding bit
                crc_with_stuff.append(1 - crc_with_stuff[-1])
            crc_with_stuff.append(bit)

        stuffed.extend(crc_with_stuff)

    def _add_trailer(self, stuffed):
        """Append the fixed-form trailer: CRC delimiter, ACK slot,
        ACK delimiter, EOF, and IFS — all outside bit-stuffing scope."""
        stuffed.extend([1, 0, 1])   # CRC Delim (R), ACK (D), ACK Delim (R)
        stuffed.extend([1] * 7)     # EOF (7 recessive bits)
        stuffed.extend([1] * 3)     # IFS  (3 recessive bits)

    # ---- Public API ----

    def send_fd_frame(self, ident, data, brs=False, esi=False):
        """Send a CAN FD standard data frame (FDF=1).

        Parameters
        ----------
        ident : int
            11-bit CAN identifier.
        data : list[int]
            Data bytes (will be padded to the next valid FD length).
        brs : bool
            Bit Rate Switch flag.  When True the data phase would use the
            fast bitrate; the generator keeps the nominal bitrate throughout
            so the decoder must be configured accordingly (or brs=False).
        esi : bool
            Error State Indicator flag.
        """
        data_len = len(data)
        dlc = self._len_to_dlc(data_len)
        actual_len = self.DLC2LEN[dlc]
        padded_data = list(data) + [0] * (actual_len - data_len)

        # --- Build raw (un-stuffed) bits: SOF through data ---
        raw_bits = [0]  # SOF — dominant

        # 11-bit identifier (MSB first)
        for i in range(10, -1, -1):
            raw_bits.append((ident >> i) & 1)

        # Control field for standard FD frame
        raw_bits.append(0)                  # RRS (Remote Request Substitution)
        raw_bits.append(0)                  # IDE (0 = standard)
        raw_bits.append(1)                  # FDF (1 = FD format)
        raw_bits.append(0)                  # RB0 (reserved)
        raw_bits.append(1 if brs else 0)    # BRS
        raw_bits.append(1 if esi else 0)    # ESI

        # DLC (4 bits, MSB first)
        for i in range(3, -1, -1):
            raw_bits.append((dlc >> i) & 1)

        # Data bytes (MSB first)
        for b in padded_data:
            for i in range(7, -1, -1):
                raw_bits.append((b >> i) & 1)

        # --- Dynamic bit stuffing (SOF through data) ---
        stuffed, stuff_count = self._apply_bit_stuffing(raw_bits)

        # --- CRC phase (fixed stuffing) ---

        # Fixed Stuff Bit after the data field — complement of last bit
        stuffed.append(1 - stuffed[-1])

        # Stuff count: 3-bit Gray code of (stuff_count mod 8)
        stuff_count_mod = stuff_count % 8
        gray_count = self._binary_to_gray(stuff_count_mod)
        for i in range(2, -1, -1):
            stuffed.append((gray_count >> i) & 1)

        # Parity bit (even parity over 3-bit gray + parity)
        parity = self._parity_bit(gray_count)
        stuffed.append(parity)

        # CRC computation over destuffed bits (after SOF, through parity)
        crc_input = list(raw_bits[1:])  # everything after SOF
        for i in range(2, -1, -1):
            crc_input.append((gray_count >> i) & 1)
        crc_input.append(parity)

        if actual_len <= 16:
            crc = self._crc17_canfd(crc_input)
            crc_len = 17
        else:
            crc = self._crc21_canfd(crc_input)
            crc_len = 21

        self._add_crc_with_fixed_stuffing(stuffed, crc, crc_len)

        # --- Trailer ---
        self._add_trailer(stuffed)

        # --- Emit ---
        self._emit_bits(stuffed)

    def send_classic_frame(self, ident, data, rtr=False):
        """Send a classic CAN standard frame (FDF=0) for mixed-bus testing.

        Parameters
        ----------
        ident : int
            11-bit CAN identifier.
        data : list[int]
            Data bytes (max 8; excess is truncated).
        rtr : bool
            Remote Transmission Request flag.
        """
        dlc = min(len(data), 8)

        raw_bits = [0]  # SOF

        # 11-bit identifier
        for i in range(10, -1, -1):
            raw_bits.append((ident >> i) & 1)

        # RTR, IDE=0, r0=0
        raw_bits.extend([1 if rtr else 0, 0, 0])

        # DLC
        for i in range(3, -1, -1):
            raw_bits.append((dlc >> i) & 1)

        # Data (not for RTR frames)
        if not rtr:
            for b in data[:8]:
                for i in range(7, -1, -1):
                    raw_bits.append((b >> i) & 1)

        # CRC-15
        crc = self._crc15_can(raw_bits[1:])
        for i in range(14, -1, -1):
            raw_bits.append((crc >> i) & 1)

        # Bit stuffing (SOF through CRC)
        stuffed, _ = self._apply_bit_stuffing(raw_bits)

        # Trailer
        self._add_trailer(stuffed)

        self._emit_bits(stuffed)

    def generate_testdata(self):
        """Generate a set of CAN FD and classic CAN frames."""

        # Frame 1: CAN FD, 4 data bytes, BRS off, ESI off
        self.send_fd_frame(0x123, [0x01, 0x02, 0x03, 0x04],
                           brs=False, esi=False)

        # Inter-frame gap (recessive idle)
        self.builder.set_level(self.channel, 1, self.bit_width * 11)

        # Frame 2: CAN FD, 8 data bytes, BRS off
        self.send_fd_frame(0x456,
                           [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11],
                           brs=False, esi=False)

        self.builder.set_level(self.channel, 1, self.bit_width * 11)

        # Frame 3: CAN FD, 2 data bytes, ESI set (error-passive indicator)
        self.send_fd_frame(0x100, [0xDE, 0xAD],
                           brs=False, esi=True)

        self.builder.set_level(self.channel, 1, self.bit_width * 11)

        # Frame 4: Classic CAN, 3 data bytes (exercises FDF=0 path)
        self.send_classic_frame(0x200, [0x11, 0x22, 0x33])
