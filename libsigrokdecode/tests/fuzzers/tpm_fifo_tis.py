import math
from .base import *


class TpmFifoTisGenerator:
    """TPM FIFO TIS stack decoder fuzzer.

    TPM FIFO TIS is a stack decoder (input: tpm-tis). This fuzzer generates
    the root-level SPI waveform that, when decoded through spi_c -> tpm_tis_spi_c,
    produces TPM TIS transactions that trigger the tpm_fifo_tis_c decoder.

    The tpm_fifo_tis_c decoder requires:
    - cmd == "TRANSACTION" with >= 7 fields
    - addr (4 bytes LE), reading (1 byte), xfer_len (2 bytes LE), xfer_data...

    We generate SPI transactions that represent a TPM command sequence:
    1. Write to TPM_STS (0xD40018) with commandReady=0x40 to go to READY state
    2. Write to TPM_DATA_FIFO (0xD40024) with a Startup command
    3. Write to TPM_STS with tpmGo=0x20 to start execution
    """

    def __init__(self, builder, channels_map, samplerate):
        self.builder = builder
        self.channels_map = channels_map
        self.samplerate = samplerate
        self.clk = channels_map.get('clk', 0)
        self.mosi = channels_map.get('mosi', 1)
        self.miso = channels_map.get('miso', 2)
        self.cs = channels_map.get('cs', 3)
        self.half_period = max(2, samplerate // 2000000)

    def _spi_select(self):
        """Assert CS (active low)."""
        self.builder.set_level(self.cs, 0, 0)

    def _spi_deselect(self):
        """Deassert CS (high)."""
        self.builder.set_level(self.cs, 1, 0)

    def _spi_write_byte(self, byte_val):
        """Write one byte over SPI: MOSI on CLK rising edge, MSB first."""
        hp = self.half_period
        for i in range(7, -1, -1):
            bit = (byte_val >> i) & 1
            # Set MOSI while CLK is low
            self.builder.set_level(self.mosi, bit, 0)
            self.builder.set_level(self.clk, 1, hp)  # Rising edge: data sampled
            self.builder.set_level(self.clk, 0, hp)   # Falling edge

    def _spi_transaction(self, write_data):
        """Perform a full SPI transaction: CS low, write bytes, CS high."""
        self._spi_select()
        for b in write_data:
            self._spi_write_byte(b)
        self._spi_deselect()
        # Gap between transactions
        self.builder.set_level(self.clk, 0, self.half_period * 4)

    def generate_testdata(self):
        # Set idle states
        self.builder.set_idle(self.clk, 0)
        self.builder.set_idle(self.cs, 1)
        self.builder.set_idle(self.mosi, 0)

        # Initial idle
        self.builder.set_level(self.cs, 1, self.half_period * 10)
        self.builder.set_level(self.clk, 0, self.half_period * 10)

        # SPI TPM write format:
        # First byte: 0x00 for write, 0x80 for read (MSB indicates R/W)
        # Then 3 bytes of address (24-bit)
        # Then data bytes

        # Transaction 1: Write TPM_STS_X = 0x40 (commandReady)
        # Address: 0xD40018 (TPM_STS locality 0)
        # SPI write: 0x00 | (addr >> 16) & 0x3F, (addr >> 8) & 0xFF, addr & 0xFF, data...
        addr_sts = 0xD40018
        self._spi_transaction([
            0x00 | ((addr_sts >> 16) & 0x3F),  # Write command + high addr
            (addr_sts >> 8) & 0xFF,             # Mid addr
            addr_sts & 0xFF,                    # Low addr
            0x40,                               # commandReady bit
        ])

        # Transaction 2: Write TPM_DATA_FIFO = TPM2_Startup(SU_Clear)
        # Address: 0xD40024 (TPM_DATA_FIFO locality 0)
        # TPM2_Startup command: tag=0x8001, size=0x000C, code=0x0144, SU_Clear=0x0000
        addr_fifo = 0xD40024
        tpm_startup = bytes([
            0x80, 0x01,  # Tag: TPM_ST_NO_SESSIONS
            0x00, 0x0C,  # Size: 12 bytes
            0x01, 0x44,  # Command code: TPM_CC_Startup
            0x00, 0x00,  # Startup type: SU_CLEAR
        ])
        self._spi_transaction([
            0x00 | ((addr_fifo >> 16) & 0x3F),
            (addr_fifo >> 8) & 0xFF,
            addr_fifo & 0xFF,
        ] + list(tpm_startup))

        # Transaction 3: Write TPM_STS_X = 0x20 (tpmGo)
        self._spi_transaction([
            0x00 | ((addr_sts >> 16) & 0x3F),
            (addr_sts >> 8) & 0xFF,
            addr_sts & 0xFF,
            0x20,  # tpmGo bit
        ])

        # Transaction 4: Read TPM_STS_X to check status
        # SPI read: 0x80 | (addr >> 16) & 0x3F, then 3 dummy bytes for read data
        self._spi_select()
        self._spi_write_byte(0x80 | ((addr_sts >> 16) & 0x3F))
        self._spi_write_byte((addr_sts >> 8) & 0xFF)
        self._spi_write_byte(addr_sts & 0xFF)
        # Read 1 byte (MOSI doesn't matter for reads, but we clock through)
        self._spi_write_byte(0x00)  # Dummy byte for read data
        self._spi_deselect()
        self.builder.set_level(self.clk, 0, self.half_period * 4)
