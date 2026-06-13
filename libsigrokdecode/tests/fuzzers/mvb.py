import math
from .base import *

class MvbGenerator:
    """MVB (Multifunction Vehicle Bus) Manchester II generator.
    1 channel: DATA(ch0).
    MVB uses Manchester II encoding at 1.5Mbit/s (3MHz clock rate).

    CRITICAL: The MVB decoder does NOT Manchester-decode the preamble.
    Instead, it receives raw tick values (0 or 1 per half-bit period)
    and shifts them into a register to match PREAMBLE_MASTER/SLAVE.
    Only after the preamble match does it start Manchester decoding.

    The decoder's process_tick() receives tick_value (0 or 1) and:
    - Before preamble match: shifts tick_value into matching_header_ticks
    - After preamble match: Manchester decodes (0=low->high, 1=high->low per tick pair)

    The decode() loop:
    1. Waits for falling edge to start
    2. Measures notch lengths in MVB clock ticks
    3. For each tick in a notch, calls process_tick(1 if phase else 0)
    4. Preamble bits are shifted in as raw tick values

    So we need to generate the preamble as raw level transitions where
    each half-bit (tick) has the value that the decoder will feed to process_tick.
    After preamble, we Manchester-encode the data bits.

    The preamble is 18 bits: MASTER = 0b101100011100010101
    Each bit of the preamble needs to be fed as a tick_value to process_tick.
    A tick_value of 1 means the signal is high during that tick period,
    0 means low. The decoder measures notch lengths and converts to ticks.

    For the preamble, we need to generate a waveform where the notch
    transitions produce the correct tick values. Each tick is 1/(3MHz) seconds.
    At 6MHz samplerate, each tick = 2 samples.

    Preamble MASTER = 0b101100011100010101 (18 bits, MSB first)
    We need to output these as consecutive tick values: 1,0,1,1,0,0,0,1,1,1,0,0,0,1,0,1,0,1
    This means the signal level alternates according to these tick values.
    Consecutive same-value ticks merge into a longer notch.

    After preamble, Manchester-encoded data follows:
    - 0 = low-then-high (tick 0 then tick 1)
    - 1 = high-then-low (tick 1 then tick 0)
    """
    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.ch = channels_map.get('mvb', 0)
        # MVB clock rate = 3MHz, each tick = sr/3e6 samples
        self.tick_samples = max(1, int(samplerate / 3e6))
        builder.set_idle(self.ch, 1)  # Idle high

    def _send_tick_value(self, value, duration_ticks=1):
        """Set the signal to 'value' for 'duration_ticks' ticks.
        Each tick = tick_samples samples."""
        self.builder.set_level(self.ch, value, self.tick_samples * duration_ticks)

    def _send_preamble_raw(self, preamble_bits):
        """Send preamble as raw tick values (NOT Manchester encoded).
        The decoder shifts raw tick values into matching_header_ticks.
        We need to output the preamble bits as consecutive signal levels,
        merging consecutive same-value bits into longer notches."""
        prev = None
        run_length = 0
        for bit in preamble_bits:
            if bit == prev:
                run_length += 1
            else:
                if prev is not None:
                    self._send_tick_value(prev, run_length)
                prev = bit
                run_length = 1
        if prev is not None:
            self._send_tick_value(prev, run_length)

    def _manchester_bit(self, bit):
        """Manchester II encoding: 1 = high-then-low, 0 = low-then-high.
        Each half-bit is 1 MVB tick."""
        ts = self.tick_samples
        if bit:
            self.builder.set_level(self.ch, 1, ts)
            self.builder.set_level(self.ch, 0, ts)
        else:
            self.builder.set_level(self.ch, 0, ts)
            self.builder.set_level(self.ch, 1, ts)

    def _crc8_mvb(self, data_bits):
        """Compute MVB CRC-8 over a list of bit values (0/1).
        Returns a list of 8 bit values for the check sequence.
        Uses the same algorithm as the Python decoder's encode_data()."""
        # CRC polynomial: x^8 + x^7 + x^6 + x^4 + x^2 + 1 = 0xE5
        # Modulo-2 division approach matching the Python decoder
        data_str = ''.join(str(b) for b in data_bits)
        key = '11100101'  # CRC polynomial

        # Appends n-1 zeroes at end of data
        l_key = len(key)
        appended_data = data_str + '0' * (l_key - 1)

        # Modulo-2 division
        def xor_str(a, b):
            return ''.join('0' if a[i] == b[i] else '1' for i in range(1, len(b)))

        def mod2div(dividend, divisor):
            pick = len(divisor)
            tmp = dividend[0:pick]
            while pick < len(dividend):
                if tmp[0] == '1':
                    tmp = xor_str(divisor, tmp) + dividend[pick]
                else:
                    tmp = xor_str('0' * pick, tmp) + dividend[pick]
                pick += 1
            if tmp[0] == '1':
                tmp = xor_str(divisor, tmp)
            else:
                tmp = xor_str('0' * len(tmp), tmp)
            return tmp

        remainder = mod2div(appended_data, key)

        # Invert and add parity
        def invert_str(data):
            return ''.join(str(1 ^ int(c, 2)) for c in data)

        def parity_str(data):
            res = 0
            for c in data:
                res ^= int(c, 2)
            return str(res)

        check = invert_str(remainder + parity_str(data_str + remainder))
        return [int(c) for c in check]

    def send_master_frame(self, f_code=0, address=0):
        """Send a master frame: 18-bit master preamble + 4-bit F-code + 12-bit address + 8-bit CRC.
        F-code 0 = PD 2B (Process Data 2 bytes)."""
        # Initial idle high
        self.builder.set_level(self.ch, 1, self.tick_samples * 10)

        # The decoder waits for a falling edge to start. We need to create one.
        # After the falling edge, the decoder starts measuring notches.
        # Phase starts at False, so the first notch (low) produces tick=0.
        # The preamble tick values need to be: 1,0,1,1,0,0,0,1,1,1,0,0,0,1,0,1,0,1
        # Since ticks alternate 0,1,0,1... (low,high,low,high...), we need:
        # Notch 1 (low, phase=False): 1 tick -> tick=0 (lead-in, will be shifted out)
        # Notch 2 (high, phase=True): 1 tick -> tick=1 (preamble bit 1)
        # Notch 3 (low, phase=False): 1 tick -> tick=0 (preamble bit 2)
        # Notch 4 (high, phase=True): 2 ticks -> tick=1,1 (preamble bits 3-4)
        # Notch 5 (low, phase=False): 3 ticks -> tick=0,0,0 (preamble bits 5-7)
        # Notch 6 (high, phase=True): 3 ticks -> tick=1,1,1 (preamble bits 8-10)
        # Notch 7 (low, phase=False): 3 ticks -> tick=0,0,0 (preamble bits 11-13)
        # Notch 8 (high, phase=True): 1 tick -> tick=1 (preamble bit 14)
        # Notch 9 (low, phase=False): 1 tick -> tick=0 (preamble bit 15)
        # Notch 10 (high, phase=True): 1 tick -> tick=1 (preamble bit 16)
        # Notch 11 (low, phase=False): 1 tick -> tick=0 (preamble bit 17)
        # Notch 12 (high, phase=True): 1 tick -> tick=1 (preamble bit 18)
        # This gives tick values: 0,1,0,1,1,0,0,0,1,1,1,0,0,0,1,0,1,0,1
        # The shift register will match PREAMBLE_MASTER after 19 ticks.

        # Lead-in: low for 1 tick (creates the falling edge the decoder waits for)
        self.builder.set_level(self.ch, 0, self.tick_samples)

        # Now generate the preamble as alternating notches
        # Preamble bits: 1,0,1,1,0,0,0,1,1,1,0,0,0,1,0,1,0,1
        # Notch pattern (alternating high/low, starting with high after lead-in low):
        # High: 1 tick (bit 1=1)
        # Low:  1 tick (bit 2=0)
        # High: 2 ticks (bits 3-4=1,1)
        # Low:  3 ticks (bits 5-7=0,0,0)
        # High: 3 ticks (bits 8-10=1,1,1)
        # Low:  3 ticks (bits 11-13=0,0,0)
        # High: 1 tick (bit 14=1)
        # Low:  1 tick (bit 15=0)
        # High: 1 tick (bit 16=1)
        # Low:  1 tick (bit 17=0)
        # High: 1 tick (bit 18=1)
        notch_durations = [
            (1, 1),   # high 1 tick
            (0, 1),   # low 1 tick
            (1, 2),   # high 2 ticks
            (0, 3),   # low 3 ticks
            (1, 3),   # high 3 ticks
            (0, 3),   # low 3 ticks
            (1, 1),   # high 1 tick
            (0, 1),   # low 1 tick
            (1, 1),   # high 1 tick
            (0, 1),   # low 1 tick
            (1, 1),   # high 1 tick
        ]
        for level, ticks in notch_durations:
            self.builder.set_level(self.ch, level, self.tick_samples * ticks)

        # Data: F-code (4 bits) + Address (12 bits) = 16 bits, MSB first
        data_bits = []
        for i in range(3, -1, -1):
            data_bits.append((f_code >> i) & 1)
        for i in range(11, -1, -1):
            data_bits.append((address >> i) & 1)

        # Compute CRC check sequence
        check_bits = self._crc8_mvb(data_bits)

        # Send data bits Manchester encoded
        for bit in data_bits:
            self._manchester_bit(bit)

        # Send check sequence (8 bits) Manchester encoded
        for bit in check_bits:
            self._manchester_bit(bit)

        # Idle gap after frame (long high = end of frame marker)
        self.builder.set_level(self.ch, 1, self.tick_samples * 20)

    def generate_testdata(self):
        """Generate test data: a master frame with F-code=0, Address=1."""
        self.send_master_frame(f_code=0, address=1)
