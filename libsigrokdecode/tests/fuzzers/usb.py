import math
from .base import *

class USBGenerator:
    def __init__(self, builder, dp_ch, dm_ch, speed='full'):
        self.builder = builder
        self.dp = dp_ch
        self.dm = dm_ch
        # Full speed: 12Mbps, Low speed: 1.5Mbps
        self.bit_width = int(builder.samplerate / (12e6 if speed == 'full' else 1.5e6))
        self.last_state = 'J' # Full speed idle is J (DP high, DM low)
        self.builder.set_idle(dp_ch, 1)
        self.builder.set_idle(dm_ch, 0)

    def _send_bit(self, bit):
        # USB uses NRZI: 0 = toggle, 1 = no change
        if bit == 0:
            self.last_state = 'K' if self.last_state == 'J' else 'J'

        dp = 1 if self.last_state == 'J' else 0
        dm = 1 - dp
        self.builder.set_level(self.dp, dp, self.bit_width)
        self.builder.pos -= self.bit_width
        self.builder.set_level(self.dm, dm, self.bit_width)

    def send_packet(self, data):
        # Sync: 00000001 (NRZI toggles on 0s)
        for b in [0,0,0,0,0,0,0,1]: self._send_bit(b)

        # Data with bit-stuffing (after 6 ones, insert 0)
        ones_count = 0
        for byte in data:
            for i in range(8):
                bit = (byte >> i) & 1
                if bit == 1:
                    ones_count += 1
                    self._send_bit(1)
                    if ones_count == 6:
                        self._send_bit(0)
                        ones_count = 0
                else:
                    self._send_bit(0)
                    ones_count = 0

        # EOP: SE0 for 2 bit widths, then J
        self.builder.set_level(self.dp, 0, self.bit_width * 2)
        self.builder.pos -= self.bit_width * 2
        self.builder.set_level(self.dm, 0, self.bit_width * 2)
        self.builder.set_level(self.dp, 1, self.bit_width)
        self.builder.pos -= self.bit_width
        self.builder.set_level(self.dm, 0, self.bit_width)


class UsbSignallingGenerator:
    """Fuzzer for usb_signalling_c decoder.

    Generates valid USB Full-Speed signalling waveforms on D+ and D- channels.
    The decoder detects SOP (J->K transition), decodes NRZI-encoded bits with
    bit-stuffing, and recognises EOP (SE0 for 2 bit times then J).
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate

    def _nrzi_send_bit(self, bit, dp_ch, dm_ch, bit_width):
        """Send a single NRZI-encoded bit on DP/DM.

        Maintains the last bus state in self._last_state ('J' or 'K').
        NRZI: 0 = toggle line state, 1 = keep same state.
        """
        if bit == 0:
            self._last_state = 'K' if self._last_state == 'J' else 'J'

        # Full-speed: J = DP high, DM low; K = DP low, DM high
        dp = 1 if self._last_state == 'J' else 0
        dm = 1 - dp
        self.builder.set_level(dp_ch, dp, bit_width)
        self.builder.pos -= bit_width
        self.builder.set_level(dm_ch, dm, bit_width)

    def _send_usb_packet(self, dp_ch, dm_ch, bit_width, data_bytes):
        """Send a complete USB packet: SYNC + data (with bit stuffing) + EOP.

        SYNC is the raw bit pattern 00000001 which NRZI-encodes to KJKJKJKJ.
        After 6 consecutive 1-bits a stuff bit (0) is inserted.
        EOP is SE0 (both lines low) for 2 bit times, then J for 1 bit time.
        """
        # SYNC: raw bits 00000001 → NRZI produces K J K J K J K J
        for b in [0, 0, 0, 0, 0, 0, 0, 1]:
            self._nrzi_send_bit(b, dp_ch, dm_ch, bit_width)

        # Data payload with bit stuffing
        ones_count = 0
        for byte_val in data_bytes:
            for i in range(8):
                bit = (byte_val >> i) & 1
                if bit == 1:
                    ones_count += 1
                    self._nrzi_send_bit(1, dp_ch, dm_ch, bit_width)
                    if ones_count == 6:
                        self._nrzi_send_bit(0, dp_ch, dm_ch, bit_width)
                        ones_count = 0
                else:
                    self._nrzi_send_bit(0, dp_ch, dm_ch, bit_width)
                    ones_count = 0

        # EOP: SE0 for 2 bit widths, then J for 1 bit width
        self.builder.set_level(dp_ch, 0, bit_width * 2)
        self.builder.pos -= bit_width * 2
        self.builder.set_level(dm_ch, 0, bit_width * 2)
        self.builder.set_level(dp_ch, 1, bit_width)
        self.builder.pos -= bit_width
        self.builder.set_level(dm_ch, 0, bit_width)

    def generate_testdata(self):
        dp_ch = self.channels_map.get("dp", 0)
        dm_ch = self.channels_map.get("dm", 1)

        # Use full-speed USB (12 Mbps). At 24 MHz samplerate this gives
        # bit_width = 2 samples/bit which is minimal but workable.
        # If the samplerate is too low, fall back to low-speed.
        fs_bitrate = 12e6
        ls_bitrate = 1.5e6
        fs_bit_width = int(self.samplerate / fs_bitrate)
        ls_bit_width = int(self.samplerate / ls_bitrate)

        if fs_bit_width >= 2:
            bit_width = fs_bit_width
        else:
            bit_width = ls_bit_width

        # Full-speed idle is J: DP=1, DM=0.
        # Write an idle period so the decoder sees J before SOP.
        self.builder.write_channels({dp_ch: 1, dm_ch: 0}, bit_width * 20)

        # Initial bus state for NRZI tracking
        self._last_state = 'J'

        # Packet 1: SETUP token — PID 0x2D (DATA0 would be 0xC3)
        # A simple GET_DESCRIPTOR request payload
        self._send_usb_packet(dp_ch, dm_ch, bit_width,
                              [0x2D, 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00])

        # Idle gap between packets
        self.builder.write_channels({dp_ch: 1, dm_ch: 0}, bit_width * 20)

        # Packet 2: DATA0 — PID 0xC3 + a few data bytes
        self._last_state = 'J'
        self._send_usb_packet(dp_ch, dm_ch, bit_width,
                              [0xC3, 0x12, 0x01, 0x00, 0x00, 0x00])

        # Idle gap
        self.builder.write_channels({dp_ch: 1, dm_ch: 0}, bit_width * 20)

        # Packet 3: ACK — PID 0x4B (just the PID byte, no data)
        self._last_state = 'J'
        self._send_usb_packet(dp_ch, dm_ch, bit_width, [0x4B])

        # Final idle
        self.builder.set_idle(dp_ch, 1)
        self.builder.set_idle(dm_ch, 0)

