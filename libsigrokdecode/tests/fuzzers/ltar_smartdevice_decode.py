import math
from .base import *

class LtarSmartdeviceDecodeGenerator:
    """1 channel: AFSK signal.
    Generates AFSK waveform that, when decoded through afsk_c -> ltar_smartdevice_c,
    produces valid LTAR SmartDevice blocks that trigger the ltar_smartdevice_decode_c decoder.

    AFSK encoding: mark=2000Hz (bit 0), space=4000Hz (bit 1).
    LTAR SmartDevice frame: start_bit(0) + 8 data bits (LSB first) + stop_bit(1).
    Block: multiple frames separated by spacer bits (1s), ended by 14+ spacer bits.
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('afsk', 0)
        self.builder.set_idle(self.ch, 1)  # Idle high

    def _generate_afsk_bit(self, bit_val):
        """Generate AFSK waveform for one bit period.
        bit=0: mark frequency (2000Hz), bit=1: space frequency (4000Hz).
        Bit period is approximately 1/1200 seconds (Bell 202 standard).
        """
        # Bit period at 1200 baud
        bit_period = self.samplerate // 1200
        freq = 4000 if bit_val else 2000  # space=1, mark=0
        half_cycle = self.samplerate // (2 * freq)
        pos = 0
        level = 1
        while pos < bit_period:
            duration = min(half_cycle, bit_period - pos)
            self.builder.set_level(self.ch, level, duration)
            level = 1 - level
            pos += duration

    def _send_frame(self, byte_val):
        """Send one LTAR SmartDevice frame: start(0) + 8 data bits (LSB first) + stop(1)."""
        # Start bit (0)
        self._generate_afsk_bit(0)
        # 8 data bits, LSB first
        for i in range(8):
            self._generate_afsk_bit((byte_val >> i) & 1)
        # Stop bit (1)
        self._generate_afsk_bit(1)

    def _send_spacer(self, count=3):
        """Send spacer bits (1s) between frames."""
        for _ in range(count):
            self._generate_afsk_bit(1)

    def _send_block_end(self):
        """Send enough spacer bits (14+) to signal end of block."""
        for _ in range(15):
            self._generate_afsk_bit(1)

    def generate_testdata(self):
        # Generate a TAGGER-STATUS block (btype=0x02, 11 frames including checksum)
        # Frame data: [btype=0x02, player+team, weapon+shield+hunting,
        #              health, loaded_ammo, ammo_low, ammo_high,
        #              shield_time, game_min, game_sec, checksum]
        # Checksum: sum of all bytes should be 0x100 (mod 256 = 0)
        frames = [0x02,  # Block type: TAGGER-STATUS
                  0x09,  # Player 1, Team 1
                  0x05,  # Semi-Auto, Ready, Normal
                  0x64,  # Health: 100
                  0x1E,  # Loaded ammo: 30
                  0x00,  # Ammo low: 0
                  0x00,  # Ammo high: 0
                  0x00,  # Shield time: 0
                  0x05,  # Game minutes: 5
                  0x00]  # Game seconds: 0
        # Calculate checksum: 0xFF - sum of all frames
        checksum = (0xFF - sum(frames)) & 0xFF
        frames.append(checksum)

        # Send the block
        for i, byte_val in enumerate(frames):
            self._send_frame(byte_val)
            if i < len(frames) - 1:
                self._send_spacer(3)

        # End of block
        self._send_block_end()

        # Generate a second simpler block: PRIORITY-UPDATE (btype=0x01, 2 frames)
        frames2 = [0x01,  # Block type: PRIORITY-UPDATE
                   0xFF]  # Checksum (0xFF - 0x01 = 0xFE)
        # Actually: checksum = (0xFF - sum(frames2)) & 0xFF
        checksum2 = (0xFF - sum(frames2)) & 0xFF
        frames2.append(checksum2)

        for i, byte_val in enumerate(frames2):
            self._send_frame(byte_val)
            if i < len(frames2) - 1:
                self._send_spacer(3)

        self._send_block_end()
