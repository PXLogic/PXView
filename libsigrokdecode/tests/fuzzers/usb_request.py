import math
from .base import *


class UsbRequestGenerator:
    """USB Request stack decoder fuzzer.

    Generates valid USB Full-Speed signalling waveforms on D+ and D- channels.
    When decoded through usb_signalling_c -> usb_packet_c, produces USB packets
    that trigger the usb_request_c decoder.

    We generate a complete USB SETUP transaction: SETUP token + DATA0 + ACK.
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.dp = channels_map.get('dp', 0)
        self.dm = channels_map.get('dm', 1)
        self._last_state = 'J'  # Full speed idle is J (DP=1, DM=0)

    def _nrzi_send_bit(self, bit, bit_width):
        """Send a single NRZI-encoded bit on DP/DM.
        NRZI: 0 = toggle line state, 1 = keep same state.
        """
        if bit == 0:
            self._last_state = 'K' if self._last_state == 'J' else 'J'

        dp = 1 if self._last_state == 'J' else 0
        dm = 1 - dp
        self.builder.set_level(self.dp, dp, bit_width)
        self.builder.pos -= bit_width
        self.builder.set_level(self.dm, dm, bit_width)

    def _send_usb_packet(self, bit_width, data_bytes):
        """Send a complete USB packet: SYNC + data (with bit stuffing) + EOP."""
        # SYNC: raw bits 00000001 -> NRZI produces KJKJKJKK
        for b in [0, 0, 0, 0, 0, 0, 0, 1]:
            self._nrzi_send_bit(b, bit_width)

        # Data payload with bit stuffing
        ones_count = 0
        for byte_val in data_bytes:
            for i in range(8):
                bit = (byte_val >> i) & 1
                if bit == 1:
                    ones_count += 1
                    self._nrzi_send_bit(1, bit_width)
                    if ones_count == 6:
                        self._nrzi_send_bit(0, bit_width)
                        ones_count = 0
                else:
                    self._nrzi_send_bit(0, bit_width)
                    ones_count = 0

        # EOP: SE0 for 2 bit widths, then J for 1 bit width
        self.builder.set_level(self.dp, 0, bit_width * 2)
        self.builder.pos -= bit_width * 2
        self.builder.set_level(self.dm, 0, bit_width * 2)
        self.builder.set_level(self.dp, 1, bit_width)
        self.builder.pos -= bit_width
        self.builder.set_level(self.dm, 0, bit_width)

    def _crc5(self, data_bits):
        """Compute USB CRC-5 for token packets."""
        crc = 0x1F
        for bit in data_bits:
            tmp = (crc >> 4) & 1
            crc = ((crc << 1) | bit) & 0x1F
            if tmp:
                crc ^= 0x05
        crc ^= 0x1F
        return crc & 0x1F

    def _crc16(self, data_bytes):
        """Compute USB CRC-16 for data packets."""
        crc = 0xFFFF
        for byte in data_bytes:
            for i in range(8):
                bit = (byte >> i) & 1
                tmp = crc & 1
                crc = (crc >> 1) | (bit << 15)
                if tmp:
                    crc ^= 0xA001
        crc ^= 0xFFFF
        return crc & 0xFFFF

    def _make_token_packet(self, pid, addr, ep):
        """Build raw bytes for a USB token packet (PID + addr/ep + CRC5)."""
        pid_byte = (pid & 0x0F) | ((~pid & 0x0F) << 4)
        addr_ep = (addr & 0x7F) | ((ep & 0x0F) << 7)
        data_bits = []
        for i in range(11):
            data_bits.append((addr_ep >> i) & 1)
        crc5 = self._crc5(data_bits)
        all_bits = data_bits + [(crc5 >> i) & 1 for i in range(5)]
        val = 0
        for i, bit in enumerate(all_bits):
            val |= (bit << i)
        return [pid_byte, val & 0xFF, (val >> 8) & 0xFF]

    def _make_data_packet(self, pid, data):
        """Build raw bytes for a USB data packet (PID + data + CRC16)."""
        pid_byte = (pid & 0x0F) | ((~pid & 0x0F) << 4)
        crc16 = self._crc16(data)
        return [pid_byte] + list(data) + [crc16 & 0xFF, (crc16 >> 8) & 0xFF]

    def _make_handshake_packet(self, pid):
        """Build raw bytes for a USB handshake packet (PID only)."""
        pid_byte = (pid & 0x0F) | ((~pid & 0x0F) << 4)
        return [pid_byte]

    def generate_testdata(self):
        # Full speed: 12 Mbps
        fs_bitrate = 12e6
        bit_width = max(2, int(self.samplerate / fs_bitrate))

        # Set idle state: J (DP=1, DM=0 for full-speed)
        self.builder.set_idle(self.dp, 1)
        self.builder.set_idle(self.dm, 0)

        # Initial idle period
        self.builder.write_channels({self.dp: 1, self.dm: 0}, bit_width * 20)

        # USB PID values:
        # SETUP = 0x0D, DATA0 = 0x03, ACK = 0x02
        SETUP_PID = 0x0D
        DATA0_PID = 0x03
        ACK_PID = 0x02

        # Transaction 1: SETUP token + DATA0 + ACK
        # SETUP token: addr=0, ep=0
        self._last_state = 'J'
        self._send_usb_packet(bit_width, self._make_token_packet(SETUP_PID, 0, 0))

        # Idle between packets
        self.builder.write_channels({self.dp: 1, self.dm: 0}, bit_width * 4)

        # DATA0: 8-byte setup packet (GET_DESCRIPTOR request)
        # bmRequestType=0x80, bRequest=0x06, wValue=0x0100, wIndex=0x0000, wLength=0x0040
        setup_data = bytes([0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00])
        self._last_state = 'J'
        self._send_usb_packet(bit_width, self._make_data_packet(DATA0_PID, setup_data))

        # Idle between packets
        self.builder.write_channels({self.dp: 1, self.dm: 0}, bit_width * 4)

        # ACK handshake
        self._last_state = 'J'
        self._send_usb_packet(bit_width, self._make_handshake_packet(ACK_PID))

        # Final idle
        self.builder.write_channels({self.dp: 1, self.dm: 0}, bit_width * 20)
