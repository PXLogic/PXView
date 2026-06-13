#!/usr/bin/env python3
"""
test_factory.py - Robust mass generation of high-fidelity tests for 215+ decoders.
Generates valid protocol bitstream data for each C decoder's test.
"""

import os
import json
import re
from protocol_synthesizer import (
    BitstreamBuilder, UARTGenerator, I2CGenerator,
    SPIGenerator, CANGenerator, USBGenerator, ManchesterGenerator,
    PS2Generator, JTAGGenerator, MDIOGenerator, MicrowireGenerator,
    HDLCGenerator, I2SGenerator, ISO7816Generator, SWDGenerator,
    OneWireGenerator, NRZIGenerator, IRNECGenerator, IRRC5Generator,
    IRRC6Generator, IRSIRCGenerator, IRLTTOGenerator, IRRecoilGenerator,
    PWMGenerator, DCCGenerator,
    DMX512Generator, DALIGenerator, CECGenerator, SPDIFGenerator,
    WiegandGenerator, OpenthermGenerator, SENTGenerator,
    MIPIRFFEGenerator,
    Z80Generator, MCS48Generator, GPIBGenerator,
    LPCGenerator, AM230xGenerator, DCF77Generator, CaliperGenerator,
    C2Generator, AVRPDIGenerator, DeltaSigmaGenerator, EM4100Generator,
    OneSingleWireGenerator, OOKGenerator, SonyMDGenerator,
    TDMAudioGenerator, TLC5620Generator, USBPowerDeliveryGenerator,
    MillerGenerator, IEBUSGenerator, Ieee488Generator, FSiGenerator,
    EthANGenerator, QiGenerator, PXX1Generator, PJDLGenerator,
    RCEncodeGenerator, RGBLEDWS281xGenerator, RinnaiControlPanelGenerator,
    CarreraGenerator, SDQGenerator, SwimGenerator, SWIGenerator,
    MvbGenerator, MorseGenerator, LFASTGenerator, DSIGenerator,
    EM4305Generator, TL5620Generator, T55xxGenerator, BEANGenerator,
    CCDGenerator, MapleBusGenerator, RVSWDGenerator, SDIOGenerator
)

DECODERS_DIR = "../c_decoders"
TESTDATA_ROOT = "testdata"

# Decoders that should be skipped (helpers, not real PDs)
BLACKLIST = ["signalling", "polarity", "invert", "bit_offset", "HtoD_Clock"]

# Mapping of input types to their upstream decoder ID and generator class
# For stack decoders: maps input name -> (upstream_c_decoder_id, GeneratorClass_or_None)
INPUT_GENERATORS = {
    "logic": ("base", None),
    "i2c": ("i2c_c", I2CGenerator),
    "spi": ("spi_c", SPIGenerator),
    "uart": ("uart_c", UARTGenerator),
    "can": ("can_c", CANGenerator),
    "usb_signalling": ("usb_signalling_c", USBGenerator),
    "ethernet": ("ethernet_c", None),  # needs NRZI+4b5b chain
    "ps2": ("ps2_c", PS2Generator),
    "onewire_link": ("onewire_link_c", None),
    "onewire_network": ("onewire_network_c", None),
    "ipv4": ("ipv4_c", None),
    "usb_packet": ("usb_packet_c", None),
    "tmc": ("tmc_c", None),
    "jtag": ("jtag_c", JTAGGenerator),
    "nrzi": ("nrzi_c", NRZIGenerator),
    "4b5b": ("4b5b_c", None),
    "mdio": ("mdio_c", MDIOGenerator),
    "microwire": ("microwire_c", MicrowireGenerator),
    "iebus": ("iebus_c", IEBUSGenerator),
    "ir_ltto": ("ir_ltto_c", None),
    "lfast": ("lfast_c", LFASTGenerator),
    "ltar_smartdevice": ("ltar_smartdevice_c", None),
    "afsk_bits": ("afsk_c", None),
    "sony_md": ("sony_md_c", None),
    "ook": ("ook_c", OOKGenerator),
    "tpm-tis": ("tpm_fifo_tis_c", None),
    "usb_request": ("usb_request_c", None),
    "pjon_link": ("pjdl_c", None),
}

# Per-decoder protocol data generation configuration
# Maps decoder_id -> dict with: samplerate, num_channels, sample_count, generator_func
# generator_func(bb) generates protocol data on the BitstreamBuilder

def _gen_jtag(bb):
    gen = JTAGGenerator(bb, 0, 1, 2, 3)  # TDI=0, TDO=1, TCK=2, TMS=3 (matches C decoder)
    gen.reset_tap()
    gen.shift_dr(0x05)

def _gen_mdio(bb):
    gen = MDIOGenerator(bb, 0, 1)
    gen.read_clause22(phy_addr=1, reg_addr=0, data=0x1141)

def _gen_microwire(bb):
    gen = MicrowireGenerator(bb, 0, 1, 2, 3)
    gen.read(addr=0x00)

def _gen_hdlc(bb):
    gen = HDLCGenerator(bb, channel=0)
    gen.send_frame(0xFF, 0x03, b'Hello')

def _gen_i2s(bb):
    gen = I2SGenerator(bb, 0, 1, 2)
    gen.send_frame(left_data=0x1234, right_data=0x5678)

def _gen_iso7816(bb):
    gen = ISO7816Generator(bb, 0, 1)
    gen.send_atr()

def _gen_swd(bb):
    gen = SWDGenerator(bb, 0, 1)
    gen.read_dp()

def _gen_onewire(bb):
    gen = OneWireGenerator(bb, channel=0)
    gen.read_rom()

def _gen_can(bb):
    gen = CANGenerator(bb, channel=0, bitrate=500000)
    gen.send_frame(0x123, [0x11, 0x22, 0x33])

def _gen_ps2(bb):
    gen = PS2Generator(bb, clk_ch=0, data_ch=1)
    gen.write_byte(0x1C)  # 'A' scancode
    bb.pos += 1000
    gen.write_byte(0xF0)  # Break
    gen.write_byte(0x1C)

def _gen_nrzi(bb):
    gen = NRZIGenerator(bb, channel=0)
    gen.send_bytes(b'\x55\xAA\xFF')

def _gen_ethernet(bb):
    """Generate NRZI-encoded 4B5B/Ethernet frame waveform.

    Produces a complete Ethernet II frame over the NRZI -> 4B5B -> Ethernet
    decoder chain: preamble for NRZI sync, 4B5B IDLE symbols, JK start
    sequence, Ethernet preamble+SFD, MAC headers, EtherType, payload,
    FCS (CRC32), and TR end sequence.
    """
    import zlib as _zlib

    # 4B5B data encoding: nibble -> 5-bit symbol
    _DATA_ENCODE = {
        0x0: 0b11110, 0x1: 0b01001, 0x2: 0b10100, 0x3: 0b10101,
        0x4: 0b01010, 0x5: 0b01011, 0x6: 0b01110, 0x7: 0b01111,
        0x8: 0b10010, 0x9: 0b10011, 0xA: 0b10110, 0xB: 0b10111,
        0xC: 0b11010, 0xD: 0b11011, 0xE: 0b11100, 0xF: 0b11101,
    }
    _CTRL_J = 0b11000
    _CTRL_K = 0b10001
    _CTRL_T = 0b01101
    _CTRL_R = 0b00111
    _CTRL_I = 0b11111

    ch = 0
    bit_width = max(2, bb.samplerate // 100000)
    level = 0

    def send_nrzi_bit(data_bit):
        nonlocal level
        if data_bit == 1:
            level = 1 - level
        bb.set_level(ch, level, bit_width)

    def send_symbol(sym5):
        for i in range(4, -1, -1):
            send_nrzi_bit((sym5 >> i) & 1)

    def send_byte(val):
        send_symbol(_DATA_ENCODE[val & 0x0F])
        send_symbol(_DATA_ENCODE[(val >> 4) & 0x0F])

    # Brief idle before preamble
    bb.set_level(ch, 0, bit_width * 4)

    # NRZI preamble: 32 toggles for clock sync (16 rising edges)
    for _ in range(32):
        send_nrzi_bit(1)

    # 4B5B IDLE symbols
    for _ in range(5):
        send_symbol(_CTRL_I)

    # JK start sequence
    send_symbol(_CTRL_J)
    send_symbol(_CTRL_K)

    # Ethernet preamble: 7 bytes of 0x55
    for _ in range(7):
        send_byte(0x55)

    # SFD: 0xD5
    send_byte(0xD5)

    # DST MAC: FF:FF:FF:FF:FF:FF (broadcast)
    for b in [0xFF] * 6:
        send_byte(b)

    # SRC MAC: 00:11:22:33:44:55
    for b in [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]:
        send_byte(b)

    # EtherType: 0x0800 (IPv4)
    send_byte(0x08)
    send_byte(0x00)

    # Payload: 46 bytes (minimum Ethernet payload)
    payload = bytes([i & 0xFF for i in range(46)])
    for b in payload:
        send_byte(b)

    # FCS: CRC32 of frame (DST+SRC+EtherType+Payload)
    frame = bytes([0xFF] * 6 + [0x00, 0x11, 0x22, 0x33, 0x44, 0x55]
                  + [0x08, 0x00] + list(payload))
    crc = _zlib.crc32(frame) & 0xFFFFFFFF
    fcs = crc.to_bytes(4, byteorder='little')
    for b in fcs:
        send_byte(b)

    # End sequence: T + R
    send_symbol(_CTRL_T)
    send_symbol(_CTRL_R)

    # Trailing IDLE
    for _ in range(5):
        send_symbol(_CTRL_I)

def _gen_nec(bb):
    gen = IRNECGenerator(bb, channel=0)
    gen.send_nec(address=0x04, command=0x08)

def _gen_rc5(bb):
    gen = IRRC5Generator(bb, channel=0)
    gen.send_rc5(address=0x00, command=0x01)

def _gen_rc6(bb):
    gen = IRRC6Generator(bb, channel=0)
    gen.send_rc6(address=0x00, command=0x01)

def _gen_sirc(bb):
    gen = IRSIRCGenerator(bb, channel=0)
    gen.send_sirc(command=0x15, address=0x01)

def _gen_pwm(bb):
    gen = PWMGenerator(bb, channel=0, freq=1000, duty=0.5)
    gen.send_cycles(20)

def _gen_dcc(bb):
    gen = DCCGenerator(bb, channel=0)
    gen.send_packet(address=0x03, instruction=0x7F)

def _gen_dmx512(bb):
    gen = DMX512Generator(bb, channel=0)
    gen.send_frame()

def _gen_dali(bb):
    gen = DALIGenerator(bb, channel=0)
    gen.send_forward(address=0xFF, command=0x20)

def _gen_cec(bb):
    gen = CECGenerator(bb, channel=0)
    gen.send_frame(initiator=0x0, follower=0x4)

def _gen_spdif(bb):
    gen = SPDIFGenerator(bb, channel=0)
    gen.send_two_subframes(ch1_data=0x00000002, ch2_data=0x00000002)

def _gen_wiegand(bb):
    gen = WiegandGenerator(bb, 0, 1)
    gen.send_wiegand26(facility=0x01, card=0x0001)

def _gen_opentherm(bb):
    gen = OpenthermGenerator(bb, channel=0)
    gen.send_read_request(msg_type=0x00, data_id=0x00)

def _gen_sent(bb):
    gen = SENTGenerator(bb, channel=0)
    gen.send_message()

def _gen_mipi_rffe(bb):
    gen = MIPIRFFEGenerator(bb, 0, 1)
    gen.send_r0w(slave_addr=0x0)

def _gen_j1850vpw(bb):
    # J1850 VPW: edge-based, measures time between edges.
    # Default active=0 (active-low). SOF: active-low pulse 164-245us.
    # In DATA state: short (24-97us) active=1, short inactive=0,
    #   long (97-170us) active=0, long inactive=1.
    # IFS: >=240us between edges ends the frame.
    sr = bb.samplerate
    def us(v): return max(1, int(v * sr / 1e6))
    # Idle high before SOF (so falling edge triggers)
    bb.set_level(0, 1, us(300))
    # SOF: active-low pulse of 200us (within 164-245us range)
    bb.set_level(0, 0, us(200))
    # Now in DATA state. Send byte 0x61: 01100001 (MSB first)
    # Each bit is a pair: active-then-inactive or inactive-then-active
    # short=64us (24-97), long=128us (97-170)
    bits = [0,1,1,0,0,0,0,1]
    for bit in bits:
        if bit == 1:
            # Bit 1: short active (64us low) + long inactive (128us high)
            bb.set_level(0, 0, us(64))
            bb.set_level(0, 1, us(128))
        else:
            # Bit 0: long active (128us low) + short inactive (64us high)
            bb.set_level(0, 0, us(128))
            bb.set_level(0, 1, us(64))
    # EOF/IFS: >=240us high
    bb.set_level(0, 1, us(280))

def _gen_sdcard(bb):
    # SD card: CMD line (ch0) with CLK (ch1)
    # CMD0: start(0) + tx(1) + cmd(000000) + arg(00000000_00000000_00000000) + crc(1001010) + stop(1)
    # = 48 bits total. Use SPI-like clocking.
    cmd_bits = [0,1, 0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 1,0,0,1,0,1,0, 1]
    for bit in cmd_bits:
        bb.set_level(0, bit, 0)
        bb.set_level(1, 1, 5)  # CLK high
        bb.set_level(1, 0, 5)  # CLK low

def _gen_sdio(bb):
    gen = SDIOGenerator(bb, cmd_ch=0, clk_ch=1)
    gen.send_command(0, 0x00000000)  # CMD0: GO_IDLE_STATE

def _gen_qspi(bb):
    # QSPI: CLK(ch0) + DQ0(ch1), SPI-like (no CS, no MISO)
    gen = SPIGenerator(bb, clk=0, mosi=1, miso=1, cs=1)
    gen.write_byte(0x03)  # Read command
    gen.write_byte(0x00)  # Address
    gen.write_byte(0x00)
    gen.write_byte(0x00)

def _gen_spi_dual_quad(bb):
    # SPI dual quad: CLK(ch0) + DQ0(ch1) + DQ1(ch2)
    gen = SPIGenerator(bb, clk=0, mosi=1, miso=2, cs=1)
    gen.write_byte(0x3B)  # Dual Output Fast Read
    gen.write_byte(0x00)
    gen.write_byte(0x00)
    gen.write_byte(0x00)

def _gen_st7735(bb):
    # ST7735: CS(ch0), CLK(ch1), MOSI(ch2), DC(ch3)
    # Decoder checks data on CLK rising edge, CS must be low.
    # DC=0 for command, DC=1 for data.
    # Description is flushed on NEXT command, so send TWO commands.
    hp = max(2, int(bb.samplerate / 2000000))
    # Idle: CS high, CLK low
    bb.write_channels({0: 1, 1: 0, 2: 0, 3: 0}, hp)
    # CS low (active) + Command 1: SWRESET (0x01) with DC=0
    for i in range(7, -1, -1):
        bit = (0x01 >> i) & 1
        bb.write_channels({0: 0, 1: 1, 2: bit, 3: 0}, hp)  # CLK high, MOSI=bit, DC=0
        bb.write_channels({0: 0, 1: 0, 2: bit, 3: 0}, hp)  # CLK low
    # Command 2: SLPOUT (0x11) with DC=0 - this flushes description of cmd1
    for i in range(7, -1, -1):
        bit = (0x11 >> i) & 1
        bb.write_channels({0: 0, 1: 1, 2: bit, 3: 0}, hp)  # CLK high, MOSI=bit, DC=0
        bb.write_channels({0: 0, 1: 0, 2: bit, 3: 0}, hp)  # CLK low
    # CS high (inactive)
    bb.write_channels({0: 1, 1: 0, 2: 0, 3: 0}, hp)

def _gen_st7789(bb):
    # ST7789: CSX(ch0), DCX(ch1), SDO(ch2), WRX(ch3)
    # Decoder waits for CSX falling edge, then reads bits on DCX rising edge.
    # WRX=0 for command, WRX=1 for data.
    # cmd_data is flushed on CSX rising or next command, so send TWO commands.
    hp = max(2, int(bb.samplerate / 2000000))
    # Idle: CSX high, DCX low
    bb.write_channels({0: 1, 1: 0, 2: 0, 3: 1}, hp)
    # CSX low (active) + Command 1: SWRESET (0x01) with WRX=0
    for i in range(7, -1, -1):
        bit = (0x01 >> i) & 1
        bb.write_channels({0: 0, 1: 1, 2: bit, 3: 0}, hp)  # DCX rising, SDO=bit, WRX=0
        bb.write_channels({0: 0, 1: 0, 2: bit, 3: 0}, hp)  # DCX falling
    # Command 2: SLPOUT (0x11) with WRX=0
    for i in range(7, -1, -1):
        bit = (0x11 >> i) & 1
        bb.write_channels({0: 0, 1: 1, 2: bit, 3: 0}, hp)  # DCX rising, SDO=bit, WRX=0
        bb.write_channels({0: 0, 1: 0, 2: bit, 3: 0}, hp)  # DCX falling
    # CSX high (inactive) - flushes cmd_data
    bb.write_channels({0: 1, 1: 0, 2: 0, 3: 1}, hp)

def _gen_spacewire(bb):
    # SpaceWire: DIN(ch0) + SIN(ch1), Data-Strobe encoding
    # Send a few bytes: Strobe toggles when Data doesn't toggle
    data_bytes = [0x00, 0x01, 0x02, 0x03]
    last_d = 0; last_s = 0
    for byte in data_bytes:
        for i in range(8):
            bit = (byte >> i) & 1
            d = bit ^ last_d if bit else last_d
            s = last_s ^ (1 if d == last_d else 0)
            bb.set_level(0, d, 10)
            bb.set_level(1, s, 10)
            last_d = d; last_s = s

def _gen_flexray(bb):
    # FlexRay: TSS (5 bits low) + FSS (1 bit high) + header
    bb.set_level(0, 0, 50)   # TSS
    bb.set_level(0, 1, 10)   # FSS
    # Header byte 0x80
    for bit in [1,0,0,0,0,0,0,0]:
        bb.set_level(0, bit, 10)

def _gen_maple_bus(bb):
    gen = MapleBusGenerator(bb, sdcka_ch=0, sdckb_ch=1)
    # Send a frame: size=0x01 (8 bytes total), src=0x20, dst=0x01, cmd=0x01, no extra data
    gen.send_frame(size_byte=0x01, src_ap=0x20, dst_ap=0x01, command=0x01)

def _gen_iec(bb):
    # IEC bus: CLK(ch0) + DATA(ch1) + XIN(ch2)
    # Simplified: generate CLK and DATA activity
    bb.set_level(2, 1, 100)  # XIN high
    # Send a byte on IEC: CLK toggles, DATA changes
    for bit in [1,0,0,0,1,0,0,0]:
        bb.set_level(1, bit, 0)
        bb.set_level(0, 1, 50)
        bb.set_level(0, 0, 50)

def _gen_stepper(bb):
    # Stepper: STEP(ch0) + DIR(ch1)
    # 10 steps forward
    bb.set_level(1, 1, 0)  # DIR=forward
    for _ in range(10):
        bb.set_level(0, 1, 10)
        bb.set_level(0, 0, 90)
    # 5 steps backward
    bb.set_level(1, 0, 0)  # DIR=backward
    for _ in range(5):
        bb.set_level(0, 1, 10)
        bb.set_level(0, 0, 90)

def _gen_xy2_100(bb):
    # XY2-100: CLK(ch0) + DATAx(ch1) + DATAY(ch2)
    # 20-bit frames: start + 18 data + parity
    for val in [0x1000, 0x2000]:
        bits = []
        # Start bit
        bits.append(1)
        # 18 data bits (MSB first)
        for i in range(17, -1, -1):
            bits.append((val >> i) & 1)
        # Parity
        bits.append(sum(bits) % 2)
        for bit in bits:
            bb.set_level(1, bit, 0)
            bb.set_level(2, bit, 0)
            bb.set_level(0, 1, 5)
            bb.set_level(0, 0, 5)

def _gen_z80(bb):
    gen = Z80Generator(bb)
    gen.m1_cycle(0x0000, 0x3E)

def _gen_mcs48(bb):
    gen = MCS48Generator(bb)
    gen.opcode_fetch(0x000, 0x04)

def _gen_gpib(bb):
    gen = GPIBGenerator(bb)
    gen.command_sequence()

def _gen_lpc(bb):
    gen = LPCGenerator(bb)
    gen.io_write(0x80, 0x55)

def _gen_am230x(bb):
    gen = AM230xGenerator(bb, channel=0)
    gen.send_reading(50.0, 25.0)

def _gen_dcf77(bb):
    gen = DCF77Generator(bb, channel=0)
    gen.send_time([0,0,1,0,0,1,0,0,1,0,0,1,0,1,0,0,1,0,0,1])

def _gen_caliper(bb):
    gen = CaliperGenerator(bb, 0, 1)
    gen.send_value(1234)

def _gen_c2(bb):
    gen = C2Generator(bb, 0, 1)
    gen.send_reset()

def _gen_avr_pdi(bb):
    gen = AVRPDIGenerator(bb, 0, 1)
    gen.send_break()

def _gen_delta_sigma(bb):
    gen = DeltaSigmaGenerator(bb, 0, 1)
    gen.send_pattern([1,0,1,0,1,1,0,0])

def _gen_em4100(bb):
    gen = EM4100Generator(bb, channel=0)
    gen.send_card(0x123456789A)

def _gen_em4305(bb):
    gen = EM4305Generator(bb, channel=0)
    gen.send_write_word()

def _gen_one_single_wire(bb):
    gen = OneSingleWireGenerator(bb, 0, 1)
    gen.send_byte(0x55)

def _gen_ook(bb):
    gen = OOKGenerator(bb, channel=0)
    gen.send_bits([1,0,1,0,1,1,0,0])

def _gen_sony_md(bb):
    gen = SonyMDGenerator(bb, channel=0)
    gen.send_message([0x00, 0x01])

def _gen_tdm_audio(bb):
    gen = TDMAudioGenerator(bb, 0, 1, 2)
    gen.send_frame()

def _gen_tlc5620(bb):
    gen = TLC5620Generator(bb, 0, 1)
    gen.write(0, 1, 0x80)

def _gen_usb_pd(bb):
    gen = USBPowerDeliveryGenerator(bb, channel=0)
    # Header 0x0161: GoodCRC control message (count=0, SRC, rev2, DFP)
    # 0x0161 = ext:0, count:0, id:0, pwr_role:1(SRC), rev:1(rev2), data_role:1(DFP), type:1(GOOD_CRC)
    gen.send_packet(header=0x0161)

def _gen_miller(bb):
    gen = MillerGenerator(bb, channel=0)
    gen.send_bits([1,0,1,0,1,1,0,0])

def _gen_iebus(bb):
    gen = IEBUSGenerator(bb, channel=0)
    gen.send_frame([1,1,1,0], 0x01, 0x02)

def _gen_ieee488(bb):
    # IEEE488 in parallel GPIB mode - reuse GPIBGenerator with 16 channels
    # Channels: DIO1-8(ch0-7), EOI(ch8), DAV(ch9), NRFD(ch10), NDAC(ch11), IFC(ch12), SRQ(ch13), ATN(ch14), REN(ch15)
    gen = GPIBGenerator(bb)
    gen.command_sequence()

def _gen_fsi(bb):
    gen = FSiGenerator(bb, 0, 1)
    gen.send_frame(0x01, 0x55)

def _gen_eth_an(bb):
    gen = EthANGenerator(bb, channel=0)
    gen.send_flp_burst(0x0040)

def _gen_qi(bb):
    gen = QiGenerator(bb, channel=0)
    # Header 0x01 = Signal Strength, needs 1 data byte + 1 checksum = 3 bytes total
    gen.send_packet(0x01, [0x02])

def _gen_pxx1(bb):
    gen = PXX1Generator(bb, channel=0)
    gen.send_bind_frame(0x1234)

def _gen_pjdl(bb):
    gen = PJDLGenerator(bb, channel=0)
    gen.write_byte(0x55)

def _gen_rc_encode(bb):
    gen = RCEncodeGenerator(bb, channel=0)
    gen.send_pattern([1,0,1,0,1,1,0,0])

def _gen_bean(bb):
    gen = BEANGenerator(bb, channel=0)
    gen.send_frame(pri=0x04, dst_id=0xFE, mes_id=0xAB, data_bytes=[0xA1, 0x80])

def _gen_ws281x(bb):
    gen = RGBLEDWS281xGenerator(bb, channel=0)
    gen.send_rgb(0xFF, 0x00, 0x00)

def _gen_rinnai(bb):
    gen = RinnaiControlPanelGenerator(bb, channel=0)
    gen.send_command(0x01)

def _gen_carrera(bb):
    gen = CarreraGenerator(bb, channel=0)
    gen.send_controller_word(controller_id=0, gas=5)

def _gen_sdq(bb):
    gen = SDQGenerator(bb, channel=0)
    gen.send_transaction()

def _gen_swim(bb):
    gen = SwimGenerator(bb, channel=0)
    gen.send_command(0x01)

def _gen_swi(bb):
    gen = SWIGenerator(bb, channel=0)
    gen.write_byte(0x55)

def _gen_mvbus(bb):
    gen = MvbGenerator(bb, {'mvb': 0}, 6000000)
    gen.send_master_frame(f_code=0, address=0x001)

def _gen_morse(bb):
    gen = MorseGenerator(bb, channel=0)
    gen.send_sos()

def _gen_lfast(bb):
    gen = LFASTGenerator(bb, channel=0)
    # Send a data frame: payload_size_id=1 (4 bytes), channel_type=4 (Data Channel A)
    # Use alternating bit patterns to ensure frequent edges for reliable decoding
    gen.send_frame(payload_size_id=0b001, channel_type=0b0100, cts=0,
                   payload_bytes=[0xAA, 0x55, 0xAA, 0x55])

def _gen_dsi(bb):
    gen = DSIGenerator(bb, channel=0)
    gen.send_backward_frame(0x80)

def _gen_ir_irmp(bb):
    # IRMP can decode many IR protocols - test multiple protocols
    
    # 1. NEC
    gen_nec = IRNECGenerator(bb, channel=0)
    gen_nec.send_nec(address=0x04, command=0x08)
    bb.set_level(0, 1, int(bb.samplerate * 0.05))  # 50ms idle
    
    # 2. RC5
    gen_rc5 = IRRC5Generator(bb, channel=0)
    gen_rc5.send_rc5(address=0x05, command=0x0A)
    bb.set_level(0, 1, int(bb.samplerate * 0.05))  # 50ms idle
    
    # 3. RC6
    gen_rc6 = IRRC6Generator(bb, channel=0)
    gen_rc6.send_rc6(address=0x06, command=0x0B)
    bb.set_level(0, 1, int(bb.samplerate * 0.05))  # 50ms idle
    
    # 4. SIRC (Sony)
    gen_sirc = IRSIRCGenerator(bb, channel=0)
    gen_sirc.send_sirc(command=0x15, address=0x01)
    bb.set_level(0, 1, int(bb.samplerate * 0.05))  # 50ms idle

def _gen_ir_ltto(bb):
    # LTTO uses specific IR protocol: pre-sync + pause + sync + data bits
    gen = IRLTTOGenerator(bb, channel=0)
    gen.send_signature(data=0x05, num_bits=5)

def _gen_ir_recoil(bb):
    # Recoil uses specific IR protocol: sync + sync pause + data bits
    gen = IRRecoilGenerator(bb, channel=0)
    gen.send_packet(data=0x05, num_bits=5)

def _gen_counter(bb):
    gen = PWMGenerator(bb, channel=0, freq=100, duty=0.5)
    gen.send_cycles(50)

def _gen_guess_bitrate(bb):
    gen = PWMGenerator(bb, channel=0, freq=9600, duty=0.5)
    gen.send_cycles(100)

def _gen_rpm(bb):
    gen = PWMGenerator(bb, channel=0, freq=50, duty=0.5)
    gen.send_cycles(100)

def _gen_timing(bb):
    gen = PWMGenerator(bb, channel=0, freq=1000, duty=0.3)
    gen.send_cycles(10)

def _gen_signature(bb):
    # Signature decoder needs SPI-like data
    gen = SPIGenerator(bb, clk=0, mosi=1, miso=2, cs=3)
    gen.select()
    gen.write_byte(0x9F)
    gen.write_byte(0x00)
    gen.write_byte(0x00)
    gen.write_byte(0x00)
    gen.deselect()

def _gen_sle44xx(bb):
    # SLE44xx smartcard - similar to ISO7816
    gen = ISO7816Generator(bb, 0, 1)
    gen.send_atr()

def _gen_tmc(bb):
    # TMC (Titan Micro Circuit) - I2C-like 2-wire protocol
    # Channels: CLK(ch0), DIO(ch1), STB(ch2, optional)
    # TMC decoder uses 2-wire mode (CLK+DIO):
    # START: DIO falling edge while CLK high
    # DATA: 8 bits LSB-first, sampled on CLK rising edge
    # ACK: CLK falling edge after 8th data bit
    # STOP: DIO rising edge while CLK high
    # For TM1637: send DATA_CMD(0x40) + ADDR_CMD(0xC0) + DATA(0x06) + STOP
    hp = max(2, int(bb.samplerate / 100000))
    # Idle: CLK high, DIO high
    bb.set_level(0, 1, hp * 2)
    bb.set_level(1, 1, hp * 2)

    def start_condition():
        # DIO falls while CLK high
        bb.set_level(0, 1, hp)   # CLK high
        bb.set_level(1, 1, hp // 2)  # DIO high
        bb.set_level(1, 0, hp - hp // 2)  # DIO falls (START)

    def send_byte_lsb(val):
        # 8 bits LSB-first on CLK rising edge
        for i in range(8):
            bit = (val >> i) & 1
            bb.set_level(1, bit, 0)  # Set DIO
            bb.set_level(0, 1, hp)   # CLK rising edge (sample)
            bb.set_level(0, 0, hp)   # CLK falling edge
        # ACK: one more CLK pulse (DIO can be anything)
        bb.set_level(0, 1, hp)   # CLK rising edge
        bb.set_level(0, 0, hp)   # CLK falling edge

    def stop_condition():
        # DIO rises while CLK high
        bb.set_level(1, 0, hp // 2)  # DIO low
        bb.set_level(0, 1, hp)       # CLK high
        bb.set_level(1, 1, hp)       # DIO rises (STOP)

    # Transaction 1: DATA command (0x40 = write, auto-address, normal mode)
    start_condition()
    send_byte_lsb(0x40)
    stop_condition()

    # Transaction 2: ADDRESS command (0xC0 = address 0) + DATA byte
    start_condition()
    send_byte_lsb(0xC0)  # Address command, digit 1
    send_byte_lsb(0x06)  # Data: segments for '1'
    stop_condition()

def _gen_usb_signalling(bb):
    gen = USBGenerator(bb, dp_ch=0, dm_ch=1)
    gen.send_packet([0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00])

def _gen_uart(bb):
    gen = UARTGenerator(bb, channel=0)
    gen.write_byte(0x55)
    gen.write_byte(0xAA)

def _gen_i2c(bb):
    gen = I2CGenerator(bb, scl_ch=0, sda_ch=1)
    gen.start()
    gen.write_byte(0x50 << 1)
    gen.write_byte(0x00)
    gen.stop()

def _gen_spi(bb):
    gen = SPIGenerator(bb, clk=0, mosi=1, miso=2, cs=3)
    gen.select()
    gen.write_byte(0x9F)
    gen.deselect()

def _gen_adat(bb):
    # ADAT: NRZ-like with sync pattern. 1 channel.
    # Send a sync pattern followed by data
    # ADAT uses a specific sync pattern at the start of each frame
    # Simplified: generate alternating data with periodic sync
    bit_width = max(2, int(bb.samplerate / 100000))
    # Sync: long low pulse
    bb.set_level(0, 0, bit_width * 10)
    # Data: alternating bits
    for _ in range(64):
        bb.set_level(0, 1, bit_width)
        bb.set_level(0, 0, bit_width)
        bb.set_level(0, 1, bit_width)
        bb.set_level(0, 1, bit_width)
        bb.set_level(0, 0, bit_width)
        bb.set_level(0, 0, bit_width)
        bb.set_level(0, 1, bit_width)
        bb.set_level(0, 0, bit_width)

def _gen_adb(bb):
    # ADB: attention signal + bit cells. 1 channel.
    # Attention: 560-1040us low, then 65-130us high
    # Bit cells: 65us per bit, 1=65us low+65us high, 0=35us low+95us high (approx)
    sr = bb.samplerate
    def us(v): return max(1, int(v * sr / 1e6))
    # Attention signal
    bb.set_level(0, 0, us(800))
    bb.set_level(0, 1, us(100))
    # Sync bit
    bb.set_level(0, 0, us(65))
    bb.set_level(0, 1, us(65))
    # Command: 0x84 (talk, addr 4, reg 0)
    for i in range(7, -1, -1):
        bit = (0x84 >> i) & 1
        if bit:
            bb.set_level(0, 0, us(65))
            bb.set_level(0, 1, us(65))
        else:
            bb.set_level(0, 0, us(35))
            bb.set_level(0, 1, us(95))
    # Stop bit
    bb.set_level(0, 1, us(200))

def _gen_afsk(bb):
    # AFSK: two frequencies (mark=1, space=0). 1 channel.
    # Bell 202: mark=1200Hz, space=2200Hz
    # Generate alternating mark and space tones
    sr = bb.samplerate
    # Mark tone (1200Hz) for 8 bits
    mark_period = max(2, int(sr / 1200))
    for _ in range(8):
        bb.set_level(0, 1, mark_period // 2)
        bb.set_level(0, 0, mark_period - mark_period // 2)
    # Space tone (2200Hz) for 8 bits
    space_period = max(2, int(sr / 2200))
    for _ in range(8):
        bb.set_level(0, 1, space_period // 2)
        bb.set_level(0, 0, space_period - space_period // 2)
    # More mark tone
    for _ in range(16):
        bb.set_level(0, 1, mark_period // 2)
        bb.set_level(0, 0, mark_period - mark_period // 2)

def _gen_ac97(bb):
    # AC97: SYNC(ch0) + CLK(ch1) - only required channels
    hp = max(2, int(bb.samplerate / 200000))
    # SYNC pulse (marks start of frame): high for 1 bit, low for 15 bits
    bb.set_level(0, 1, hp)
    bb.set_level(0, 0, hp * 15)
    # Clock and data: 16 CLK cycles with SYNC pattern
    for _ in range(16):
        bb.set_level(1, 1, hp)
        bb.set_level(1, 0, hp)

def _gen_t55xx(bb):
    # T55xx: field clock gap encoding. 1 channel.
    # Uses start_gap + write_gaps with on-time between gaps determining bit value
    gen = T55xxGenerator(bb, channel=0)
    gen.send_write_command(opcode=0b10, lock=0, data=0x12345678, address=2)

def _gen_jitter(bb):
    # Jitter: PWM signal with slight timing variations
    gen = PWMGenerator(bb, channel=0, freq=1000, duty=0.5)
    gen.send_cycles(50)

def _gen_seven_segment(bb):
    # Seven segment: just generate transitions on 1 channel
    gen = PWMGenerator(bb, channel=0, freq=100, duty=0.5)
    gen.send_cycles(20)

def _gen_parallel(bb):
    # Parallel: CLK on ch0, D0-D7 on ch1-8
    for val in [0x55, 0xAA, 0xFF, 0x00, 0x12, 0x34]:
        # Set data lines
        for bit in range(8):
            bb.set_level(1 + bit, (val >> bit) & 1, 0)
        # Clock pulse
        bb.set_level(0, 1, 50)
        bb.set_level(0, 0, 50)

def _gen_graycode(bb):
    # Graycode: generate a gray code sequence on 1 channel
    # Just toggle the channel in a gray code pattern
    gray_vals = [0, 1, 3, 2, 6, 7, 5, 4]
    for v in gray_vals:
        bb.set_level(0, v & 1, 100)

def _gen_numbers_and_state(bb):
    # Numbers and state decoder: CLK(ch0) + bit0(ch1) + bit1(ch2) + ...
    # With CLK, decoder samples data on rising CLK edge
    # Send some values with clock
    hp = max(2, int(bb.samplerate / 200000))
    for val in [0x55, 0xAA, 0xFF, 0x00, 0x12, 0x34]:
        # Set data lines (bit0=ch1, bit1=ch2, ..., bit7=ch8)
        for bit in range(8):
            bb.set_level(1 + bit, (val >> bit) & 1, 0)
        # Clock pulse
        bb.set_level(0, 1, hp)
        bb.set_level(0, 0, hp)

def _gen_ccd(bb):
    gen = CCDGenerator(bb, channel=0)
    # Send a Speed message: header 0x24 + 2 data bytes + checksum
    gen.send_message([0x24, 0x32, 0x14])  # Speed: 50 km/h, 20 mph

def _gen_rvswd(bb):
    gen = RVSWDGenerator(bb, clk_ch=0, dio_ch=1)
    gen.send_short_packet(addr_host=0x01, operation=0, data_target=0x00000001)

# Master decoder config: decoder_id -> {samplerate, num_channels, sample_count, gen_func, extra_stack}
DECODER_CONFIG = {
    # === High priority: common protocols ===
    "jtag_c":            {"gen": _gen_jtag, "num_channels": 4, "sample_count": 100000},
    "mdio_c":            {"gen": _gen_mdio, "num_channels": 2, "sample_count": 100000},
    "microwire_c":       {"gen": _gen_microwire, "num_channels": 4, "sample_count": 100000},
    "hdlc_c":            {"gen": _gen_hdlc, "num_channels": 1, "sample_count": 100000},
    "i2s_c":             {"gen": _gen_i2s, "num_channels": 3, "samplerate": 4000000, "sample_count": 200000},
    "iso7816_c":         {"gen": _gen_iso7816, "num_channels": 2, "sample_count": 100000},
    "swd_c":             {"gen": _gen_swd, "num_channels": 2, "sample_count": 100000},
    "rvswd_c":           {"gen": _gen_rvswd, "num_channels": 2, "sample_count": 100000},
    "onewire_c":         {"gen": _gen_onewire, "num_channels": 1, "sample_count": 100000},
    "onewire_link_c":    {"gen": _gen_onewire, "num_channels": 1, "sample_count": 100000},
    "can_c":             {"gen": _gen_can, "num_channels": 1, "samplerate": 10000000, "sample_count": 200000},
    "can_fd_c":          {"gen": _gen_can, "num_channels": 1, "samplerate": 10000000, "sample_count": 200000},
    "ps2_c":             {"gen": _gen_ps2, "num_channels": 2, "sample_count": 100000},

    # === Medium priority: special protocols ===
    "nrzi_c":            {"gen": _gen_nrzi, "num_channels": 1, "sample_count": 100000},
    "ir_nec_c":          {"gen": _gen_nec, "num_channels": 1, "sample_count": 200000},
    "ir_rc5_c":          {"gen": _gen_rc5, "num_channels": 1, "sample_count": 200000},
    "ir_rc6_c":          {"gen": _gen_rc6, "num_channels": 1, "sample_count": 200000},
    "ir_sirc_c":         {"gen": _gen_sirc, "num_channels": 1, "sample_count": 200000},
    "ir_irmp_c":         {"gen": _gen_ir_irmp, "num_channels": 1, "sample_count": 200000},
    "ir_ltto_c":         {"gen": _gen_ir_ltto, "num_channels": 1, "sample_count": 200000},
    "ir_recoil_c":       {"gen": _gen_ir_recoil, "num_channels": 1, "sample_count": 200000},
    "pwm_c":             {"gen": _gen_pwm, "num_channels": 1, "sample_count": 100000},
    "counter_c":         {"gen": _gen_counter, "num_channels": 1, "sample_count": 100000},
    "guess_bitrate_c":   {"gen": _gen_guess_bitrate, "num_channels": 1, "sample_count": 100000},
    "rpm_c":             {"gen": _gen_rpm, "num_channels": 1, "sample_count": 200000},
    "timing_c":          {"gen": _gen_timing, "num_channels": 1, "sample_count": 100000},
    "dcc_c":             {"gen": _gen_dcc, "num_channels": 1, "sample_count": 100000},
    "dmx512_c":          {"gen": _gen_dmx512, "num_channels": 1, "samplerate": 4000000, "sample_count": 500000},
    "dali_c":            {"gen": _gen_dali, "num_channels": 1, "samplerate": 1000000, "sample_count": 200000},
    "cec_c":             {"gen": _gen_cec, "num_channels": 1, "sample_count": 100000},
    "spdif_c":           {"gen": _gen_spdif, "num_channels": 1, "samplerate": 6000000, "sample_count": 300000},
    "wiegand_c":         {"gen": _gen_wiegand, "num_channels": 2, "sample_count": 100000},
    "opentherm_c":       {"gen": _gen_opentherm, "num_channels": 1, "sample_count": 100000},
    "sent_c":            {"gen": _gen_sent, "num_channels": 1, "sample_count": 100000},
    "mipi_rffe_c":       {"gen": _gen_mipi_rffe, "num_channels": 2, "sample_count": 100000},

    # === Low priority: rare/complex protocols ===
    "sae_j1850_vpw_c":   {"gen": _gen_j1850vpw, "num_channels": 1, "sample_count": 100000},
    "sdcard_sd_c":       {"gen": _gen_sdcard, "num_channels": 2, "sample_count": 100000},
    "sdio_c":            {"gen": _gen_sdio, "num_channels": 2, "sample_count": 100000},
    "qspi_c":            {"gen": _gen_qspi, "num_channels": 2, "sample_count": 100000},
    "spi_dual_quad_c":   {"gen": _gen_spi_dual_quad, "num_channels": 3, "sample_count": 100000},
    "st7735_c":          {"gen": _gen_st7735, "num_channels": 4, "sample_count": 100000},
    "st7789_c":          {"gen": _gen_st7789, "num_channels": 4, "sample_count": 100000},
    "spacewire_c":       {"gen": _gen_spacewire, "num_channels": 2, "sample_count": 100000},
    "flexray_c":         {"gen": _gen_flexray, "num_channels": 1, "sample_count": 100000},
    "maple_bus_c":       {"gen": _gen_maple_bus, "num_channels": 2, "sample_count": 100000},
    "iec_c":             {"gen": _gen_iec, "num_channels": 3, "sample_count": 100000},
    "stepper_motor_c":   {"gen": _gen_stepper, "num_channels": 2, "sample_count": 100000},
    "xy2_100_c":         {"gen": _gen_xy2_100, "num_channels": 3, "sample_count": 100000},

    # === Special: multi-channel complex protocols ===
    "z80_c":             {"gen": _gen_z80, "num_channels": 13, "sample_count": 100000},
    "mcs48_c":           {"gen": _gen_mcs48, "num_channels": 14, "sample_count": 100000},
    "gpib_c":            {"gen": _gen_gpib, "num_channels": 16, "sample_count": 100000},
    "lpc_c":             {"gen": _gen_lpc, "num_channels": 6, "sample_count": 100000},

    # === Special: simple 1-2 channel protocols ===
    "am230x_c":          {"gen": _gen_am230x, "num_channels": 1, "sample_count": 100000},
    "dcf77_c":           {"gen": _gen_dcf77, "num_channels": 1, "sample_count": 2000000},
    "caliper_c":         {"gen": _gen_caliper, "num_channels": 2, "sample_count": 100000},
    "c2_c":              {"gen": _gen_c2, "num_channels": 2, "sample_count": 100000},
    "avr_pdi_c":         {"gen": _gen_avr_pdi, "num_channels": 2, "sample_count": 100000},
    "delta-sigma_c":     {"gen": _gen_delta_sigma, "num_channels": 2, "sample_count": 500000},
    "em4100_c":          {"gen": _gen_em4100, "num_channels": 1, "sample_count": 200000},
    "em4305_c":          {"gen": _gen_em4305, "num_channels": 1, "sample_count": 200000},
    "one_single_wire_c": {"gen": _gen_one_single_wire, "num_channels": 2, "sample_count": 100000},
    "ook_c":             {"gen": _gen_ook, "num_channels": 1, "sample_count": 100000},
    "sony_md_c":         {"gen": _gen_sony_md, "num_channels": 1, "sample_count": 100000},
    "tdm_audio_c":       {"gen": _gen_tdm_audio, "num_channels": 3, "sample_count": 100000},
    "tlc5620_c":         {"gen": _gen_tlc5620, "num_channels": 2, "sample_count": 100000},
    "usb_power_delivery_c": {"gen": _gen_usb_pd, "num_channels": 1, "samplerate": 10000000, "sample_count": 200000},
    "miller_c":          {"gen": _gen_miller, "num_channels": 1, "sample_count": 100000},
    "iebus_c":           {"gen": _gen_iebus, "num_channels": 1, "sample_count": 100000},
    "ieee488_c":         {"gen": _gen_ieee488, "num_channels": 16, "sample_count": 100000},
    "fsi_c":             {"gen": _gen_fsi, "num_channels": 2, "sample_count": 100000},
    "eth_an_c":          {"gen": _gen_eth_an, "num_channels": 1, "sample_count": 100000},
    "qi_c":              {"gen": _gen_qi, "num_channels": 1, "sample_count": 100000},
    "pxx1_c":            {"gen": _gen_pxx1, "num_channels": 1, "sample_count": 100000},
    "pjdl_c":            {"gen": _gen_pjdl, "num_channels": 1, "sample_count": 100000},
    "rc_encode_c":       {"gen": _gen_rc_encode, "num_channels": 1, "sample_count": 100000},
    "rgb_led_ws281x_c":  {"gen": _gen_ws281x, "num_channels": 1, "samplerate": 8000000, "sample_count": 200000},
    "rinnai_control_panel_c": {"gen": _gen_rinnai, "num_channels": 1, "sample_count": 100000},
    "carrera_c":         {"gen": _gen_carrera, "num_channels": 1, "sample_count": 100000},
    "sdq_c":             {"gen": _gen_sdq, "num_channels": 1, "sample_count": 100000},
    "swim_c":            {"gen": _gen_swim, "num_channels": 1, "sample_count": 100000},
    "swi_c":             {"gen": _gen_swi, "num_channels": 1, "sample_count": 100000},
    "mvb_c":             {"gen": _gen_mvbus, "num_channels": 1, "samplerate": 6000000, "sample_count": 100000},
    "morse_c":           {"gen": _gen_morse, "num_channels": 1, "sample_count": 500000},
    "lfast_c":           {"gen": _gen_lfast, "num_channels": 1, "sample_count": 100000},
    "dsi_c":             {"gen": _gen_dsi, "num_channels": 1, "sample_count": 100000},
    "signature_c":       {"gen": _gen_signature, "num_channels": 4, "sample_count": 100000},
    "sle44xx_c":         {"gen": _gen_sle44xx, "num_channels": 3, "sample_count": 100000},
    "tmc_c":             {"gen": _gen_tmc, "num_channels": 4, "sample_count": 100000},
    "parallel_c":        {"gen": _gen_parallel, "num_channels": 9, "sample_count": 100000},
    "graycode_c":        {"gen": _gen_graycode, "num_channels": 1, "sample_count": 100000},
    "numbers_and_state_c": {"gen": _gen_numbers_and_state, "num_channels": 9, "sample_count": 100000},
    "ccd_c":             {"gen": _gen_ccd, "num_channels": 1, "sample_count": 100000},
    "sda2506_c":         {"gen": _gen_i2c, "num_channels": 2, "sample_count": 100000},
    "tl5620_c":          {"gen": _gen_tlc5620, "num_channels": 2, "sample_count": 100000},
    "mipi_dsi_c":        {"gen": _gen_dsi, "num_channels": 2, "sample_count": 100000},
    "emmc_sd_c":         {"gen": _gen_sdcard, "num_channels": 2, "sample_count": 100000},
    "aud_c":             {"gen": _gen_i2s, "num_channels": 6, "samplerate": 4000000, "sample_count": 200000},
    "pcfx_ctrlr_c":      {"gen": _gen_spi, "num_channels": 4, "sample_count": 100000},
    "cjtag_c":           {"gen": _gen_jtag, "num_channels": 4, "sample_count": 100000},
    "cjtag_oscan0_c":    {"gen": _gen_jtag, "num_channels": 4, "sample_count": 100000},
    "bean_c":            {"gen": _gen_bean, "num_channels": 1, "sample_count": 100000},
    "ac97_c":            {"gen": _gen_ac97, "num_channels": 2, "sample_count": 100000},
    "adat_c":            {"gen": _gen_adat, "num_channels": 1, "samplerate": 100000000, "sample_count": 200000},
    "adb_c":             {"gen": _gen_adb, "num_channels": 1, "sample_count": 100000},
    "afsk_c":            {"gen": _gen_afsk, "num_channels": 1, "sample_count": 100000},
    "t55xx_c":           {"gen": _gen_t55xx, "num_channels": 1, "sample_count": 200000},
    "jitter_c":          {"gen": _gen_jitter, "num_channels": 2, "sample_count": 100000},
    "seven_segment_c":   {"gen": _gen_seven_segment, "num_channels": 7, "sample_count": 100000},

    # === Base decoders (needed for stack chains) ===
    "i2c_c":             {"gen": _gen_i2c, "num_channels": 2, "sample_count": 100000},
    "spi_c":             {"gen": _gen_spi, "num_channels": 4, "sample_count": 100000},
    "spi_fast_c":        {"gen": _gen_spi, "num_channels": 4, "sample_count": 100000},
    "uart_c":            {"gen": _gen_uart, "num_channels": 1, "sample_count": 100000},
    "uart_fast_c":       {"gen": _gen_uart, "num_channels": 1, "sample_count": 100000},
}

# Stack decoder configurations: decoder_id -> list of upstream decoder IDs
STACK_CONFIG = {
    # JTAG chain
    "jtag_avr_c":    ["jtag_c"],
    "jtag_ejtag_c":  ["jtag_c"],
    "jtag_stm32_c":  ["jtag_c"],
    # OneWire chain
    "onewire_network_c": ["onewire_link_c"],
    "ds2408_c":      ["onewire_network_c", "onewire_link_c"],
    "ds243x_c":      ["onewire_network_c", "onewire_link_c"],
    "ds28ea00_c":    ["onewire_network_c", "onewire_link_c"],
    # PS/2 chain
    "ps2_keyboard_c": ["ps2_c"],
    "ps2_mouse_c":    ["ps2_c"],
    # CAN chain
    # (can_c and can_fd_c are base decoders, no stack needed)
    # NRZI -> 4b5b -> Ethernet chain
    "4b5b_c":        ["nrzi_c"],
    "ethernet_c":    ["4b5b_c", "nrzi_c"],
    "arp_c":         ["ethernet_c", "4b5b_c", "nrzi_c"],
    "ipv4_c":        ["ethernet_c", "4b5b_c", "nrzi_c"],
    "udp_c":         ["ipv4_c", "ethernet_c", "4b5b_c", "nrzi_c"],
    # OOK chain
    "ook_oregon_c":  ["ook_c"],
    "ook_vis_c":     ["ook_c"],
    # TMC chain
    "tm1637_c":      ["tmc_c"],
    "tm1638_c":      ["tmc_c"],
    # MDIO chain
    "cfp_c":         ["mdio_c"],
    # Microwire chain
    "eeprom93xx_c":  ["microwire_c"],
    # IEBus chain
    "avclan_c":      ["iebus_c"],
    # IR chain
    "ir_ltto_decode_c": ["ir_ltto_c"],
    # LFAST chain
    "sipi_c":        ["lfast_c"],
    # AFSK chain
    "ltar_smartdevice_c": ["afsk_c"],
    "ltar_smartdevice_decode_c": ["ltar_smartdevice_c", "afsk_c"],
    # Sony MD chain
    "sony_md_decode_c": ["sony_md_c"],
    # PJDL chain
    "pjon_c":        ["pjdl_c"],
    # USB chain
    "usb_request_c": ["usb_packet_c"],
    # TPM chain
    "tpm_fifo_tis_c": [],
}

# For stack decoders, what generator to use for the ROOT (bottom of stack)
STACK_ROOT_GEN = {
    "4b5b_c":        _gen_ethernet,
    "ethernet_c":    _gen_ethernet,
    "arp_c":         _gen_ethernet,
    "ipv4_c":        _gen_ethernet,
    "udp_c":         _gen_ethernet,
    "onewire_network_c": _gen_onewire,
    "ds2408_c":      _gen_onewire,
    "ds243x_c":      _gen_onewire,
    "ds28ea00_c":    _gen_onewire,
    "ps2_keyboard_c": _gen_ps2,
    "ps2_mouse_c":   _gen_ps2,
    "ook_oregon_c":  _gen_ook,
    "ook_vis_c":     _gen_ook,
    "tm1637_c":      _gen_tmc,
    "tm1638_c":      _gen_tmc,
    "cfp_c":         _gen_mdio,
    "eeprom93xx_c":  _gen_microwire,
    "avclan_c":      _gen_iebus,
    "ir_ltto_decode_c": _gen_ir_ltto,
    "sipi_c":        _gen_lfast,
    "ltar_smartdevice_c": _gen_pwm,  # AFSK root
    "ltar_smartdevice_decode_c": _gen_pwm,
    "sony_md_decode_c": _gen_sony_md,
    "jtag_avr_c": _gen_jtag,
    "jtag_ejtag_c": _gen_jtag,
    "jtag_stm32_c": _gen_jtag,
    "pjon_c":        _gen_pjdl,
    "usb_request_c": _gen_usb_signalling,
    "tpm_fifo_tis_c": _gen_spi,
}

# For stack decoders, override num_channels
STACK_NUM_CHANNELS = {
    "4b5b_c": 1,
    "ethernet_c": 1,
    "arp_c": 1,
    "ipv4_c": 1,
    "udp_c": 1,
    "onewire_network_c": 1,
    "ds2408_c": 1,
    "ds243x_c": 1,
    "ds28ea00_c": 1,
    "ps2_keyboard_c": 2,
    "ps2_mouse_c": 2,
    "ook_oregon_c": 1,
    "ook_vis_c": 1,
    "tm1637_c": 4,
    "tm1638_c": 4,
    "cfp_c": 2,
    "eeprom93xx_c": 4,
    "avclan_c": 1,
    "ir_ltto_decode_c": 1,
    "sipi_c": 1,
    "ltar_smartdevice_c": 1,
    "ltar_smartdevice_decode_c": 1,
    "sony_md_decode_c": 1,
    "jtag_avr_c": 4,
    "jtag_ejtag_c": 4,
    "jtag_stm32_c": 4,
    "pjon_c": 1,
    "usb_request_c": 2,
    "tpm_fifo_tis_c": 4,
}

# For stack decoders, override samplerate
STACK_SAMPLERATE = {
    "usb_request_c": 12000000,
}

# For stack decoders, override sample_count
STACK_SAMPLE_COUNT = {
    "4b5b_c": 100000,
    "ethernet_c": 100000,
    "arp_c": 100000,
    "ipv4_c": 100000,
    "udp_c": 100000,
    "onewire_network_c": 100000,
    "ds2408_c": 100000,
    "ds243x_c": 100000,
    "ds28ea00_c": 100000,
    "ps2_keyboard_c": 100000,
    "ps2_mouse_c": 100000,
    "ook_oregon_c": 100000,
    "ook_vis_c": 100000,
    "tm1637_c": 100000,
    "tm1638_c": 100000,
    "cfp_c": 100000,
    "eeprom93xx_c": 100000,
    "avclan_c": 100000,
    "ir_ltto_decode_c": 200000,
    "sipi_c": 100000,
    "ltar_smartdevice_c": 100000,
    "ltar_smartdevice_decode_c": 100000,
    "sony_md_decode_c": 100000,
    "pjon_c": 100000,
    "usb_request_c": 200000,
    "tpm_fifo_tis_c": 100000,
    "jtag_avr_c": 100000,
    "jtag_ejtag_c": 100000,
    "jtag_stm32_c": 100000,
}


def parse_decoder_metadata(c_file):
    with open(c_file, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()

    # Extract ID: first try the .id field inside srd_c_decoder struct, fall back to filename
    decoder_id = None
    struct_match = re.search(r'struct\s+srd_c_decoder\s+\w+\s*=\s*\{', content)
    if struct_match:
        after_struct = content[struct_match.end():struct_match.end() + 500]
        id_in_struct = re.search(r'\.id\s*=\s*"([^"]+)"', after_struct)
        if id_in_struct:
            decoder_id = id_in_struct.group(1)
    if not decoder_id:
        decoder_id = os.path.basename(c_file).replace(".c", "")

    # Extract Inputs
    inputs = []
    input_match = re.search(r'static\s+const\s+char\s*\*?\s*\w+_inputs\s*\[\]\s*=\s*\{([^}]+)\}', content, re.DOTALL)
    if input_match:
        inputs = [i.strip().strip('"') for i in input_match.group(1).split(',') if i.strip() and "NULL" not in i]

    # Extract Channels
    channels = {}
    ch_pat = r'static\s+struct\s+srd_channel\s+\w+_channels\s*\[\]\s*=\s*\{(.*?)\}\s*;'
    ch_match = re.search(ch_pat, content, re.DOTALL)
    if ch_match:
        entries = re.findall(r'\{\s*"([^"]+)"', ch_match.group(1))
        for i, name in enumerate(entries):
            channels[name] = i
    else:
        num_ch_match = re.search(r'\.num_channels\s*=\s*(\d+)', content)
        if num_ch_match:
            num = int(num_ch_match.group(1))
            for i in range(num):
                channels[f"ch{i}"] = i

    return decoder_id, inputs, channels


def generate_test_for_decoder(c_file):
    if any(b in c_file for b in BLACKLIST):
        return

    try:
        d_id, inputs, channels = parse_decoder_metadata(c_file)
        if d_id in BLACKLIST:
            return
    except Exception as e:
        print(f"ERROR parsing {c_file}: {e}")
        return

    # Determine if this is a stack decoder
    stack_ids = STACK_CONFIG.get(d_id, None)
    is_stack_decoder = stack_ids is not None

    # Determine num_channels, samplerate, sample_count
    if is_stack_decoder:
        num_channels = STACK_NUM_CHANNELS.get(d_id, 2)
        samplerate = STACK_SAMPLERATE.get(d_id, 1000000)
        sample_count = STACK_SAMPLE_COUNT.get(d_id, 100000)
    elif d_id in DECODER_CONFIG:
        cfg = DECODER_CONFIG[d_id]
        num_channels = cfg.get("num_channels", 2)
        samplerate = cfg.get("samplerate", 1000000)
        sample_count = cfg.get("sample_count", 20000)
    else:
        # Fallback: use parsed channels
        primary_input = inputs[0] if inputs else "logic"
        num_channels = max(channels.values()) + 1 if channels else 2
        samplerate = 1000000
        sample_count = 20000
        if "usb" in d_id:
            samplerate = 12000000
        if "can" in d_id:
            samplerate = 10000000
        if "adat" in d_id:
            samplerate = 100000000
        # Ensure enough channels for fallback generators
        if primary_input == "spi":
            num_channels = max(num_channels, 4)
        elif primary_input in ("i2c", "ps2", "usb_signalling", "mdio"):
            num_channels = max(num_channels, 2)
        elif primary_input == "jtag":
            num_channels = max(num_channels, 4)
        elif primary_input == "microwire":
            num_channels = max(num_channels, 4)

    # Build BitstreamBuilder
    bb = BitstreamBuilder(num_channels, sample_count, samplerate)

    # Generate protocol data
    gen_ok = False
    if is_stack_decoder:
        # Use the root generator for stack decoders
        root_gen = STACK_ROOT_GEN.get(d_id)
        if root_gen:
            try:
                root_gen(bb)
                gen_ok = True
            except Exception as e:
                print(f"  WARN: gen failed for {d_id}: {e}")
    elif d_id in DECODER_CONFIG:
        cfg = DECODER_CONFIG[d_id]
        gen_func = cfg.get("gen")
        if gen_func:
            try:
                gen_func(bb)
                gen_ok = True
            except Exception as e:
                print(f"  WARN: gen failed for {d_id}: {e}")
    else:
        # Fallback: try to match by input type
        primary_input = inputs[0] if inputs else "logic"
        if primary_input == "i2c":
            _gen_i2c(bb); gen_ok = True
        elif primary_input == "spi":
            _gen_spi(bb); gen_ok = True
        elif primary_input == "uart":
            _gen_uart(bb); gen_ok = True
        elif primary_input == "can":
            _gen_can(bb); gen_ok = True
        elif primary_input == "usb_signalling":
            _gen_usb_signalling(bb); gen_ok = True
        elif primary_input == "ps2":
            _gen_ps2(bb); gen_ok = True
        elif primary_input == "jtag":
            _gen_jtag(bb); gen_ok = True
        elif primary_input == "mdio":
            _gen_mdio(bb); gen_ok = True
        elif primary_input == "microwire":
            _gen_microwire(bb); gen_ok = True

    # Build stack config
    stack = []
    if is_stack_decoder and stack_ids:
        for sid in stack_ids:
            stack_entry = {"id": sid}
            # Parse stack decoder's channels for mapping
            stack_c_file = os.path.join(DECODERS_DIR, sid + ".c")
            if os.path.exists(stack_c_file):
                try:
                    _, _, stack_channels = parse_decoder_metadata(stack_c_file)
                    if stack_channels:
                        if "uart" in sid:
                            stack_entry["channels"] = {"rx": 0}
                        else:
                            stack_entry["channels"] = stack_channels
                except Exception:
                    pass
            stack.append(stack_entry)
    elif not is_stack_decoder:
        # For non-stack decoders with non-logic input, build stack from INPUT_GENERATORS
        primary_input = inputs[0] if inputs else "logic"
        current_input = primary_input
        while current_input in INPUT_GENERATORS:
            base_id, _ = INPUT_GENERATORS[current_input]
            if base_id == "base":
                break
            stack_c_file = os.path.join(DECODERS_DIR, base_id + ".c")
            stack_entry = {"id": base_id}
            if os.path.exists(stack_c_file):
                try:
                    _, _, stack_channels = parse_decoder_metadata(stack_c_file)
                    if stack_channels:
                        if "uart" in base_id:
                            stack_entry["channels"] = {"rx": 0}
                        else:
                            stack_entry["channels"] = stack_channels
                except Exception:
                    pass
            stack.insert(0, stack_entry)
            break  # Only 1 level for non-configured decoders

    # Save to testdata
    test_dir = os.path.join(TESTDATA_ROOT, d_id, "default")
    os.makedirs(test_dir, exist_ok=True)

    with open(os.path.join(test_dir, "input.bin"), "wb") as f:
        f.write(bb.get_bitpacked())

    config = {
        "decoder": d_id,
        "samplerate": samplerate,
        "num_channels": num_channels,
        "sample_count": sample_count,
        "channels": channels,
        "stack": stack
    }

    with open(os.path.join(test_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=2)

    status = "OK" if gen_ok else "FALLBACK"
    print(f"[{status:8}] {d_id:25} | ch={num_channels:2} | sr={samplerate:>10} | stack={len(stack)}")


def main():
    if not os.path.exists(DECODERS_DIR):
        print(f"Error: {DECODERS_DIR} not found.")
        return

    c_files = [f for f in os.listdir(DECODERS_DIR) if f.endswith("_c.c")]
    c_files.sort()

    print(f"Regenerating test data for {len(c_files)} decoders...")
    print("-" * 80)

    for f in c_files:
        generate_test_for_decoder(os.path.join(DECODERS_DIR, f))

    print("-" * 80)
    print("Done. All input.bin and config.json files have been updated.")


if __name__ == "__main__":
    main()
