/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2019 Marco Geisler <m-sigrok@mageis.de>
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_STROBE = 0,
    ANN_SINGLE_READ,
    ANN_SINGLE_WRITE,
    ANN_BURST_READ,
    ANN_BURST_WRITE,
    ANN_STATUS_READ,
    ANN_STATUS,
    ANN_WARN,
    NUM_ANN,
};

enum cc1101_cmd_type {
    CC1101_CMD_WRITE = 0,
    CC1101_CMD_BURST_WRITE,
    CC1101_CMD_READ,
    CC1101_CMD_BURST_READ,
    CC1101_CMD_STROBE,
    CC1101_CMD_STATUS_READ,
    CC1101_CMD_UNKNOWN,
};

#define MAX_BURST_BYTES 256

typedef struct {
    int first;
    int cmd_type;
    int cmd_addr;
    int min_bytes;
    int max_bytes;
    uint8_t mosi_mb[MAX_BURST_BYTES];
    uint8_t miso_mb[MAX_BURST_BYTES];
    int mb_count;
    uint64_t ss_mb, es_mb;
    int cs_was_released;
    int out_ann;
} cc1101_state;

static const struct { uint8_t addr; const char *name; } cc1101_regs[] = {
    {0x00, "IOCFG2"}, {0x01, "IOCFG1"}, {0x02, "IOCFG0"},
    {0x03, "FIFOTHR"}, {0x04, "SYNC1"}, {0x05, "SYNC0"},
    {0x06, "PKTLEN"}, {0x07, "PKTCTRL1"}, {0x08, "PKTCTRL0"},
    {0x09, "ADDR"}, {0x0A, "CHANNR"}, {0x0B, "FSCTRL1"},
    {0x0C, "FSCTRL0"}, {0x0D, "FREQ2"}, {0x0E, "FREQ1"},
    {0x0F, "FREQ0"}, {0x10, "MDMCFG4"}, {0x11, "MDMCFG3"},
    {0x12, "MDMCFG2"}, {0x13, "MDMCFG1"}, {0x14, "MDMCFG0"},
    {0x15, "DEVIATN"}, {0x16, "MCSM2"}, {0x17, "MCSM1"},
    {0x18, "MCSM0"}, {0x19, "FOCCFG"}, {0x1A, "BSCFG"},
    {0x1B, "AGCTRL2"}, {0x1C, "AGCTRL1"}, {0x1D, "AGCTRL0"},
    {0x1E, "WOREVT1"}, {0x1F, "WOREVT0"}, {0x20, "WORCTRL"},
    {0x21, "FREND1"}, {0x22, "FREND0"}, {0x23, "FSCAL3"},
    {0x24, "FSCAL2"}, {0x25, "FSCAL1"}, {0x26, "FSCAL0"},
    {0x27, "RCCTRL1"}, {0x28, "RCCTRL0"}, {0x29, "FSTEST"},
    {0x2A, "PTEST"}, {0x2B, "AGCTEST"}, {0x2C, "TEST2"},
    {0x2D, "TEST1"}, {0x2E, "TEST0"},
    {0xFF, NULL}
};

static const struct { uint8_t addr; const char *name; } cc1101_status_regs[] = {
    {0x30, "PARTNUM"}, {0x31, "VERSION"}, {0x32, "FREQEST"},
    {0x33, "LQI"}, {0x34, "RSSI"}, {0x35, "MARCSTATE"},
    {0x36, "WORTIME1"}, {0x37, "WORTIME0"}, {0x38, "PKTSTATUS"},
    {0x39, "VCO_VC_DAC"}, {0x3A, "TXBYTES"}, {0x3B, "RXBYTES"},
    {0x3C, "RCCTRL1_STATUS"}, {0x3D, "RCCTRL0_STATUS"},
    {0x3E, "PATABLE"}, {0x3F, "FIFO"},
    {0xFF, NULL}
};

static const struct { uint8_t addr; const char *name; } cc1101_strobes[] = {
    {0x30, "SRES"}, {0x31, "SFSTXON"}, {0x32, "SXOFF"},
    {0x33, "SCAL"}, {0x34, "SRX"}, {0x35, "STX"},
    {0x36, "SIDLE"}, {0x37, ""}, {0x38, "SWOR"},
    {0x39, "SPWD"}, {0x3A, "SFRX"}, {0x3B, "SFTX"},
    {0x3C, "SWORRST"}, {0x3D, "SNOP"},
    {0xFF, NULL}
};

static const char *cc1101_status_states[] = {
    "IDLE", "RX", "TX", "FSTXON", "CALIBRATE", "SETTLING",
    "RXFIFO_OVERFLOW", "TXFIFO_OVERFLOW"
};

static const char *cc1101_inputs[] = {"spi", NULL};
static const char *cc1101_tags[] = {"IC", "Wireless/RF", NULL};

static const char *cc1101_ann_labels[][3] = {
    {"", "strobe", "Command strobe"},
    {"", "single_read", "Single register read"},
    {"", "single_write", "Single register write"},
    {"", "burst_read", "Burst register read"},
    {"", "burst_write", "Burst register write"},
    {"", "status_read", "Status read"},
    {"", "status", "Status register"},
    {"", "warning", "Warning"},
};

static const int cc1101_row_cmd_classes[] = {ANN_STROBE, -1};
static const int cc1101_row_data_classes[] = {ANN_SINGLE_READ, ANN_SINGLE_WRITE, ANN_BURST_READ, ANN_BURST_WRITE, ANN_STATUS_READ, -1};
static const int cc1101_row_status_classes[] = {ANN_STATUS, -1};
static const int cc1101_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row cc1101_ann_rows[] = {
    {"cmd", "Commands", cc1101_row_cmd_classes, 1},
    {"data", "Data", cc1101_row_data_classes, 5},
    {"status", "Status register", cc1101_row_status_classes, 1},
    {"warnings", "Warnings", cc1101_row_warnings_classes, 1},
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

static int cc1101_parse_command(uint8_t b, int *addr, int *min_bytes, int *max_bytes)
{
    *addr = b & 0x3F;
    if (*addr < 0x30 || *addr == 0x3E || *addr == 0x3F) {
        switch (b & 0xC0) {
        case 0x00: *min_bytes = 1; *max_bytes = 1; return CC1101_CMD_WRITE;
        case 0x40: *min_bytes = 1; *max_bytes = MAX_BURST_BYTES; return CC1101_CMD_BURST_WRITE;
        case 0x80: *min_bytes = 1; *max_bytes = 1; return CC1101_CMD_READ;
        case 0xC0: *min_bytes = 1; *max_bytes = MAX_BURST_BYTES; return CC1101_CMD_BURST_READ;
        }
    } else {
        if ((b & 0x40) == 0x00) { *min_bytes = 0; *max_bytes = 0; return CC1101_CMD_STROBE; }
        if ((b & 0xC0) == 0xC0) { *min_bytes = 1; *max_bytes = MAX_BURST_BYTES; return CC1101_CMD_STATUS_READ; }
    }
    return CC1101_CMD_UNKNOWN;
}

static const char *cc1101_reg_name(int addr)
{
    if (addr < 0x30) {
        for (int i = 0; cc1101_regs[i].name; i++) {
            if (cc1101_regs[i].addr == addr)
                return cc1101_regs[i].name;
        }
    } else if (addr <= 0x3F) {
        for (int i = 0; cc1101_status_regs[i].name; i++) {
            if (cc1101_status_regs[i].addr == addr)
                return cc1101_status_regs[i].name;
        }
    }
    return "unknown";
}

static const char *cc1101_strobe_name(int addr)
{
    for (int i = 0; cc1101_strobes[i].name; i++) {
        if (cc1101_strobes[i].addr == addr)
            return cc1101_strobes[i].name;
    }
    return "unknown strobe";
}

static void cc1101_decode_status(struct srd_decoder_inst *di, cc1101_state *s,
    uint64_t ss, uint64_t es, uint8_t status, const char *label)
{
    char buf[512];
    const char *chip_rdy = (status & 0x80) ? "CHIP_RDYn is high! " : "";
    int state_idx = (status & 0x70) >> 4;
    int fifo = status & 0x0F;
    const char *fifo_dir = (s->cmd_type == CC1101_CMD_READ ||
                            s->cmd_type == CC1101_CMD_BURST_READ ||
                            s->cmd_type == CC1101_CMD_STATUS_READ)
                           ? "available in RX FIFO" : "free in TX FIFO";
    snprintf(buf, sizeof(buf), "%s = %02X; %sSTATE is %s, %d bytes %s",
             label, status, chip_rdy, cc1101_status_states[state_idx], fifo, fifo_dir);
    c_put(di, ss, es, s->out_ann, ANN_STATUS, buf);
}

static void cc1101_finish_command(struct srd_decoder_inst *di, cc1101_state *s)
{
    char buf[512];
    int pos = 0;
    const char *cmd_name = NULL;
    int ann = -1;
    int use_miso = 0;

    switch (s->cmd_type) {
    case CC1101_CMD_WRITE:
        cmd_name = "Write"; ann = ANN_SINGLE_WRITE; break;
    case CC1101_CMD_BURST_WRITE:
        cmd_name = "Burst write"; ann = ANN_BURST_WRITE; break;
    case CC1101_CMD_READ:
        cmd_name = "Read"; ann = ANN_SINGLE_READ; use_miso = 1; break;
    case CC1101_CMD_BURST_READ:
        cmd_name = "Burst read"; ann = ANN_BURST_READ; use_miso = 1; break;
    case CC1101_CMD_STROBE:
        cmd_name = "Strobe"; ann = ANN_STROBE; break;
    case CC1101_CMD_STATUS_READ:
        cmd_name = "Status read"; ann = ANN_STATUS_READ; use_miso = 1; break;
    default:
        c_put(di, s->ss_mb, s->es_mb, s->out_ann, ANN_WARN, "unhandled command");
        return;
    }

    if (s->cmd_type == CC1101_CMD_STROBE) {
        /* Strobe commands have no data bytes */
        return;
    }

    const char *reg_name = cc1101_reg_name(s->cmd_addr);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s: %s (%02X) = ",
                    cmd_name, reg_name, s->cmd_addr);

    for (int i = 0; i < s->mb_count && pos < (int)sizeof(buf) - 16; i++) {
        uint8_t b = use_miso ? s->miso_mb[i] : s->mosi_mb[i];
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X", b);
    }

    c_put(di, s->ss_mb, s->es_mb, s->out_ann, ann, buf);
}

static void cc1101_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    cc1101_state *s = (cc1101_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        int cs_old = -1, cs_new = -1;
        parse_cs_change(fields, n_fields, &cs_old, &cs_new);

        if (cs_old == -1 && cs_new == 1) {
            s->cs_was_released = 1;
        }

        if (cs_old == 0 && cs_new == 1) {
            /* Rising edge: process collected data */
            if (s->cmd_type != CC1101_CMD_UNKNOWN && s->mb_count > 0) {
                if (s->mb_count < s->min_bytes) {
                    c_put(di, start_sample, start_sample, s->out_ann, ANN_WARN, "missing data bytes");
                } else {
                    cc1101_finish_command(di, s);
                }
            }
            /* Reset for next command */
            s->first = 1;
            s->mb_count = 0;
            s->cmd_type = CC1101_CMD_UNKNOWN;
            s->cs_was_released = 1;
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0) return;
    if (!s->cs_was_released) return;

    int have_mosi = 0, have_miso = 0;
    uint8_t mosi = 0, miso = 0;
    parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi, &miso);

    if (!have_mosi || !have_miso) return;

    if (s->first) {
        s->first = 0;
        /* First MOSI byte is the command */
        int addr, min_b = 0, max_b = 0;
        s->cmd_type = cc1101_parse_command(mosi, &addr, &min_b, &max_b);
        s->cmd_addr = addr;
        s->min_bytes = min_b;
        s->max_bytes = max_b;

        if (s->cmd_type == CC1101_CMD_UNKNOWN) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, "unknown command");
            return;
        }

        /* First MISO byte is always the status register */
        char status_label[64];
        if (s->cmd_type == CC1101_CMD_STROBE) {
            snprintf(status_label, sizeof(status_label), "Status");
        } else {
            const char *reg_name = cc1101_reg_name(s->cmd_addr);
            const char *cmd_name = NULL;
            switch (s->cmd_type) {
            case CC1101_CMD_WRITE: cmd_name = "Write"; break;
            case CC1101_CMD_BURST_WRITE: cmd_name = "Burst write"; break;
            case CC1101_CMD_READ: cmd_name = "Read"; break;
            case CC1101_CMD_BURST_READ: cmd_name = "Burst read"; break;
            case CC1101_CMD_STATUS_READ: cmd_name = "Status read"; break;
            default: cmd_name = "Unknown"; break;
            }
            snprintf(status_label, sizeof(status_label), "%s: %s (%02X)",
                     cmd_name, reg_name, s->cmd_addr);
        }
        cc1101_decode_status(di, s, start_sample, end_sample, miso, status_label);

        if (s->cmd_type == CC1101_CMD_STROBE) {
            /* Strobe command: output immediately */
            char buf[256];
            const char *sname = cc1101_strobe_name(s->cmd_addr);
            snprintf(buf, sizeof(buf), "Strobe %s", sname);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_STROBE, buf);
        } else {
            /* Start collecting data bytes */
            s->ss_mb = start_sample;
            s->mb_count = 0;
        }
    } else {
        /* Subsequent bytes: data bytes */
        if (s->cmd_type == CC1101_CMD_UNKNOWN) return;

        if (s->mb_count >= s->max_bytes) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, "excess byte");
            return;
        }

        s->es_mb = end_sample;
        s->mosi_mb[s->mb_count] = mosi;
        s->miso_mb[s->mb_count] = miso;
        s->mb_count++;
    }
}

static void cc1101_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(cc1101_state)));
    }
    cc1101_state *s = (cc1101_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(cc1101_state));
    s->first = 1;
    s->cmd_type = CC1101_CMD_UNKNOWN;
}

static void cc1101_start(struct srd_decoder_inst *di)
{
    cc1101_state *s = (cc1101_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "cc1101");
}

static void cc1101_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void cc1101_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder cc1101_c_decoder = {
    .id = "cc1101_c",
    .name = "CC1101(C)",
    .longname = "Texas Instruments CC1101 (C)",
    .desc = "Low-power sub-1GHz RF transceiver chip. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = cc1101_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = cc1101_ann_rows,
    .inputs = cc1101_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = cc1101_tags,
    .num_tags = 2,
    .reset = cc1101_reset,
    .start = cc1101_start,
    .decode = cc1101_decode,
    .destroy = cc1101_destroy,
    .decode_upper = cc1101_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &cc1101_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}