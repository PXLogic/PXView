##
## Copyright (C) 2016 Soenke J. Peters
## Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
##
## This program is free software: you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation, either version 3 of the License, or
## (at your option) any later version.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##
## You should have received a copy of the GNU General Public License
## along with this program. If not, see <http://www.gnu.org/licenses/>.

try:
    from .regdecode import *
except SystemError:
    import sys
    sys.path.append('./')
    from regdecode import *

# http://www.cypress.com/file/136666/download#page=105
# http://www.cypress.com/file/126466/download#page=15
regs = {
#   addr: ('name',               size)
    0x00: ('CHANNEL_ADR',           1),
    0x01: ('TX_LENGTH_ADR',         1),
    0x02: ('TX_CTRL_ADR',           1),
    0x03: ('TX_CFG_ADR',            1),
    0x04: ('TX_IRQ_STATUS_ADR',     1),
    0x05: ('RX_CTRL_ADR',           1),
    0x06: ('RX_CFG_ADR',            1),
    0x07: ('RX_IRQ_STATUS_ADR',     1),
    0x08: ('RX_STATUS_ADR',         1),
    0x09: ('RX_COUNT_ADR',          1),
    0x0A: ('RX_LENGTH_ADR',         1),
    0x0B: ('PWR_CTRL_ADR',          1),
    0x0C: ('XTAL_CTRL_ADR',         1),
    0x0D: ('IO_CFG_ADR',            1),
    0x0E: ('GPIO_CTRL_ADR',         1),
    0x0F: ('XACT_CFG_ADR',          1),
    0x10: ('FRAMING_CFG_ADR',       1),
    0x11: ('DATA32_THOLD_ADR',      1),
    0x12: ('DATA64_THOLD_ADR',      1),
    0x13: ('RSSI_ADR',              1),
    0x14: ('EOP_CTRL_ADR',          1),
    0x15: ('CRC_SEED_LSB_ADR',      1),
    0x16: ('CRC_SEED_MSB_ADR',      1),
    0x17: ('TX_CRC_LSB_ADR',        1),
    0x18: ('TX_CRC_MSB_ADR',        1),
    0x19: ('RX_CRC_LSB_ADR',        1),
    0x1A: ('RX_CRC_MSB_ADR',        1),
    0x1B: ('TX_OFFSET_LSB_ADR',     1),
    0x1C: ('TX_OFFSET_MSB_ADR',     1),
    0x1D: ('MODE_OVERRIDE_ADR',     1),
    0x1E: ('RX_OVERRIDE_ADR',       1),
    0x1F: ('TX_OVERRIDE_ADR',       1),
    0x26: ('XTAL_CFG_ADR',          1),
    0x27: ('CLK_OFFSET_ADR',        1),
    0x28: ('CLK_EN_ADR',            1),
    0x29: ('RX_ABORT_ADR',          1),
    0x32: ('AUTO_CAL_TIME_ADR',     1),
    0x35: ('AUTO_CAL_OFFSET_ADR',   1),
    0x39: ('ANALOG_CTRL_ADR',       1),
    0x20: ('TX_BUFFER_ADR',        16),
    0x21: ('RX_BUFFER_ADR',        16),
    0x22: ('SOP_CODE_ADR',          8),
    0x23: ('DATA_CODE_ADR',        16),
    0x24: ('PREAMBLE_ADR',          3),
    0x25: ('MFG_ID_ADR',            6)
}

RegDecode(regs)

@RDecode
def reg_0x00(v = 0x48):
    CHANNEL_MSK = 0x7F
    CHANNEL_MAX = 0x62
    try:
        v = v & CHANNEL_MSK
    except TypeError:
        v = v[0] & CHANNEL_MSK

    if ((v % 3) == 0) and (v <= 96):
        t = "100us_fast"
    elif ((v % 2) == 0) and (v <= 94):
        t = "180us_medium"
    elif (v <= 97):
        t = "270us_slow"
    else:
        t = "not_valid"
    m = "CHANNEL {} ({}GHz, {})".format(v, ((2400+(v * 98/CHANNEL_MAX))/1000), t)
    if (v > CHANNEL_MAX):
        w = "Warn: Channel# > {}".format(CHANNEL_MAX)
    else:
        w = None
    if w is None:
        return m
    else:
        return m, w

# TODO: Programmatical approach for this kind of register decoding?
@RDecode
def reg_0x02(v = 0x03):
    bf = []
    if ((v & 1<<7) != 0): bf.append("TX_GO")
    if ((v & 1<<6) != 0): bf.append("TX_CLR")
    if ((v & 1<<5) != 0): bf.append("TXB15_IRQEN")
    if ((v & 1<<4) != 0): bf.append("TXB8_IRQEN")
    if ((v & 1<<3) != 0): bf.append("TXB0_IRQEN")
    if ((v & 1<<2) != 0): bf.append("TXBERR_IRQEN")
    if ((v & 1<<1) != 0): bf.append("TXC_IRQEN")
    if ((v & 1<<0) != 0): bf.append("TXE_IRQEN")
    return ' | '.join(bf)

@RDecode
def reg_0x03(v = 0x05, type="LP"):
    bf = []
    warn = []
    if ((v & 1<<5) != 0): bf.append("DATCODE_LEN_64")
    else: bf.append("DATCODE_LEN_32")

    if  ((v & 0x18) == 0x00): bf.append("DATMODE_1MBPS")
    elif ((v & 0x18) == 0x08): bf.append("DATMODE_8DR")
    elif ((v & 0x18) == 0x10):
        bf.append("DATMODE_DDR")
        if (type != "LP"):
            warn.append("DATMODE_DDR not supported on LPStar")
    elif ((v & 0x18) == 0x18):
        bf.append("DATMODE_SDR")
        if (type != "LP"):
            warn.append("DATMODE_SDR not supported on LPStar")
    if  ((v & 0x07) == 0x00): bf.append("PA_N30_DBM")
    elif ((v & 0x07) == 0x01): bf.append("PA_N25_DBM")
    elif ((v & 0x07) == 0x02): bf.append("PA_N20_DBM")
    elif ((v & 0x07) == 0x03): bf.append("PA_N15_DBM")
    elif ((v & 0x07) == 0x04): bf.append("PA_N10_DBM")
    elif ((v & 0x07) == 0x05): bf.append("PA_N5_DBM")
    elif ((v & 0x07) == 0x06): bf.append("PA_0_DBM")
    elif ((v & 0x07) == 0x07): bf.append("PA_4_DBM")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x04(v):
    bf = []
    if ((v & 1<<7) != 0): bf.append("XS_IRQ")
    if ((v & 1<<6) != 0): bf.append("LV_IRQ")
    if ((v & 1<<5) != 0): bf.append("TXB15_IRQ")
    if ((v & 1<<4) != 0): bf.append("TXB8_IRQ")
    if ((v & 1<<3) != 0): bf.append("TXB0_IRQ")
    if ((v & 1<<2) != 0): bf.append("TXBERR_IRQ")
    if ((v & 1<<1) != 0): bf.append("TXC_IRQ")
    if ((v & 1<<0) != 0): bf.append("TXE_IRQ")
    return ' | '.join(bf)

@RDecode
def reg_0x05(v = 0x07):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("RX_GO")
    if ((v & 1<<6) != 0): warn.append("RSVD bit #6 not 0")
    if ((v & 1<<5) != 0): bf.append("RXB16_IRQEN")
    if ((v & 1<<4) != 0): bf.append("RXB8_IRQEN")
    if ((v & 1<<3) != 0): bf.append("RXB1_IRQEN")
    if ((v & 1<<2) != 0): bf.append("RXBERR_IRQEN")
    if ((v & 1<<1) != 0): bf.append("RXC_IRQEN")
    if ((v & 1<<0) != 0): bf.append("RXE_IRQEN")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x06(v = 0x92):
    bf = []
    if ((v & 1<<7) != 0): bf.append("AUTO_AGC_EN")
    if ((v & 1<<6) != 0): bf.append("LNA_EN")
    if ((v & 1<<5) != 0): bf.append("ATT_EN")
    if ((v & 1<<4) != 0):
        bf.append("HI")
    else:
        bf.append("LO")
    if ((v & 1<<3) != 0): bf.append("FASTTURN_EN")
    if ((v & 1<<1) != 0): bf.append("RXOW_EN")
    if ((v & 1<<0) != 0): bf.append("VLD_EN")
    return ' | '.join(bf)

@RDecode
def reg_0x07(v):
    bf = []
    if ((v & 1<<7) != 0): bf.append("RXOW_IRQ")
    if ((v & 1<<6) != 0): bf.append("SOFTDET_IRQ")
    if ((v & 1<<5) != 0): bf.append("RXB16_IRQ")
    if ((v & 1<<4) != 0): bf.append("RXB8_IRQ")
    if ((v & 1<<3) != 0): bf.append("RXB1_IRQ")
    if ((v & 1<<2) != 0): bf.append("RXBERR_IRQ")
    if ((v & 1<<1) != 0): bf.append("RXC_IRQ")
    if ((v & 1<<0) != 0): bf.append("RXE_IRQ")

    return ' | '.join(bf)

@RDecode
def reg_0x08(v):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("RX_ACK")
    if ((v & 1<<6) != 0): bf.append("RX_PKTERR")
    if ((v & 1<<5) != 0): bf.append("RX_EOPERR")
    if ((v & 1<<4) != 0): bf.append("RX_CRC0")
    if ((v & 1<<3) != 0): bf.append("RX_BAD_CRC")
    if ((v & 1<<2) != 0): bf.append("DATCODE_LEN_64")
    else: bf.append("DATCODE_LEN_32")
    if  ((v & 0b11) == 0b00): bf.append("DATMODE_1MBPS")
    elif ((v & 0b11) == 0b01): bf.append("DATMODE_8DR")
    elif ((v & 0b11) == 0b10): bf.append("DATMODE_DDR")
    elif ((v & 0b11) == 0b11):
        bf.append("0b11")
        warn.append("Receive Data Mode 0b11 not valid")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x0B(v = 0xA0, type="LP"):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("PMU_EN")
    if ((v & 1<<6) != 0): bf.append("LV_IRQ_EN")
    if ((v & 1<<5) != 0): bf.append("PMU_MODE_FORCE")
    if ((v & 1<<4) != 0): bf.append("PFET_OFF")

    if  ((v & 0x0C) == 0x0C): bf.append("LV_IRQ_TH_1P8_V")
    elif  ((v & 0x0C) == 0x08): bf.append("LV_IRQ_TH_2P0_V")
    elif  ((v & 0x0C) == 0x04): bf.append("LV_IRQ_TH_2P2_V")
    elif  ((v & 0x0C) == 0x00): bf.append("LV_IRQ_TH_PMU_OUTV")

    if  ((v & 0x03) == 0x03): bf.append("PMU_OUTV_2P4")
    elif  ((v & 0x03) == 0x02): bf.append("PMU_OUTV_2P5")
    elif  ((v & 0x03) == 0x01): bf.append("PMU_OUTV_2P6")
    elif  ((v & 0x03) == 0x00): bf.append("PMU_OUTV_2P7")

    if type == "LPstar" and v != 0b00010000:
        warn.append("The firmware should set 0b00010000 to this register while initiating")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x0C(v = 0x04):
    bf = []
    warn = []

    if  ((v & 0xC0) == 0x00): bf.append("XOUT_FNC_XOUT_FREQ")
    elif  ((v & 0xC0) == 0x40): bf.append("XOUT_FNC_PA_N")
    elif  ((v & 0xC0) == 0x80): bf.append("XOUT_FNC_RAD_STREAM")
    elif  ((v & 0xC0) == 0xC0): bf.append("XOUT_FNC_GPIO")

    if ((v & 1<<5) != 0): bf.append("XS_IRQ_EN")

    if  ((v & 0x07) == 0x00): bf.append("XOUT_FREQ_12MHZ")
    elif  ((v & 0x07) == 0x01): bf.append("XOUT_FREQ_6MHZ")
    elif  ((v & 0x07) == 0x02): bf.append("XOUT_FREQ_3MHZ")
    elif  ((v & 0x07) == 0x03): bf.append("XOUT_FREQ_1P5MHZ")
    elif  ((v & 0x07) == 0x04): bf.append("XOUT_FREQ_P75MHZ")
    else:
        bf.append("0b{0:b}".format(v&0x07))
        warn.append("Frequency setting 0b{0:b} not defined".format(v&0x07))

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x0D(v = 0x00, type = "LP"):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("IRQ_OD")
    if ((v & 1<<6) != 0): bf.append("IRQ_POL")
    if ((v & 1<<5) != 0): bf.append("MISO_OD")
    if ((v & 1<<4) != 0): bf.append("XOUT_OD")
    if ((v & 1<<3) != 0):
        bf.append("PACTL_OD")
        if type == "LPstar":
            warn.append("For LPstar bit #3 is reserved")
    if ((v & 1<<2) != 0):
        bf.append("PACTL_GPIO")
        if type == "LPstar":
            warn.append("For LPstar bit #2 is reserved")
    if ((v & 1<<1) != 0): bf.append("SPI_3_PIN")
    if ((v & 1<<0) != 0): bf.append("IRQ_GPIO")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x0E(v = 0x00, type = "LP"):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("XOUT_OP")
    if ((v & 1<<6) != 0): bf.append("MISO_OP")
    if ((v & 1<<5) != 0):
        bf.append("PACTL_OP")
        if type == "LPstar":
            warn.append("For LPstar bit #5 is reserved")
    if ((v & 1<<4) != 0): bf.append("IRQ_OP")
    if ((v & 1<<3) != 0): bf.append("XOUT_IP")
    if ((v & 1<<2) != 0): bf.append("MISO_IP")
    if ((v & 1<<1) != 0):
        bf.append("PACTL_IP")
        if type == "LPstar":
            warn.append("For LPstar bit #1 is reserved")
    if ((v & 1<<0) != 0): bf.append("IRQ_IP")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x0F(v = 0x80):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("ACK_EN")
    if ((v & 1<<5) != 0): bf.append("FRC_END_STATE")

    if  ((v & 0x1C) == 0x00): bf.append("END_STATE_SLEEP")
    elif  ((v & 0x1C) == 0x04): bf.append("END_STATE_IDLE")
    elif  ((v & 0x1C) == 0x08): bf.append("END_STATE_TXSYNTH")
    elif  ((v & 0x1C) == 0x0C): bf.append("END_STATE_RXSYNTH")
    elif  ((v & 0x1C) == 0x10): bf.append("END_STATE_RX")
    else:
        warn.append("Transaction End State 0b{0:b} not defined".format(v&0x1C))

    if  ((v & 0x03) == 0x00): bf.append("ACK_TO_4X")
    elif  ((v & 0x03) == 0x01): bf.append("ACK_TO_8X")
    elif  ((v & 0x03) == 0x02): bf.append("ACK_TO_12X")
    elif  ((v & 0x03) == 0x03): bf.append("ACK_TO_15X")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x10(v = 0xA5):
    bf = []
    warn = []
    SOP_THRESH = (v & 0x1F)
    if ((v & 1<<7) != 0): bf.append("SOP_EN")
    if ((v & 1<<6) != 0):
        bf.append("SOP_LEN")
        if SOP_THRESH != 0x0E:
            warn.append("Typical applications configure SOP_THRESH = 0x0E for SOP64")
    else:
        if (SOP_THRESH | 1) != (0x04 | 1): # When SOP_LEN is cleared, the most significant bit is disregarded
            warn.append("Typical applications configure SOP_THRESH = 0x04 for SOP32")
    if ((v & 1<<5) != 0): bf.append("LEN_EN")

    bf.append("SOP_THRESH_{0:#04x}".format(SOP_THRESH))

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x11(v = 0x04):
    return v & 0x0f

@RDecode
def reg_0x12(v = 0x0a):
    return v & 0x1f

@RDecode
def reg_0x13(v = 0x20):
    bf = []
    if ((v & 1<<7) != 0): bf.append("SOP_RSSI")
    if ((v & 1<<5) != 0): bf.append("LNA_STATE")

    bf.append("RSSI_LVL_{0:#04x}".format(v & 0x1f))

    return ' | '.join(bf)

@RDecode
def reg_0x14(v = 0xA4):
    bf = []
    warn = []
    HINT = (v & 0x70) >> 4
    EOP = (v & 0x0f)
    if ((v & 1<<7) != 0):
        bf.append("HINT_EN")
        if HINT == 0:
            warn.append("EOP Hint Symbol Count cannot be 0")

    bf.append("HINT_{}".format(HINT))
    bf.append("EOP_{}".format(EOP))

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x1C(v = 0x00):
    return v & 0x0f

@RDecode
def reg_0x1D(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<7) != 0):
        bf.append("RSVD_DIS_AUTO_SEN") # TODO: Verify name
        warn.append("bit #7 RSVD, must be 0")
    if ((v & 1<<6) != 0):
        bf.append("RSVD_SEN_TXRXB") # TODO: Verify name
        warn.append("bit #6 RSVD, must be 0")
    if ((v & 1<<5) != 0): bf.append("FRC_SEN")

    if ((v & 0x18) == 0x18):
        bf.append("FRC_AWAKE")
    elif ((v & 0x18) == 0x08):
        bf.append("FRC_AWAKE_OFF_1")
    elif ((v & 0x18) == 0x00):
        bf.append("FRC_AWAKE_OFF_2")
    else:
        bf.append("FRC_AWAKE_OFF_X") # TODO: check docu
        warn.append("Unknown FRC_AWAKE mode")

    if ((v & 1<<0) != 0): bf.append("RST")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x1E(v = 0x00):
    bf = []
    if ((v & 1<<7) != 0): bf.append("ACK_RX")
    if ((v & 1<<6) != 0): bf.append("RXTX_DLY") # aka EXTEND_RX_TX
    if ((v & 1<<5) != 0): bf.append("MAN_RXACK")
    if ((v & 1<<4) != 0): bf.append("FRC_RXDR")
    if ((v & 1<<3) != 0): bf.append("DIS_CRC0")
    if ((v & 1<<2) != 0): bf.append("DIS_RXCRC")
    if ((v & 1<<1) != 0): bf.append("ACE")

    return ' | '.join(bf)

@RDecode
def reg_0x1F(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<7) != 0): bf.append("ACK_TX_SEN")
    if ((v & 1<<6) != 0): bf.append("FRX_PREAMBLE")
    if ((v & 1<<5) != 0):
        bf.append("RSVD_DIS_TX_RETRANS") # TODO: Verify name
        warn.append("bit #5 RSVD, must be 0")
    if ((v & 1<<4) != 0): bf.append("MAN_TXACK")
    if ((v & 1<<3) != 0): bf.append("OVRRD_ACK")
    if ((v & 1<<2) != 0): bf.append("DIS_TXRC")
    if ((v & 1<<1) != 0):
        bf.append("RSVD_CO") # TODO: Verify name
        warn.append("bit #1 RSVD, must be 0")
    if ((v & 1<<0) != 0): bf.append("TXINV")

    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x26(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<3) != 0): bf.append("START_DLY")
    elif (v != 0):
        warn.append("bits #7:4,2:0 RSVD, must be 0")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x27(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<1) != 0): bf.append("RXF")
    elif (v != 0):
        warn.append("bits #7:2,0 RSVD, must be 0")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x28(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<1) != 0): bf.append("RXF")
    elif (v != 0):
        warn.append("bits #7:2,0 RSVD, must be 0")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x29(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<5) != 0): bf.append("ABORT_EN")
    elif (v != 0):
        warn.append("bits #7:6,4:0 RSVD, must be 0")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x32(v = 0x0C):
    bf = []
    warn = []
    if (v == 0x3C):
        bf.append("AUTO_CAL_TIME_MAX")
    else:
        bf.append("{0:#0x}".format(v))
        warn.append("Firmware MUST write 0x3C to this register during initialization.")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w


@RDecode
def reg_0x35(v = 0x00):
    bf = []
    warn = []
    if (v == 0x14):
        bf.append("AUTO_CAL_OFFSET_MINUS_4")
    else:
        bf.append("{0:#0x}".format(v))
        warn.append("Firmware MUST write 0x14 to this register during initialization.")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w

@RDecode
def reg_0x39(v = 0x00):
    bf = []
    warn = []
    if ((v & 1<<1) != 0): bf.append("RX_INV")
    if ((v & 1<<0) != 0): bf.append("ALL_SLOW")
    if ((v & 0b11111100) != 0):
        warn.append("bits #7:2 RSVD, must be 0")
    w = ""
    if len(warn) > 0:
        w = "Warn: "
        w += '; '.join(warn)

    return ' | '.join(bf), w


if __name__ == '__main__':
    # Test code follows:
    print(RegDecode.decode(0x00, 0x48))
    print(RegDecode.valid(0xff))
    print(RegDecode.width(0x20))
