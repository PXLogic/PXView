import math
from .base import *

class FSiGenerator:
    """FSI (Flexible Service Interface) generator.
    2 channels: DATA(ch0), CLOCK(ch1).
    FSI data is electrically inverted (raw 0 = logical 1, raw 1 = logical 0).
    The decoder uses fsi_data_prev (the data value from the PREVIOUS clock edge)
    for all state machine decisions and CRC calculation.
    For master transmission, the decoder only processes rising clock edges."""

    def __init__(self, builder, data_ch=0, clk_ch=1, samplerate=1000000):
        self.builder = builder
        self.data = data_ch
        self.clk = clk_ch
        self.sr = samplerate
        self.half_period = max(2, int(samplerate / 200000))
        # Set idle states: clock low, data high (raw=1 = logical 0, idle)
        builder.set_idle(clk_ch, 0)
        builder.set_idle(data_ch, 1)

    def _clock_edge(self, data_raw_val):
        """Send one clock cycle: rising edge then falling edge.
        data_raw_val is the raw electrical value on the data line.
        Logical value = NOT data_raw_val."""
        hp = self.half_period
        self.builder.write_channels({self.clk: 1, self.data: data_raw_val}, hp)
        self.builder.write_channels({self.clk: 0, self.data: data_raw_val}, hp)

    def _send_break(self):
        """BREAK: 256+ consecutive logical 1s on rising clock edge.
        Logical 1 = raw 0 (electrically inverted)."""
        for _ in range(260):
            self._clock_edge(0)  # raw=0 means logical 1

    def _send_logical_bit(self, logical_val):
        """Send one logical bit value. logical_val=1 means raw=0."""
        self._clock_edge(0 if logical_val else 1)

    def _compute_crc4(self, bits_list):
        """Compute FSI CRC-4 using Galois LFSR polynomial 0x7.
        bits_list contains the fsi_data_prev values for each clock edge
        where crc_calculating=1."""
        crc = 0
        for bit in bits_list:
            feedback = ((crc >> 3) & 1) ^ bit
            crc = (crc & ~(1 << 0)) | ((feedback & 1) << 0)
            crc = (crc & ~(1 << 1)) | ((((crc & 1) ^ feedback) & 1) << 1)
            crc = (crc & ~(1 << 2)) | (((((crc >> 1) & 1) ^ feedback) & 1) << 2)
            crc = (crc & ~(1 << 3)) | ((((crc >> 2) & 1) & 1) << 3)
        return crc & 0xF

    def send_abs_adr_write(self, slave_id=0, address=0, data_byte=0x55):
        """Send a complete ABS_ADR write transaction.
        The decoder uses fsi_data_prev (previous edge's data value).
        This means the data we send on edge N is processed as fsi_data_prev
        on edge N+1. We need to account for this one-edge delay."""

        # BREAK: 256+ logical 1s
        self._send_break()

        # BREAK_TAR: 4+ clock cycles of logical 0 (raw=1)
        # The decoder counts tar_timer on each edge (both rising and falling)
        # tar_timer > tar_cycles(3) completes BREAK_TAR
        for _ in range(5):
            self._clock_edge(1)  # raw=1 means logical 0

        # After BREAK_TAR, decoder enters IDLE.
        # In IDLE, on rising clock edge: if fsi_data_prev == 1 -> START
        # fsi_data_prev is set from the PREVIOUS edge's data value.
        # The last BREAK_TAR edge had logical 0, so fsi_data_prev = 0.
        # We need to send a rising edge with logical 1 to set fsi_data_prev=1,
        # then the NEXT rising edge will see fsi_data_prev=1 and detect START.

        # Send one rising edge with logical 1 (raw=0)
        self._send_logical_bit(1)
        # Now fsi_data_prev = 1 (set at end of this edge processing)

        # Next rising edge: fsi_data_prev=1 -> START detected!
        # The data value on THIS edge becomes the first fsi_data_prev
        # for TX_SLAVE_ID processing.
        # TX_SLAVE_ID: 2 bits, tx_slave_id = (tx_slave_id >> 1) | (fsi_data_prev << 1)
        # For slave_id=0: both fsi_data_prev values should be 0
        # First TX_SLAVE_ID bit: fsi_data_prev = data from START edge
        # We want fsi_data_prev=0, so the START edge data should be logical 0
        self._send_logical_bit(0)  # This triggers START; data=0 for first slave ID bit

        # Second TX_SLAVE_ID bit: fsi_data_prev = data from previous edge = 0
        self._send_logical_bit(0)  # slave_id bit 1: fsi_data_prev=0

        # COMMAND: ABS_ADR = 100 (3 bits, MSB first)
        # command_code = (command_code << 1) | fsi_data_prev
        # For 100: fsi_data_prev sequence = 1, 0, 0
        self._send_logical_bit(1)  # fsi_data_prev=1 for command bit 0
        self._send_logical_bit(0)  # fsi_data_prev=0 for command bit 1
        self._send_logical_bit(0)  # fsi_data_prev=0 for command bit 2

        # DIRECTION: 0 = Write
        # direction = fsi_data_prev
        self._send_logical_bit(0)  # fsi_data_prev=0 -> Write

        # ADDRESS: 21 bits MSB first
        # address = (address << 1) | fsi_data_prev
        addr_bits = []
        for i in range(20, -1, -1):
            bit = (address >> i) & 1
            addr_bits.append(bit)
            self._send_logical_bit(bit)

        # DATA_SIZE: 0 = BYTE (when address_raw & 3 != 3)
        # data_size determined by fsi_data_prev
        self._send_logical_bit(0)  # fsi_data_prev=0 -> BYTE

        # TX_DATA: 8 bits MSB first
        # data = (data << 1) | fsi_data_prev
        data_bits = []
        for i in range(7, -1, -1):
            bit = (data_byte >> i) & 1
            data_bits.append(bit)
            self._send_logical_bit(bit)

        # CRC: 4 bits
        # CRC is computed over fsi_data_prev values where crc_calculating=1
        # crc_calculating=1 during: TX_SLAVE_ID, COMMAND, DIRECTION, ADDRESS, DATA_SIZE, TX_DATA
        # The CRC bits sent should match the computed CRC.
        #
        # Collect all fsi_data_prev values for CRC calculation.
        # The decoder processes fsi_data_prev on each rising clock edge.
        # For master transmission, only rising edges are processed.
        #
        # fsi_data_prev values (in order of rising edges where crc_calculating=1):
        # START edge data (becomes fsi_data_prev for first TX_SLAVE_ID edge)
        # But wait - START detection happens when fsi_data_prev=1, and at that
        # point crc_calculating is set to 1. The fsi_data_prev=1 from the START
        # detection edge IS included in the CRC? Let me re-check...
        #
        # In the decoder:
        # IDLE: if fsi_data_prev == 1:
        #   crc_calculating = 1  (set here)
        #   state = TX_SLAVE_ID
        # Then at the END of the iteration: CRC is updated with fsi_data_prev
        #
        # So the fsi_data_prev=1 that triggered START IS included in CRC!
        #
        # Wait, let me trace more carefully:
        # In IDLE state, on a rising edge where fsi_data_prev=1:
        #   1. crc_internal = 0 (reset in IDLE entry)
        #   2. crc_calculating = 1 (set when START detected)
        #   3. putb([1, ['START']])
        #   4. state = TX_SLAVE_ID
        #   5. At end of iteration: CRC update with fsi_data_prev
        #      crc_feedback = ((crc_prev >> 3) & 1) ^ fsi_data_prev
        #      fsi_data_prev = 1 -> this IS included in CRC
        #
        # Hmm, but the CRC update happens at the END of the decode() iteration,
        # after all state machine processing. So the fsi_data_prev value that
        # triggered START (which is 1) IS fed into the CRC.
        #
        # Let me collect ALL fsi_data_prev values in order:
        # 1. START detection: fsi_data_prev = 1 (from the edge before START)
        # 2. First TX_SLAVE_ID edge: fsi_data_prev = data from START edge = 0
        # 3. Second TX_SLAVE_ID edge: fsi_data_prev = 0
        # 4-6. COMMAND edges: fsi_data_prev = 1, 0, 0
        # 7. DIRECTION edge: fsi_data_prev = 0
        # 8-28. ADDRESS edges: fsi_data_prev = addr_bits[0..20]
        # 29. DATA_SIZE edge: fsi_data_prev = 0
        # 30-37. TX_DATA edges: fsi_data_prev = data_bits[0..7]
        #
        # Wait, I need to be more careful. The fsi_data_prev on each rising
        # edge is the data value from the PREVIOUS rising edge.
        #
        # Let me trace the rising edges and their fsi_data_prev values:
        #
        # Edge 0 (BREAK edge): fsi_data_prev = 0 (initial), data = logical 1
        #   -> fsi_data_prev becomes 1
        # ... (many BREAK edges)
        # Edge N (last BREAK edge): fsi_data_prev = 1, data = logical 1
        #   -> fsi_data_prev becomes 1
        #
        # BREAK_TAR edges: fsi_data_prev = 1, data = logical 0
        #   -> fsi_data_prev becomes 0
        # ... more BREAK_TAR edges
        #
        # Edge M (first edge after BREAK_TAR, logical 1):
        #   fsi_data_prev = 0 (from last BREAK_TAR edge)
        #   In IDLE: fsi_data_prev != 1, so no START
        #   -> fsi_data_prev becomes 1
        #
        # Edge M+1 (START edge, logical 0):
        #   fsi_data_prev = 1 (from edge M)
        #   In IDLE: fsi_data_prev == 1 -> START!
        #   crc_calculating = 1
        #   CRC update: feedback = (0 ^ 1) = 1, crc updated
        #   -> fsi_data_prev becomes 0
        #
        # Edge M+2 (first TX_SLAVE_ID edge, logical 0):
        #   fsi_data_prev = 0 (from START edge)
        #   TX_SLAVE_ID: tx_slave_id = (0 >> 1) | (0 << 1) = 0
        #   CRC update with fsi_data_prev = 0
        #   -> fsi_data_prev becomes 0
        #
        # Edge M+3 (second TX_SLAVE_ID edge, logical 0):
        #   fsi_data_prev = 0
        #   TX_SLAVE_ID: tx_slave_id = (0 >> 1) | (0 << 1) = 0, count=0 -> done
        #   CRC update with fsi_data_prev = 0
        #   -> fsi_data_prev becomes 0
        #
        # So the CRC input values (fsi_data_prev on each rising edge where
        # crc_calculating=1) are:
        # 1 (START), 0, 0, 1, 0, 0, 0, addr_bits..., 0, data_bits...
        #
        # But wait - the CRC is also updated on falling edges during BREAK_TAR!
        # No, for master transmission, the decoder skips falling edges:
        #   if (not fsi_clk): continue  (line 140-141)
        # So only rising edges are processed during master transmission.
        #
        # Actually, during BREAK_TAR, the state machine processes ALL edges
        # (both rising and falling) because BREAK_TAR is not in the list of
        # states that skip edges based on clock direction. Let me re-check...
        #
        # Lines 132-141:
        # if ((self.state == 'TAR') or (self.state == 'RX_SLAVE_ID') or ...):
        #     # Slave transmitting, sample on falling edge
        #     if (fsi_clk): continue
        # else:
        #     # Master transmitting, sample on rising edge
        #     if (not fsi_clk): continue
        #
        # BREAK_TAR is NOT in the slave list, so it falls into the else branch:
        # master transmitting, sample on rising edge only.
        # So during BREAK_TAR, only rising edges are processed.
        # And crc_calculating = 0 during BREAK_TAR (set to 0 at line 391).
        #
        # OK so the CRC input values are:
        # On the START detection edge: fsi_data_prev = 1
        # Then on each subsequent rising edge where crc_calculating=1:
        # The fsi_data_prev values from the data we sent.

        # Let me collect the CRC bits properly.
        # The CRC is computed over fsi_data_prev values on rising edges
        # where crc_calculating=1, starting from the START detection edge.

        # fsi_data_prev values in order:
        # 1. START edge: fsi_data_prev = 1 (this triggered START)
        # 2. First TX_SLAVE_ID: fsi_data_prev = 0 (data from START edge)
        # 3. Second TX_SLAVE_ID: fsi_data_prev = 0
        # 4. COMMAND bit 0: fsi_data_prev = 1
        # 5. COMMAND bit 1: fsi_data_prev = 0
        # 6. COMMAND bit 2: fsi_data_prev = 0
        # 7. DIRECTION: fsi_data_prev = 0
        # 8-28. ADDRESS (21 bits): fsi_data_prev = addr_bits
        # 29. DATA_SIZE: fsi_data_prev = 0
        # 30-37. TX_DATA (8 bits): fsi_data_prev = data_bits

        crc_bits = [1, 0, 0, 1, 0, 0, 0] + addr_bits + [0] + data_bits
        crc = self._compute_crc4(crc_bits)

        for i in range(3, -1, -1):
            self._send_logical_bit((crc >> i) & 1)

        # TAR: 4+ clock cycles of logical 0
        for _ in range(5):
            self._send_logical_bit(0)

    def send_frame(self, cmd, data_val):
        """Legacy method - sends ABS_ADR write."""
        self.send_abs_adr_write(slave_id=0, address=0, data_byte=data_val)


# P1 dynamic dispatch alias: fsi_c -> FsiGenerator
FsiGenerator = FSiGenerator
