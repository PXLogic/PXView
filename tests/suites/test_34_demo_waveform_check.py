"""
test_34_demo_waveform_check.py - demo pattern bit-level waveform cross-check.

Verifies the demo driver's PATTERN_MIXED waveform against the generation
intent using PURE PYTHON reference decoders on raw `get_samples` data —
independent of any C decoder. This is the regression guard for the demo
generator timing rework (spec: fix-demo-pattern-bus-timing).

Channel layout (PATTERN_MIXED):
  ch0-1: I2C  (SCL, SDA)
  ch2-5: SPI  (CS, SCLK, MOSI, MISO)
  ch6:   UART (RX)

Post-fix generation intent (per libsigrok/src/hardware/demo/protocol.c):
  I2C  frame = 42 bit-times @ I2C_SPB=50:  [idle,idle] START
               ADDR(0x50+W) ACK D0 ACK D1 ACK D2 ACK STOP [idle,idle]
               D0 = frame_num & 0xFF, D1 = 0x10+(n&0xF), D2 = 0x20+(n&0xF)
  SPI  frame = 52 bit-times @ SPI_SPB=40:  12 idle (CS high) + 40 data
               MOSI = [0x03, 0x00, 0x00, n&0xFF, 0xFF]
               MISO = [0xFF, 0xFF, 0xFF, 0xFF, 0x10+(n&0xF)]
  UART frame = 12 bit-times @ UART_SPB=80: 2 leading mark + start + 8 data
               (LSB first) + stop + 2 mark idle; byte = (n ^ 0xAA) & 0xFF
"""

import pytest

from helpers.capture_helper import do_buffer_capture_with_pattern

pytestmark = pytest.mark.p1

SAMPLE_RATE = 1_000_000
SAMPLE_COUNT = 100_000

CH_I2C_SCL, CH_I2C_SDA = 0, 1
CH_SPI_CS, CH_SPI_SCLK, CH_SPI_MOSI, CH_SPI_MISO = 2, 3, 4, 5
CH_UART_RX = 6


# ======================================================================
# Reference decoders (pure Python, no C decoder involvement)
# ======================================================================

def _bits_to_bytes_msb(bits):
    """Pack a bit list into bytes, MSB first per byte."""
    out = bytearray()
    for i in range(0, len(bits) - len(bits) % 8, 8):
        v = 0
        for b in bits[i:i + 8]:
            v = (v << 1) | (1 if b else 0)
        out.append(v)
    return bytes(out)


def decode_spi_mode0(cs, sclk, mosi, miso):
    """SPI mode 0: sample MOSI/MISO on each SCLK rising edge while CS low.

    Returns list of (mosi_bytes, miso_bytes), one entry per CS assertion.
    """
    frames = []
    i = 1
    n = len(cs)
    while i < n:
        if cs[i - 1] == 1 and cs[i] == 0:  # CS falling edge
            o_bits, i_bits = [], []
            j = i + 1
            while j < n and cs[j] == 0:
                if sclk[j - 1] == 0 and sclk[j] == 1:  # rising edge
                    o_bits.append(1 if mosi[j] else 0)
                    i_bits.append(1 if miso[j] else 0)
                j += 1
            if len(o_bits) >= 8:
                frames.append((_bits_to_bytes_msb(o_bits),
                               _bits_to_bytes_msb(i_bits)))
            i = j
        else:
            i += 1
    return frames


def decode_uart(rx):
    """UART: auto-measure bit time from min edge distance, midpoint sampling.

    Returns (frames, spb) where frames = list of (start_pos, byte, stop_ok),
    data bits LSB first, 1 stop bit. Resyncs on each falling edge.
    """
    n = len(rx)
    edges = [i for i in range(1, n) if (rx[i - 1] > 0) != (rx[i] > 0)]
    if len(edges) < 2:
        return [], 0
    spb = min(b - a for a, b in zip(edges, edges[1:]))

    frames = []
    i = 1
    while i < n:
        if rx[i - 1] > 0 and rx[i] == 0:  # start edge
            mid = i + spb + spb // 2      # centre of data bit 0
            stop_pos = i + 9 * spb + spb // 2
            if stop_pos >= n:
                break
            val = 0
            for k in range(8):
                val |= (1 if rx[mid + k * spb] > 0 else 0) << k
            stop_ok = rx[stop_pos] > 0
            frames.append((i, val, stop_ok))
            i = stop_pos  # resume scanning after stop bit
        else:
            i += 1
    return frames, spb


def decode_i2c(scl, sda):
    """I2C: START/STOP = SDA edge while SCL high; data on SCL rising edge.

    Returns list of transactions; each = dict(addr, rw, acks, data_bytes)
    with data_bytes decoded from 9-bit groups (8 data + 1 ack).
    Returns [] if the stream has no valid START (bus never idle first).
    """
    n = len(scl)
    # R3 guard: without an idle prefix the first SCL-high SDA edge is not a
    # legitimate START (pre-fix demo drops it), so no transaction forms.
    if not (scl[0] == 1 and sda[0] == 1):
        return []
    transactions = []
    in_txn = False
    bits = []

    def flush():
        nonlocal bits
        if in_txn and bits:
            txn = {"addr": None, "rw": None, "data_bytes": [], "acks": []}
            groups = []
            for g in range(0, len(bits) - len(bits) % 9, 9):
                groups.append(bits[g:g + 9])
            if groups:
                first = groups[0]
                addr = 0
                for b in first[:7]:
                    addr = (addr << 1) | b
                txn["addr"] = addr
                txn["rw"] = first[7]
                txn["acks"].append(first[8])
                for g in groups[1:]:
                    v = 0
                    for b in g[:8]:
                        v = (v << 1) | b
                    txn["data_bytes"].append(v)
                    txn["acks"].append(g[8])
            transactions.append(txn)
        bits = []

    for i in range(1, n):
        scl_rising = scl[i - 1] == 0 and scl[i] == 1
        sda_falling = sda[i - 1] == 1 and sda[i] == 0
        sda_rising = sda[i - 1] == 0 and sda[i] == 1
        if scl[i] == 1 or scl[i - 1] == 1:
            # SCL-high context: START / STOP conditions
            if sda_falling and scl[i] == 1 and scl[i - 1] == 1:
                flush()  # spurious mid-txn edge also restarts cleanly
                in_txn = True
                continue
            if sda_rising and scl[i] == 1 and scl[i - 1] == 1 and in_txn:
                flush()
                in_txn = False
                continue
        if scl_rising and in_txn:
            bits.append(1 if sda[i] else 0)
    return transactions


def i2c_sda_violations_while_scl_high(scl, sda):
    """R2 invariant: inside a transaction (START..STOP), SDA may only
    change while SCL is LOW. Any SDA edge with SCL high at both samples
    is a violation (spurious START/STOP or missing hold time)."""
    n = len(scl)
    in_txn = False
    bad = []
    for i in range(1, n):
        sda_edge = (sda[i - 1] > 0) != (sda[i] > 0)
        if sda_edge and scl[i] == 1 and scl[i - 1] == 1:
            falling = sda[i] == 0
            if not in_txn and falling:
                in_txn = True  # legitimate START
                continue
            if in_txn and not falling:
                in_txn = False  # legitimate STOP
                continue
            bad.append(i)  # SDA change while SCL high inside transaction
            if falling:
                in_txn = True
        elif sda_edge and in_txn:
            in_txn = True  # data-phase change, SCL low — fine
    return bad


# ======================================================================
# Fixture: capture PATTERN_MIXED and fetch raw channel bytes
# ======================================================================

@pytest.fixture(scope="module")
def mixed_channels(mcp, device_id):
    """Capture PATTERN_MIXED once and yield per-channel sample bytes."""
    try:
        mcp.connect_device(device_id)
    except Exception:
        pass  # already connected
    do_buffer_capture_with_pattern(
        mcp, device_id,
        channels=[0, 1, 2, 3, 4, 5, 6],
        sample_rate=SAMPLE_RATE,
        sample_count=SAMPLE_COUNT,
        pattern="mixed",
    )
    chans = {}
    for ch in range(7):
        raw = mcp.get_samples(
            channel_index=ch, channel_type="logic",
            start_sample=0, end_sample=SAMPLE_COUNT,
        )
        assert raw is not None and len(raw) > 0, f"ch{ch}: no samples"
        chans[ch] = bytes(1 if b else 0 for b in raw)
    yield chans


# ======================================================================
# Tests
# ======================================================================

class TestSpiWaveform:
    def test_spi_mosi_miso_frame_exact(self, mixed_channels):
        """Frame 0 present from stream start; MOSI/MISO bytes byte-exact."""
        cs = mixed_channels[CH_SPI_CS]
        sclk = mixed_channels[CH_SPI_SCLK]
        mosi = mixed_channels[CH_SPI_MOSI]
        miso = mixed_channels[CH_SPI_MISO]

        assert cs[0] == 1, "stream must start idle (CS high, R3)"
        frames = decode_spi_mode0(cs, sclk, mosi, miso)
        assert len(frames) >= 5, \
            f"only {len(frames)} SPI frames decoded in 100k samples"

        for n, (mo, mi) in enumerate(frames[:3]):
            expect_o = bytes([0x03, 0x00, 0x00, n & 0xFF, 0xFF])
            expect_i = bytes([0xFF, 0xFF, 0xFF, 0xFF, 0x10 + (n & 0x0F)])
            assert mo[:5] == expect_o, \
                f"frame {n}: MOSI {mo[:5].hex()} != {expect_o.hex()}"
            assert mi[:5] == expect_i, \
                f"frame {n}: MISO {mi[:5].hex()} != {expect_i.hex()}"

    def test_spi_bit_time_independent(self, mixed_channels):
        """R1: SPI bit time must differ from I2C/UART (independent SPB)."""
        sclk = mixed_channels[CH_SPI_SCLK]
        edges = [i for i in range(1, len(sclk)) if sclk[i] != sclk[i - 1]]
        # Measure the SCLK half-period from rising→falling pairs
        half_periods = [b - a for a, b in zip(edges[::2], edges[1::2])
                        if sclk[a] == 1]
        assert half_periods, "no SCLK pulses found"
        spi_half = min(half_periods)
        # SPI_SPB=40 → half period 20; I2C_SPB=50 → 25. Assert SPI<25.
        assert spi_half < 25, \
            f"SPI half-period {spi_half} suggests shared I2C timebase"


class TestUartWaveform:
    def test_uart_bytes_exact_from_first_frame(self, mixed_channels):
        """First decoded byte must be 0xAA (frame 0), sequence (n^0xAA)."""
        rx = mixed_channels[CH_UART_RX]
        assert rx[0] == 1, "stream must start at mark level (R3)"

        frames, spb = decode_uart(rx)
        assert spb >= 40, f"UART bit time {spb} too coarse (R1, UART_SPB=80)"
        assert len(frames) >= 10, \
            f"only {len(frames)} UART frames decoded in 100k samples"

        for k, (pos, val, stop_ok) in enumerate(frames[:8]):
            expect = (k ^ 0xAA) & 0xFF
            assert val == expect, \
                f"frame {k} @sample {pos}: got 0x{val:02X}, expected 0x{expect:02X}"
            assert stop_ok, f"frame {k} @sample {pos}: stop bit not high"


class TestI2cWaveform:
    def test_i2c_transactions_exact(self, mixed_channels):
        """Frame 0: addr 0x50 write, D0=n, D1=0x10+n, D2=0x20+n, ACKs low."""
        scl = mixed_channels[CH_I2C_SCL]
        sda = mixed_channels[CH_I2C_SDA]
        assert scl[0] == 1 and sda[0] == 1, \
            "stream must start bus-idle (R3)"

        txns = decode_i2c(scl, sda)
        assert len(txns) >= 5, \
            f"only {len(txns)} I2C transactions decoded in 100k samples"

        for n, txn in enumerate(txns[:3]):
            assert txn["addr"] == 0x50, \
                f"txn {n}: addr 0x{txn['addr']:02X} != 0x50"
            assert txn["rw"] == 0, f"txn {n}: unexpected read flag"
            data = txn["data_bytes"]
            assert len(data) >= 3, f"txn {n}: only {len(data)} data bytes"
            assert data[0] == (n & 0xFF), \
                f"txn {n}: D0 0x{data[0]:02X} != 0x{n & 0xFF:02X}"
            assert data[1] == 0x10 + (n & 0x0F), \
                f"txn {n}: D1 0x{data[1]:02X} != 0x{0x10 + (n & 0x0F):02X}"
            assert data[2] == 0x20 + (n & 0x0F), \
                f"txn {n}: D2 0x{data[2]:02X} != 0x{0x20 + (n & 0x0F):02X}"
            assert all(a == 0 for a in txn["acks"]), \
                f"txn {n}: non-ACK bits {txn['acks']}"

    def test_i2c_sda_stable_while_scl_high(self, mixed_channels):
        """R2: inside a transaction SDA only changes while SCL is low."""
        scl = mixed_channels[CH_I2C_SCL]
        sda = mixed_channels[CH_I2C_SDA]
        bad = i2c_sda_violations_while_scl_high(scl, sda)
        assert not bad, \
            f"{len(bad)} SDA transitions while SCL high inside txn " \
            f"(first at sample {bad[0]})"
