/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 Soenke J. Peters
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_WRITE = 0,
    ANN_READ,
    ANN_TX_DATA,
    ANN_RX_DATA,
    ANN_STATE,
    ANN_WARN,
    ANN_WAIT,
    NUM_ANN,
};

#define MAX_REG_WIDTH 16

typedef struct {
    int first;
    int addr;
    int dir_wr;   /* 1 = write, 0 = read */
    int inc;      /* 1 = auto increment */
    int reg_width;
    uint8_t mosi_mb[MAX_REG_WIDTH];
    uint8_t miso_mb[MAX_REG_WIDTH];
    int mb_count;
    uint64_t mb_s, mb_e;
    int cs_was_released;
    int spi3pin;
    double delaysplit;
    uint64_t samplerate;
    uint64_t wait_s, wait_e;
    int out_ann;
    int out_binary;
} cyrf6936_state;

static const struct { uint8_t addr; const char *name; int width; } cyrf6936_regs[] = {
    {0x00, "CHANNEL_ADR", 1}, {0x01, "TX_LENGTH_ADR", 1},
    {0x02, "TX_CTRL_ADR", 1}, {0x03, "TX_CFG_ADR", 1},
    {0x04, "TX_IRQ_STATUS_ADR", 1}, {0x05, "RX_CTRL_ADR", 1},
    {0x06, "RX_CFG_ADR", 1}, {0x07, "RX_IRQ_STATUS_ADR", 1},
    {0x08, "RX_STATUS_ADR", 1}, {0x09, "RX_COUNT_ADR", 1},
    {0x0A, "RX_LENGTH_ADR", 1}, {0x0B, "PWR_CTRL_ADR", 1},
    {0x0C, "XTAL_CTRL_ADR", 1}, {0x0D, "IO_CFG_ADR", 1},
    {0x0E, "GPIO_CTRL_ADR", 1}, {0x0F, "XACT_CFG_ADR", 1},
    {0x10, "FRAMING_CFG_ADR", 1}, {0x11, "DATA32_THOLD_ADR", 1},
    {0x12, "DATA64_THOLD_ADR", 1}, {0x13, "RSSI_ADR", 1},
    {0x14, "EOP_CTRL_ADR", 1}, {0x15, "CRC_SEED_LSB_ADR", 1},
    {0x16, "CRC_SEED_MSB_ADR", 1}, {0x17, "TX_CRC_LSB_ADR", 1},
    {0x18, "TX_CRC_MSB_ADR", 1}, {0x19, "RX_CRC_LSB_ADR", 1},
    {0x1A, "RX_CRC_MSB_ADR", 1}, {0x1B, "TX_OFFSET_LSB_ADR", 1},
    {0x1C, "TX_OFFSET_MSB_ADR", 1}, {0x1D, "MODE_OVERRIDE_ADR", 1},
    {0x1E, "RX_OVERRIDE_ADR", 1}, {0x1F, "TX_OVERRIDE_ADR", 1},
    {0x20, "TX_BUFFER_ADR", 16}, {0x21, "RX_BUFFER_ADR", 16},
    {0x22, "SOP_CODE_ADR", 8}, {0x23, "DATA_CODE_ADR", 16},
    {0x24, "PREAMBLE_ADR", 3}, {0x25, "MFG_ID_ADR", 6},
    {0x26, "XTAL_CFG_ADR", 1}, {0x27, "CLK_OFFSET_ADR", 1},
    {0x28, "CLK_EN_ADR", 1}, {0x29, "RX_ABORT_ADR", 1},
    {0x32, "AUTO_CAL_TIME_ADR", 1}, {0x35, "AUTO_CAL_OFFSET_ADR", 1},
    {0x39, "ANALOG_CTRL_ADR", 1},
    {0xFF, NULL, 0}
};

static const char *cyrf6936_inputs[] = {"spi", NULL};
static const char *cyrf6936_outputs[] = {"cyrf6936", NULL};
static const char *cyrf6936_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option cyrf6936_options[] = {
    {"spi3pin", "dec_cyrf6936_opt_spi3pin",
     "SPI 3-pin mode with MOSI/MISO combined as SDAT on the MOSI pin",
     NULL, NULL},
    {"delaysplit", "dec_cyrf6936_opt_delaysplit",
     "annotate delays (in us) larger than... (0 = off)",
     NULL, NULL},
    {"invert_mosi", "dec_cyrf6936_opt_invert_mosi",
     "Invert MOSI",
     NULL, NULL},
    {"invert_miso", "dec_cyrf6936_opt_invert_miso",
     "Invert MISO",
     NULL, NULL},
};

static const char *cyrf6936_ann_labels[][3] = {
    {"", "write", "Write"},
    {"", "read", "Read"},
    {"", "tx-data", "Payload sent to the device"},
    {"", "rx-data", "Payload read from the device"},
    {"", "state", "State change"},
    {"", "warning", "Warnings"},
    {"", "wait", "Wait"},
};

static const int cyrf6936_row_cmd_classes[] = {ANN_WRITE, ANN_READ, ANN_TX_DATA, ANN_RX_DATA, -1};
static const int cyrf6936_row_warnings_classes[] = {ANN_STATE, ANN_WARN, -1};
static const int cyrf6936_row_delays_classes[] = {ANN_WAIT, -1};

static const struct srd_c_ann_row cyrf6936_ann_rows[] = {
    {"cmd", "Commands", cyrf6936_row_cmd_classes, 4},
    {"warnings", "Warnings", cyrf6936_row_warnings_classes, 2},
    {"delays", "Delays", cyrf6936_row_delays_classes, 1},
};

static const struct srd_decoder_binary cyrf6936_binary[] = {
    {0, "txpayload", "Transfer payload"},
    {1, "rxpayload", "Receive payload"},
};

static void parse_spi_data(const c_field *fields, int n_fields,
    int *have_mosi, int *have_miso, uint8_t *mosi_byte, uint8_t *miso_byte)
{
    if (n_fields < 1) return;
    *have_mosi = (fields[0].u8 & 1) ? 1 : 0;
    *have_miso = (fields[0].u8 & 2) ? 1 : 0;
    uint64_t mv = 0, sv = 0;
    if (n_fields >= 9) {
        for (int i = 0; i < 8; i++)
            mv |= ((uint64_t)fields[1 + i].u8) << (8 * i);
    }
    if (n_fields >= 17) {
        for (int i = 0; i < 8; i++)
            sv |= ((uint64_t)fields[9 + i].u8) << (8 * i);
    }
    *mosi_byte = (uint8_t)mv;
    *miso_byte = (uint8_t)sv;
}

static void parse_cs_change(const c_field *fields, int n_fields,
    int *cs_old, int *cs_new)
{
    *cs_old = (n_fields > 0) ? (int)fields[0].u8 : -1;
    *cs_new = (n_fields > 1) ? (int)fields[1].u8 : -1;
    if (*cs_old == 0xFF) *cs_old = -1;
}

static const char *cyrf6936_reg_name(int addr)
{
    for (int i = 0; cyrf6936_regs[i].name; i++) {
        if (cyrf6936_regs[i].addr == addr)
            return cyrf6936_regs[i].name;
    }
    return NULL;
}

static int cyrf6936_reg_width(int addr)
{
    for (int i = 0; cyrf6936_regs[i].name; i++) {
        if (cyrf6936_regs[i].addr == addr)
            return cyrf6936_regs[i].width;
    }
    return 0;
}

static int cyrf6936_reg_valid(int addr)
{
    return cyrf6936_reg_name(addr) != NULL;
}

/* Register decode functions for key registers */
static const char *cyrf6936_decode_reg(uint8_t addr, const uint8_t *data, int count __attribute__((unused)),
    char *warn_buf, int warn_buf_size)
{
    static char decode_buf[256];
    warn_buf[0] = '\0';

    switch (addr) {
    case 0x00: { /* CHANNEL_ADR */
        uint8_t v = data[0];
        uint8_t channel = v & 0x7F;
        const char *speed_type;
        if ((channel % 3 == 0) && channel <= 96)
            speed_type = "100us_fast";
        else if ((channel % 2 == 0) && channel <= 94)
            speed_type = "180us_medium";
        else if (channel <= 97)
            speed_type = "270us_slow";
        else
            speed_type = "not_valid";
        double freq_ghz = (2400.0 + (channel * 98.0 / 0x62)) / 1000.0;
        snprintf(decode_buf, sizeof(decode_buf), "CHANNEL %d (%.3fGHz, %s)", channel, freq_ghz, speed_type);
        if (channel > 0x62)
            snprintf(warn_buf, warn_buf_size, "Warn: Channel# > %d", 0x62);
        return decode_buf;
    }
    case 0x02: { /* TX_CTRL_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTX_GO", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTX_CLR", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXB15_IRQEN", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXB8_IRQEN", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXB0_IRQEN", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXBERR_IRQEN", pos ? " | " : "");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXC_IRQEN", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXE_IRQEN", pos ? " | " : "");
        return decode_buf;
    }
    case 0x03: { /* TX_CFG_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDATCODE_LEN_64", pos ? " | " : "");
        else pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDATCODE_LEN_32", pos ? " | " : "");
        int dm = v & 0x18;
        if (dm == 0x00) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_1MBPS");
        else if (dm == 0x08) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_8DR");
        else if (dm == 0x10) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_DDR");
        else if (dm == 0x18) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_SDR");
        int pa = v & 0x07;
        const char *pa_names[] = {"PA_N30_DBM","PA_N25_DBM","PA_N20_DBM","PA_N15_DBM","PA_N10_DBM","PA_N5_DBM","PA_0_DBM","PA_4_DBM"};
        pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | %s", pa_names[pa]);
        return decode_buf;
    }
    case 0x04: { /* TX_IRQ_STATUS_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sXS_IRQ", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sLV_IRQ", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXB15_IRQ", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXB8_IRQ", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXB0_IRQ", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXBERR_IRQ", pos ? " | " : "");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXC_IRQ", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXE_IRQ", pos ? " | " : "");
        return decode_buf;
    }
    case 0x05: { /* RX_CTRL_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_GO", pos ? " | " : "");
        if (v & (1 << 6)) { pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRSVD bit #6 not 0", pos ? " | " : "");
            snprintf(warn_buf, warn_buf_size, "Warn: RSVD bit #6 not 0"); }
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXB16_IRQEN", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXB8_IRQEN", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXB1_IRQEN", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXBERR_IRQEN", pos ? " | " : "");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXC_IRQEN", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXE_IRQEN", pos ? " | " : "");
        return decode_buf;
    }
    case 0x06: { /* RX_CFG_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sAUTO_AGC_EN", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sLNA_EN", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sATT_EN", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sHI", pos ? " | " : "");
        else pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sLO", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | FASTTURN_EN");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | RXOW_EN");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | VLD_EN");
        return decode_buf;
    }
    case 0x07: { /* RX_IRQ_STATUS_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXOW_IRQ", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sSOFTDET_IRQ", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXB16_IRQ", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXB8_IRQ", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXB1_IRQ", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXBERR_IRQ", pos ? " | " : "");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXC_IRQ", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXE_IRQ", pos ? " | " : "");
        return decode_buf;
    }
    case 0x08: { /* RX_STATUS_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_ACK", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_PKTERR", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_EOPERR", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_CRC0", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_BAD_CRC", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDATCODE_LEN_64", pos ? " | " : "");
        else pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDATCODE_LEN_32", pos ? " | " : "");
        int dm = v & 0x03;
        if (dm == 0) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_1MBPS");
        else if (dm == 1) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_8DR");
        else if (dm == 2) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | DATMODE_DDR");
        else { pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | 0b11");
            snprintf(warn_buf, warn_buf_size, "Warn: Receive Data Mode 0b11 not valid"); }
        return decode_buf;
    }
    case 0x0D: { /* IO_CFG_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sIRQ_OD", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sIRQ_POL", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sMISO_OD", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sXOUT_OD", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sPACTL_OD", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sPACTL_GPIO", pos ? " | " : "");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sSPI_3_PIN", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sIRQ_GPIO", pos ? " | " : "");
        return decode_buf;
    }
    case 0x0F: { /* XACT_CFG_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sACK_EN", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRNULL_STATE", pos ? " | " : "");
        int es = v & 0x1C;
        if (es == 0x00) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sEND_STATE_SLEEP", pos ? " | " : "");
        else if (es == 0x04) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sEND_STATE_IDLE", pos ? " | " : "");
        else if (es == 0x08) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sEND_STATE_TXSYNTH", pos ? " | " : "");
        else if (es == 0x0C) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sEND_STATE_RXSYNTH", pos ? " | " : "");
        else if (es == 0x10) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sEND_STATE_RX", pos ? " | " : "");
        int ack_to = v & 0x03;
        const char *ack_names[] = {"ACK_TO_4X","ACK_TO_8X","ACK_TO_12X","ACK_TO_15X"};
        pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, " | %s", ack_names[ack_to]);
        return decode_buf;
    }
    case 0x1D: { /* MODE_OVERRIDE_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) { pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRSVD_DIS_AUTO_SEN", pos ? " | " : "");
            snprintf(warn_buf, warn_buf_size, "Warn: bit #7 RSVD, must be 0"); }
        if (v & (1 << 6)) { pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRSVD_SEN_TXRXB", pos ? " | " : "");
            if (warn_buf[0] == '\0') snprintf(warn_buf, warn_buf_size, "Warn: bit #6 RSVD, must be 0"); }
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRC_SEN", pos ? " | " : "");
        int fa = v & 0x18;
        if (fa == 0x18) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRC_AWAKE", pos ? " | " : "");
        else if (fa == 0x08) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRC_AWAKE_OFF_1", pos ? " | " : "");
        else if (fa == 0x00) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRC_AWAKE_OFF_2", pos ? " | " : "");
        else pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRC_AWAKE_OFF_X", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRST", pos ? " | " : "");
        return decode_buf;
    }
    case 0x1E: { /* RX_OVERRIDE_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sACK_RX", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRXTX_DLY", pos ? " | " : "");
        if (v & (1 << 5)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sMAN_RXACK", pos ? " | " : "");
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRC_RXDR", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDIS_CRC0", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDIS_RXCRC", pos ? " | " : "");
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sACE", pos ? " | " : "");
        return decode_buf;
    }
    case 0x1F: { /* TX_OVERRIDE_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 7)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sACK_TX_SEN", pos ? " | " : "");
        if (v & (1 << 6)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sFRX_PREAMBLE", pos ? " | " : "");
        if (v & (1 << 5)) { pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRSVD_DIS_TX_RETRANS", pos ? " | " : "");
            snprintf(warn_buf, warn_buf_size, "Warn: bit #5 RSVD, must be 0"); }
        if (v & (1 << 4)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sMAN_TXACK", pos ? " | " : "");
        if (v & (1 << 3)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sOVRRD_ACK", pos ? " | " : "");
        if (v & (1 << 2)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sDIS_TXRC", pos ? " | " : "");
        if (v & (1 << 1)) { pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRSVD_CO", pos ? " | " : "");
            if (warn_buf[0] == '\0') snprintf(warn_buf, warn_buf_size, "Warn: bit #1 RSVD, must be 0"); }
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sTXINV", pos ? " | " : "");
        return decode_buf;
    }
    case 0x32: { /* AUTO_CAL_TIME_ADR */
        uint8_t v = data[0];
        if (v == 0x3C) {
            snprintf(decode_buf, sizeof(decode_buf), "AUTO_CAL_TIME_MAX");
        } else {
            snprintf(decode_buf, sizeof(decode_buf), "0x%02x", v);
            snprintf(warn_buf, warn_buf_size, "Warn: Firmware MUST write 0x3C to this register during initialization.");
        }
        return decode_buf;
    }
    case 0x35: { /* AUTO_CAL_OFFSET_ADR */
        uint8_t v = data[0];
        if (v == 0x14) {
            snprintf(decode_buf, sizeof(decode_buf), "AUTO_CAL_OFFSET_MINUS_4");
        } else {
            snprintf(decode_buf, sizeof(decode_buf), "0x%02x", v);
            snprintf(warn_buf, warn_buf_size, "Warn: Firmware MUST write 0x14 to this register during initialization.");
        }
        return decode_buf;
    }
    case 0x39: { /* ANALOG_CTRL_ADR */
        uint8_t v = data[0];
        int pos = 0;
        decode_buf[0] = '\0';
        if (v & (1 << 1)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sRX_INV", pos ? " | " : "");
        if (v & (1 << 0)) pos += snprintf(decode_buf + pos, sizeof(decode_buf) - pos, "%sALL_SLOW", pos ? " | " : "");
        if (v & 0xFC) snprintf(warn_buf, warn_buf_size, "Warn: bits #7:2 RSVD, must be 0");
        return decode_buf;
    }
    default:
        return NULL;
    }
}

static void cyrf6936_format_command(cyrf6936_state *s, const char *textdata, char *buf, int buf_size)
{
    const char *reg = cyrf6936_reg_name(s->addr);
    if (!reg) reg = "unknown";
    const char *multi = s->inc ? "_inc" : "";

    if (s->dir_wr) {
        if (textdata)
            snprintf(buf, buf_size, "write%s(%s, \"%s\")", multi, reg, textdata);
        else
            snprintf(buf, buf_size, "write%s(%s)", multi, reg);
    } else {
        if (textdata)
            snprintf(buf, buf_size, "read%s(%s) == \"%s\"", multi, reg, textdata);
        else
            snprintf(buf, buf_size, "read%s(%s)", multi, reg);
    }
}

static void cyrf6936_finish_command(struct srd_decoder_inst *di, cyrf6936_state *s)
{
    if (s->mb_count == 0) return;

    /* Binary output for TX/RX buffers */
    if (s->dir_wr && s->addr == 0x20) {
        /* TX_BUFFER_ADR: output binary tx payload */
        uint8_t bindata[MAX_REG_WIDTH] = {0};
        int w = cyrf6936_reg_width(s->addr);
        for (int i = 0; i < s->mb_count && i < w; i++)
            bindata[i] = s->mosi_mb[i];
        c_put_bin(di, s->mb_s, s->mb_e, s->out_binary, 0,
                             s->mb_count < w ? s->mb_count : w, bindata);
    } else if (!s->dir_wr && s->addr == 0x21) {
        /* RX_BUFFER_ADR: output binary rx payload */
        uint8_t bindata[MAX_REG_WIDTH] = {0};
        int w = cyrf6936_reg_width(s->addr);
        const uint8_t *src = s->spi3pin ? s->mosi_mb : s->miso_mb;
        for (int i = 0; i < s->mb_count && i < w; i++)
            bindata[i] = src[i];
        c_put_bin(di, s->mb_s, s->mb_e, s->out_binary, 1,
                             s->mb_count < w ? s->mb_count : w, bindata);
    }

    /* Decode register value */
    char warn_buf[256] = "";
    const char *decoded = cyrf6936_decode_reg(s->addr, s->dir_wr ? s->mosi_mb : s->miso_mb,
                                               s->mb_count, warn_buf, sizeof(warn_buf));

    char cmd_buf[2048];
    cyrf6936_format_command(s, decoded, cmd_buf, sizeof(cmd_buf));

    c_put(di, s->mb_s, s->mb_e, s->out_ann,
              s->dir_wr ? ANN_WRITE : ANN_READ, cmd_buf);

    if (warn_buf[0] != '\0') {
        c_put(di, s->mb_s, s->mb_e, s->out_ann, ANN_WARN, warn_buf);
    }
}

static void cyrf6936_next(cyrf6936_state *s)
{
    if (s->inc)
        s->addr++;
    s->mb_count = 0;
    s->mb_s = (uint64_t)-1;
    s->mb_e = 0;
}

static void cyrf6936_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    cyrf6936_state *s = (cyrf6936_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        int cs_old = -1, cs_new = -1;
        parse_cs_change(fields, n_fields, &cs_old, &cs_new);

        if (cs_old == -1 && cs_new == -1) return;
        if (cs_old == -1 && cs_new == 1) {
            s->cs_was_released = 1;
        }

        if (cs_old == 0 && cs_new == 1) {
            /* Rising edge: process collected data */
            if (s->addr >= 0 && s->mb_count > 0 && s->mb_s != (uint64_t)-1) {
                cyrf6936_finish_command(di, s);
            }
            s->wait_s = end_sample;
            /* Reset for next command */
            s->first = 1;
            s->mb_count = 0;
            s->addr = -1;
            s->cs_was_released = 1;
        }

        if (cs_old == 1 && cs_new == 0) {
            /* Falling edge: start of pause, calculate delay */
            s->wait_e = start_sample;
            if (s->delaysplit > 0 && s->samplerate > 0 && s->wait_e > s->wait_s) {
                double dt = ((double)(s->wait_e - s->wait_s) * 1000000.0) / (double)s->samplerate;
                if (dt >= s->delaysplit) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "delay_us(%.1f)", dt);
                    c_put(di, s->wait_s, s->wait_e, s->out_ann, ANN_WAIT, buf);
                }
            }
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0) return;
    if (!s->cs_was_released) return;

    int have_mosi = 0, have_miso = 0;
    uint8_t mosi = 0, miso = 0;
    parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi, &miso);

    if (!have_mosi) return;

    if (s->first) {
        s->first = 0;
        /* First MOSI byte is the command */
        s->addr = mosi & 0x3F;
        s->dir_wr = (mosi & 0x80) >> 7;
        s->inc = (mosi & 0x40) >> 6;
        s->mb_s = start_sample;

        /* Check if register is valid */
        if (!cyrf6936_reg_valid(s->addr)) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, "unknown address/register");
        }

        /* First MISO byte is discarded, but check for unexpected data */
        if (have_miso && miso != 0xFF && miso != 0x00) {
            char buf[64];
            snprintf(buf, sizeof(buf), "unrequested data 0x%02x", miso);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, buf);
        }
    } else {
        /* Data byte */
        int max_w = cyrf6936_reg_width(s->addr);

        if (s->mb_count >= max_w) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, "excess byte");
            return;
        }

        if (s->mb_s == (uint64_t)-1)
            s->mb_s = start_sample;
        s->mb_e = end_sample;
        s->mosi_mb[s->mb_count] = mosi;
        s->miso_mb[s->mb_count] = miso;
        s->mb_count++;

        if (s->mb_count >= max_w) {
            cyrf6936_finish_command(di, s);
            cyrf6936_next(s);
        }

        /* IO_CFG_ADR (0x0D) special handling: detect SPI 3-pin mode */
        if (s->addr == 0x0D) {
            uint8_t b;
            if (!s->dir_wr && s->spi3pin == 0)
                b = miso;
            else
                b = mosi;
            int old_spi3pin = s->spi3pin;
            if (b & 0x02)
                s->spi3pin = 1;
            else
                s->spi3pin = 0;
            if (s->spi3pin != old_spi3pin) {
                const char *msg = s->spi3pin ? "3-pin SPI mode (SDAT)" : "4-pin SPI mode (MOSI/MISO)";
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATE, msg);
            }
        }

        if (s->inc) {
            cyrf6936_next(s);
        }
    }
}

static void cyrf6936_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(cyrf6936_state)));
    }
    cyrf6936_state *s = (cyrf6936_state *)c_decoder_get_private(di);
    int saved_spi3pin = s->spi3pin;
    double saved_delaysplit = s->delaysplit;
    uint64_t saved_samplerate = s->samplerate;
    int saved_cs_released = s->cs_was_released;
    int saved_out_ann = s->out_ann;
    int saved_out_binary = s->out_binary;

    memset(s, 0, sizeof(cyrf6936_state));
    s->first = 1;
    s->addr = -1;
    s->spi3pin = saved_spi3pin;
    s->delaysplit = saved_delaysplit;
    s->samplerate = saved_samplerate;
    s->cs_was_released = saved_cs_released;
    s->out_ann = saved_out_ann;
    s->out_binary = saved_out_binary;
}

static void cyrf6936_start(struct srd_decoder_inst *di)
{
    cyrf6936_state *s = (cyrf6936_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "cyrf6936");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "cyrf6936");

    const char *spi3pin_str = c_opt_str(di, "spi3pin", "no");
    s->spi3pin = (strcmp(spi3pin_str, "yes") == 0) ? 1 : 0;

    s->delaysplit = c_opt_dbl(di, "delaysplit", 0.0);
}

static void cyrf6936_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    cyrf6936_state *s = (cyrf6936_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void cyrf6936_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void cyrf6936_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder cyrf6936_c_decoder = {
    .id = "cyrf6936_c",
    .name = "CYRF6936(C)",
    .longname = "Cypress CYRF6936 WirelessUSB LP 2.4 GHz Radio SoC (C)",
    .desc = "2.4GHz transceiver chip. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = cyrf6936_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = cyrf6936_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = cyrf6936_ann_rows,
    .inputs = cyrf6936_inputs,
    .num_inputs = 1,
    .outputs = cyrf6936_outputs,
    .num_outputs = 1,
    .binary = cyrf6936_binary,
    .num_binary = 2,
    .tags = cyrf6936_tags,
    .num_tags = 1,
    .reset = cyrf6936_reset,
    .start = cyrf6936_start,
    .decode = cyrf6936_decode,
    .destroy = cyrf6936_destroy,
    .metadata = cyrf6936_metadata,
    .decode_upper = cyrf6936_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    cyrf6936_options[0].def = g_variant_new_string("no");
    GSList *spi3pin_vals = NULL;
    spi3pin_vals = g_slist_append(spi3pin_vals, g_variant_new_string("no"));
    spi3pin_vals = g_slist_append(spi3pin_vals, g_variant_new_string("yes"));
    cyrf6936_options[0].values = spi3pin_vals;

    cyrf6936_options[1].def = g_variant_new_double(0.0);

    cyrf6936_options[2].def = g_variant_new_string("no");
    GSList *invert_mosi_vals = NULL;
    invert_mosi_vals = g_slist_append(invert_mosi_vals, g_variant_new_string("yes"));
    invert_mosi_vals = g_slist_append(invert_mosi_vals, g_variant_new_string("no"));
    cyrf6936_options[2].values = invert_mosi_vals;

    cyrf6936_options[3].def = g_variant_new_string("no");
    GSList *invert_miso_vals = NULL;
    invert_miso_vals = g_slist_append(invert_miso_vals, g_variant_new_string("yes"));
    invert_miso_vals = g_slist_append(invert_miso_vals, g_variant_new_string("no"));
    cyrf6936_options[3].values = invert_miso_vals;

    return &cyrf6936_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}