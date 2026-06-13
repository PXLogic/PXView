import math
from .base import *

class MipiRffeGenerator:
    """MIPI RFFE protocol generator (2 channels: SCLK=ch0, SDATA=ch1).

    SSC (Sequence Start Condition) as detected by the C decoder:
      1. SCLK low + SDATA rising edge  (trigger step 1)
      2. SCLK low + SDATA falling edge (trigger step 2, condition 1)
      3. SCLK rising edge              (trigger step 3, condition 1 -> SSC found)

    Data bits are read by the decoder on SCLK falling edge (SDATA sampled),
    then SCLK rising edge is waited for before processing.

    Important: The C decoder's rffe_handle() consumes an extra SCLK falling edge
    at the end of each field (after the last bit's rising edge). This extra edge
    must be provided in the waveform between fields. The rffe_handle_CMD() does
    NOT consume this extra edge.

    Parity is odd parity: the parity bit makes the total number of 1s odd.
    For the first parity in a frame (Pcount==1), the decoder adds an
    add_val adjustment based on command type before computing parity.
    """
    def __init__(self, builder, channels_map=None, samplerate=1000000):
        self.builder = builder
        self.samplerate = samplerate
        if isinstance(channels_map, dict):
            # New-style: (builder, channels_map=dict, samplerate=int)
            self.scl = channels_map.get("sclk", 0)
            self.sda = channels_map.get("sdata", 1)
            self.bit_width = max(4, int(samplerate / 1000000))
        elif isinstance(channels_map, int):
            # Legacy: (builder, scl_ch, sda_ch, ...)
            self.scl = channels_map
            self.sda = samplerate
            self.samplerate = 1000000
            self.bit_width = 2
        else:
            self.scl = 0
            self.sda = 1
            self.bit_width = max(4, int(samplerate / 1000000))
        self.half = self.bit_width // 2
        self.builder.set_idle(self.scl, 1)  # SCLK idle high
        self.builder.set_idle(self.sda, 1)  # SDATA idle high

    def _start_condition(self):
        """Generate SSC matching the C decoder's detection sequence:
        1. SCLK low + SDATA rising  -> decoder step 1 trigger
        2. SCLK low + SDATA falling -> decoder step 2, matched condition 1
        3. SCLK rising              -> decoder step 3, matched condition 1 -> SSC found
        """
        h = self.half
        # Idle: SCLK high, SDATA low (setup before SSC)
        self.builder.write_channels({self.scl: 1, self.sda: 0}, h)
        # SCLK goes low, SDATA still low
        self.builder.write_channels({self.scl: 0, self.sda: 0}, h)
        # SDATA rises while SCLK is low -> step 1 trigger (SCLK low + SDATA rising)
        self.builder.write_channels({self.scl: 0, self.sda: 1}, h)
        # SDATA falls while SCLK is low -> step 2 trigger (SCLK low + SDATA falling)
        self.builder.write_channels({self.scl: 0, self.sda: 0}, h)
        # SCLK rises -> step 3 trigger (SCLK rising) -> SSC detected
        self.builder.write_channels({self.scl: 1, self.sda: 0}, h)

    def _bus_park(self):
        """Bus Park: SCLK low + SDATA low, then both return high (idle).
        The decoder waits for SCLK low + SDATA low to detect bus park."""
        h = self.half
        # Both go low (decoder detects bus park on SCLK low + SDATA low)
        self.builder.write_channels({self.scl: 0, self.sda: 0}, h)
        # Return to idle (SCLK high, SDATA high)
        self.builder.write_channels({self.scl: 1, self.sda: 1}, h)

    def _write_bit(self, bit):
        """Write one bit: SDATA set before SCLK falling edge.
        The decoder reads SDATA on SCLK falling edge, then waits for SCLK rising."""
        h = self.half
        # SCLK high, SDATA = bit value (setup before falling edge)
        self.builder.write_channels({self.scl: 1, self.sda: bit}, h)
        # SCLK falls (decoder samples SDATA here)
        self.builder.write_channels({self.scl: 0, self.sda: bit}, h)
        # SCLK rises (decoder waits for this after reading data)
        self.builder.write_channels({self.scl: 1, self.sda: bit}, h)

    def _field_separator(self):
        """Provide an extra SCLK falling+rising edge consumed by rffe_handle()
        at the end of each field. After reading the last bit of a field, the
        C decoder waits for one more SCLK falling edge before processing the
        field and transitioning to the next state. SDATA value is irrelevant."""
        h = self.half
        # SCLK falls (extra falling edge consumed by rffe_handle)
        self.builder.write_channels({self.scl: 0, self.sda: 0}, h)
        # SCLK rises back (so next field can start cleanly with SCLK high)
        self.builder.write_channels({self.scl: 1, self.sda: 0}, h)

    def _compute_parity_data(self, data_value, add_val=0, key=0):
        """Compute the parity bit the same way the decoder does.
        The decoder starts with parity=1 and toggles for each set bit in
        (Pdata + add_val * (1 << (key+1))).
        Result: parity_bit = 1 if adjusted value has even number of 1s, 0 if odd.
        This implements odd parity: total 1s (data + parity_bit) is odd."""
        adjusted = data_value + (add_val * (1 << (key + 1)))
        parity = 1
        tmp = adjusted
        while tmp:
            parity = 1 - parity
            tmp = tmp & (tmp - 1)
        return parity

    def send_r0w(self, slave_addr=0x0, data=0x00):
        """Send Register 0 Write command.
        R0W: SSC + SA(4bit) + cmd_bit0=1 + DATA(7bit) + Parity(1bit) + BusPark.
        The decoder detects R0W when cmd_bit0=1 after SA.
        Parity for R0W data frame: odd parity over (data | 0x80)."""
        self._start_condition()
        # SA: 4 bits MSB first
        for i in range(3, -1, -1):
            self._write_bit((slave_addr >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of SA
        # Command bit 0 = 1 (indicates R0W)
        # rffe_handle_CMD reads this on SCLK falling edge, no extra edge needed
        self._write_bit(1)
        # DATA: 7 bits MSB first (register 0 write data)
        data_7bit = data & 0x7F
        for i in range(6, -1, -1):
            self._write_bit((data_7bit >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of DATA
        # Parity: for R0W, add_val=1, key=6 -> adjusted = data_7bit + (1 << 7) = data_7bit | 0x80
        parity_bit = self._compute_parity_data(data_7bit, add_val=1, key=6)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # Bus Park
        self._bus_park()

    def send_rw(self, slave_addr=0x0, address=0x00, data=0x00):
        """Send Register Write command.
        RW: SSC + SA(4bit) + cmd_bits[0,1,0] + ADDRESS(5bit) + Parity + DATA(8bit) + Parity + BusPark.
        Command encoding: bit0=0, bit1=1 (basic), bit2=0 (write) -> RW.
        Address parity: odd parity over (address | 0x40) [add_val=2, key=4].
        Data parity: odd parity over data_value [no add_val for Pcount>1]."""
        self._start_condition()
        # SA: 4 bits MSB first
        for i in range(3, -1, -1):
            self._write_bit((slave_addr >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of SA
        # Command bits: bit0=0, bit1=1 (basic), bit2=0 (write)
        # rffe_handle_CMD reads these, no extra edge needed between cmd bits
        self._write_bit(0)  # bit0: not R0W
        self._write_bit(1)  # bit1: basic (sdata=1 -> extended=0)
        self._write_bit(0)  # bit2: write (sdata=0 when basic -> RW)
        # ADDRESS: 5 bits MSB first
        addr_5bit = address & 0x1F
        for i in range(4, -1, -1):
            self._write_bit((addr_5bit >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of ADDRESS
        # Parity over address (Pcount=1, add_val=2 for RW, key=4)
        parity_bit = self._compute_parity_data(addr_5bit, add_val=2, key=4)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # DATA: 8 bits MSB first
        for i in range(7, -1, -1):
            self._write_bit((data >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of DATA
        # Parity over data (Pcount=2, no add_val adjustment)
        parity_bit = self._compute_parity_data(data)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # Bus Park
        self._bus_park()

    def send_rr(self, slave_addr=0x0, address=0x00, read_data=0x00):
        """Send Register Read command.
        RR: SSC + SA(4bit) + cmd_bits[0,1,1] + ADDRESS(5bit) + Parity + BusPark
            + DATA(8bit) + Parity + BusPark.
        Command encoding: bit0=0, bit1=1 (basic), bit2=1 (read) -> RR.
        After first parity+bus_park, slave drives data on the bus."""
        self._start_condition()
        # SA: 4 bits MSB first
        for i in range(3, -1, -1):
            self._write_bit((slave_addr >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of SA
        # Command bits: bit0=0, bit1=1 (basic), bit2=1 (read)
        self._write_bit(0)  # bit0: not R0W
        self._write_bit(1)  # bit1: basic (sdata=1 -> extended=0)
        self._write_bit(1)  # bit2: read (sdata=1 when basic -> RR)
        # ADDRESS: 5 bits MSB first
        addr_5bit = address & 0x1F
        for i in range(4, -1, -1):
            self._write_bit((addr_5bit >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of ADDRESS
        # Parity over address (Pcount=1, add_val=3 for RR, key=4)
        parity_bit = self._compute_parity_data(addr_5bit, add_val=3, key=4)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # Bus Park (first: BPcount=1, decoder goes to FIND_DATA)
        self._bus_park()
        # DATA: 8 bits MSB first (slave drives)
        for i in range(7, -1, -1):
            self._write_bit((read_data >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of DATA
        # Parity over data (Pcount=2, no add_val adjustment)
        parity_bit = self._compute_parity_data(read_data)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # Bus Park (second: BPcount=2, decoder calls rffe_init)
        self._bus_park()

    def send_erw(self, slave_addr=0x0, address=0x00, data=0x55):
        """Send Extended Register Write command.
        ERW: SSC + SA(4bit) + cmd_bits[0,0,0,0] + BC(4bit) + Parity + ADDRESS(8bit)
             + Parity + DATA(8bit) + Parity + BusPark.
        Command encoding: bit0=0, bit1=0 (extended), bit2=0 (write), bit3=0 (short) -> ERW.
        BC=0 means 1 byte of data."""
        self._start_condition()
        # SA: 4 bits MSB first
        for i in range(3, -1, -1):
            self._write_bit((slave_addr >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of SA
        # Command bits: bit0=0, bit1=0 (extended), bit2=0 (write), bit3=0 (short)
        self._write_bit(0)  # bit0: not R0W
        self._write_bit(0)  # bit1: extended (sdata=0 -> extended=1)
        self._write_bit(0)  # bit2: isWrite=true (sdata=0 when extended -> write)
        self._write_bit(0)  # bit3: short (sdata=0 -> ERW, not long)
        # BC: 4 bits MSB first (value 0 = 1 byte, for ERW/ERR BC is 4 bits)
        bc_val = 0  # 0 means 1 byte
        for i in range(3, -1, -1):
            self._write_bit((bc_val >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of BC
        # Parity over BC (Pcount=1, add_val=0 for ERW, key=3)
        parity_bit = self._compute_parity_data(bc_val, add_val=0, key=3)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # ADDRESS: 8 bits MSB first
        for i in range(7, -1, -1):
            self._write_bit((address >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of ADDRESS
        # Parity over address (Pcount=2, no add_val)
        parity_bit = self._compute_parity_data(address)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # DATA: 8 bits MSB first
        for i in range(7, -1, -1):
            self._write_bit((data >> i) & 1)
        self._field_separator()  # extra edge consumed by rffe_handle at end of DATA
        # Parity over data (Pcount=3, no add_val)
        parity_bit = self._compute_parity_data(data)
        self._write_bit(parity_bit)
        self._field_separator()  # extra edge consumed by rffe_handle at end of Parity
        # Bus Park
        self._bus_park()

    def generate_testdata(self):
        """Generate test data with multiple MIPI RFFE command types."""
        # Register 0 Write (simplest command)
        self.send_r0w(slave_addr=0x2, data=0x1A)
        # Register Write
        self.send_rw(slave_addr=0x1, address=0x05, data=0x55)
        # Extended Register Write
        self.send_erw(slave_addr=0x0, address=0x10, data=0x34)

# Keep old name as alias for backward compatibility
MIPIRFFEGenerator = MipiRffeGenerator
