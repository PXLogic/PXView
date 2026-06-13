/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2019 Jiahao Li <reg@ljh.me>
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_RCR = 0,
    ANN_RBM,
    ANN_WCR,
    ANN_WBM,
    ANN_BFS,
    ANN_BFC,
    ANN_SRC,
    ANN_DATA,
    ANN_REG_ADDR,
    ANN_WARNING,
    NUM_ANN,
};

#define OPCODE_MASK   0xE0
#define REG_ADDR_MASK 0x1F
#define REG_ADDR_ECON1 0x1F
#define BIT_ECON1_BSEL0 0x01
#define BIT_ECON1_BSEL1 0x02

#define MAX_BYTES 256

typedef struct {
    int out_ann;
    uint8_t mosi[MAX_BYTES];
    uint8_t miso[MAX_BYTES];
    uint64_t ranges_ss[MAX_BYTES];
    uint64_t ranges_es[MAX_BYTES];
    int byte_count;
    uint64_t cmd_ss;
    uint64_t cmd_es;
    int active;
    int bsel0;
    int bsel1;
    int bsel_known;
} enc28j60_state;

static const char *enc28j60_inputs[] = {"spi", NULL};
static const char *enc28j60_tags[] = {"Embedded/industrial", "Networking", NULL};

static const char *enc28j60_ann_labels[][3] = {
    {"", "rcr", "Read Control Register"},
    {"", "rbm", "Read Buffer Memory"},
    {"", "wcr", "Write Control Register"},
    {"", "wbm", "Write Buffer Memory"},
    {"", "bfs", "Bit Field Set"},
    {"", "bfc", "Bit Field Clear"},
    {"", "src", "System Reset Command"},
    {"", "data", "Data"},
    {"", "reg-addr", "Register Address"},
    {"", "warning", "Warning"},
};

static const int enc28j60_row_commands_classes[] = {
    ANN_RCR, ANN_RBM, ANN_WCR, ANN_WBM, ANN_BFS, ANN_BFC, ANN_SRC, -1
};
static const int enc28j60_row_fields_classes[] = { ANN_DATA, ANN_REG_ADDR, -1 };
static const int enc28j60_row_warnings_classes[] = { ANN_WARNING, -1 };

static const struct srd_c_ann_row enc28j60_ann_rows[] = {
    {"commands", "Commands", enc28j60_row_commands_classes, 7},
    {"fields", "Fields", enc28j60_row_fields_classes, 2},
    {"warnings", "Warnings", enc28j60_row_warnings_classes, 1},
};

/* Bank 0-3 register name tables (4 banks x 32 registers) */
static const char *enc28j60_regs[4][32] = {
    { /* Bank 0 */
        "ERDPTL", "ERDPTH", "EWRPTL", "EWRPTH", "ETXSTL", "ETXSTH",
        "ETXNDL", "ETXNDH", "ERXSTL", "ERXSTH", "ERXNDL", "ERXNDH",
        "ERXRDPTL", "ERXRDPTH", "ERXWRPTL", "ERXWRPTH", "EDMASTL",
        "EDMASTH", "EDMANDL", "EDMANDH", "EDMADSTL", "EDMADSTH",
        "EDMACSL", "EDMACSH", "\xe2\x80\x94", "\xe2\x80\x94",
        "Reserved", "EIE", "EIR", "ESTAT", "ECON2", "ECON1",
    },
    { /* Bank 1 */
        "EHT0", "EHT1", "EHT2", "EHT3", "EHT4", "EHT5", "EHT6", "EHT7",
        "EPMM0", "EPMM1", "EPMM2", "EPMM3", "EPMM4", "EPMM5", "EPMM6", "EPMM7",
        "EPMCSL", "EPMCSH", "\xe2\x80\x94", "\xe2\x80\x94",
        "EPMOL", "EPMOH", "Reserved", "Reserved",
        "ERXFCON", "EPKTCNT", "Reserved", "EIE", "EIR", "ESTAT", "ECON2", "ECON1",
    },
    { /* Bank 2 */
        "MACON1", "Reserved", "MACON3", "MACON4", "MABBIPG", "\xe2\x80\x94",
        "MAIPGL", "MAIPGH", "MACLCON1", "MACLCON2", "MAMXFLL", "MAMXFLH",
        "Reserved", "Reserved", "Reserved", "\xe2\x80\x94",
        "Reserved", "Reserved", "MICMD", "\xe2\x80\x94",
        "MIREGADR", "Reserved", "MIWRL", "MIWRH", "MIRDL", "MIRDH",
        "Reserved", "EIE", "EIR", "ESTAT", "ECON2", "ECON1",
    },
    { /* Bank 3 */
        "MAADR5", "MAADR6", "MAADR3", "MAADR4", "MAADR1", "MAADR2",
        "EBSTSD", "EBSTCON", "EBSTCSL", "EBSTCSH", "MISTAT", "\xe2\x80\x94",
        "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94",
        "\xe2\x80\x94", "\xe2\x80\x94", "EREVID", "\xe2\x80\x94",
        "\xe2\x80\x94", "ECOCON", "Reserved", "EFLOCON", "EPAUSL", "EPAUSH",
        "Reserved", "EIE", "EIR", "ESTAT", "ECON2", "ECON1",
    },
};

static const char *enc28j60_get_reg_name(enc28j60_state *s, int reg_addr)
{
    if (!s->bsel_known)
        return NULL;
    int bank = (s->bsel1 << 1) + s->bsel0;
    return enc28j60_regs[bank][reg_addr];
}

static void enc28j60_put_data_byte(struct srd_decoder_inst *di, enc28j60_state *s,
    uint8_t data, int byte_index, int binary)
{
    uint64_t rss = s->ranges_ss[byte_index];
    uint64_t res;
    if (byte_index == s->byte_count - 1)
        res = s->cmd_es;
    else
        res = s->ranges_ss[byte_index + 1];

    if (binary) {
        char buf[16];
        buf[0] = '0'; buf[1] = 'b';
        for (int i = 7; i >= 0; i--)
            buf[2 + (7 - i)] = (data & (1 << i)) ? '1' : '0';
        buf[10] = '\0';
        c_put(di, rss, res, s->out_ann, ANN_DATA, buf);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "Data 0x%02X", data);
        c_put_v(di, rss, res, s->out_ann, ANN_DATA, data, buf);
    }
}

static void enc28j60_put_register_header(struct srd_decoder_inst *di, enc28j60_state *s)
{
    int reg_addr = s->mosi[0] & REG_ADDR_MASK;
    const char *reg_name = enc28j60_get_reg_name(s, reg_addr);

    uint64_t rss = s->cmd_ss;
    uint64_t res = s->ranges_ss[1];

    if (reg_name == NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Reg Bank ? Addr 0x%02X", reg_addr);
        c_put_v(di, rss, res, s->out_ann, ANN_REG_ADDR, reg_addr, buf);
        c_put(di, rss, res, s->out_ann, ANN_WARNING,
            "Warning: Register bank not known yet.", "Warning");
    } else {
        c_put(di, rss, res, s->out_ann, ANN_REG_ADDR, reg_name);

        if (strcmp(reg_name, "\xe2\x80\x94") == 0 || strcmp(reg_name, "Reserved") == 0) {
            c_put(di, rss, res, s->out_ann, ANN_WARNING,
                "Warning: Invalid register accessed.", "Warning");
        }
    }
}

static void enc28j60_process_rcr(struct srd_decoder_inst *di, enc28j60_state *s)
{
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_RCR,
        "Read Control Register", "RCR");

    if (s->byte_count != 2 && s->byte_count != 3) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid command length.", "Warning");
        return;
    }

    enc28j60_put_register_header(di, s);

    int reg_addr = s->mosi[0] & REG_ADDR_MASK;
    const char *reg_name = enc28j60_get_reg_name(s, reg_addr);

    if (reg_name == NULL) {
        /* Can't tell if MAC/MII register or not, trust user */
    } else {
        if (reg_name[0] == 'M' && s->byte_count != 3) {
            c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
                "Warning: Attempting to read a MAC/MII register without using the dummy byte.",
                "Warning");
            return;
        }
        if (reg_name[0] != 'M' && s->byte_count != 2) {
            c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
                "Warning: Attempting to read a non-MAC/MII register using the dummy byte.",
                "Warning");
            return;
        }
    }

    if (s->byte_count == 2) {
        enc28j60_put_data_byte(di, s, s->miso[1], 1, 0);
    } else {
        /* Dummy byte */
        uint64_t rss = s->ranges_ss[1];
        uint64_t res = s->ranges_ss[2];
        c_put(di, rss, res, s->out_ann, ANN_DATA, "Dummy Byte", "Dummy");
        enc28j60_put_data_byte(di, s, s->miso[2], 2, 0);
    }
}

static void enc28j60_process_rbm(struct srd_decoder_inst *di, enc28j60_state *s)
{
    if (s->mosi[0] != 0x3A) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid header byte.", "Warning");
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Read Buffer Memory: Length %d", s->byte_count - 1);
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_RBM, buf, "RBM");

    for (int i = 1; i < s->byte_count; i++) {
        enc28j60_put_data_byte(di, s, s->miso[i], i, 0);
    }
}

static void enc28j60_process_wcr(struct srd_decoder_inst *di, enc28j60_state *s)
{
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WCR,
        "Write Control Register", "WCR");

    if (s->byte_count != 2) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid command length.", "Warning");
        return;
    }

    enc28j60_put_register_header(di, s);
    enc28j60_put_data_byte(di, s, s->mosi[1], 1, 0);

    if ((s->mosi[0] & REG_ADDR_MASK) == REG_ADDR_ECON1) {
        s->bsel0 = (s->mosi[1] & BIT_ECON1_BSEL0) >> 0;
        s->bsel1 = (s->mosi[1] & BIT_ECON1_BSEL1) >> 1;
        s->bsel_known = 1;
    }
}

static void enc28j60_process_wbm(struct srd_decoder_inst *di, enc28j60_state *s)
{
    if (s->mosi[0] != 0x7A) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid header byte.", "Warning");
        return;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "Write Buffer Memory: Length %d", s->byte_count - 1);
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WBM, buf, "WBM");

    for (int i = 1; i < s->byte_count; i++) {
        enc28j60_put_data_byte(di, s, s->mosi[i], i, 0);
    }
}

static void enc28j60_process_bfs(struct srd_decoder_inst *di, enc28j60_state *s)
{
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_BFS,
        "Bit Field Set", "BFS");

    if (s->byte_count != 2) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid command length.", "Warning");
        return;
    }

    enc28j60_put_register_header(di, s);
    enc28j60_put_data_byte(di, s, s->mosi[1], 1, 1);

    if ((s->mosi[0] & REG_ADDR_MASK) == REG_ADDR_ECON1) {
        if (s->mosi[1] & BIT_ECON1_BSEL0)
            s->bsel0 = 1;
        if (s->mosi[1] & BIT_ECON1_BSEL1)
            s->bsel1 = 1;
        s->bsel_known = 1;
    }
}

static void enc28j60_process_bfc(struct srd_decoder_inst *di, enc28j60_state *s)
{
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_BFC,
        "Bit Field Clear", "BFC");

    if (s->byte_count != 2) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid command length.", "Warning");
        return;
    }

    enc28j60_put_register_header(di, s);
    enc28j60_put_data_byte(di, s, s->mosi[1], 1, 1);

    if ((s->mosi[0] & REG_ADDR_MASK) == REG_ADDR_ECON1) {
        if (s->mosi[1] & BIT_ECON1_BSEL0)
            s->bsel0 = 0;
        if (s->mosi[1] & BIT_ECON1_BSEL1)
            s->bsel1 = 0;
        s->bsel_known = 1;
    }
}

static void enc28j60_process_src(struct srd_decoder_inst *di, enc28j60_state *s)
{
    c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_SRC,
        "System Reset Command", "SRC");

    if (s->byte_count != 1) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Invalid command length.", "Warning");
        return;
    }

    s->bsel0 = 0;
    s->bsel1 = 0;
    s->bsel_known = 1;
}

static void enc28j60_process_command(struct srd_decoder_inst *di, enc28j60_state *s)
{
    if (s->byte_count == 0) {
        s->active = 0;
        return;
    }

    uint8_t header = s->mosi[0];
    uint8_t opcode = header & OPCODE_MASK;

    switch (opcode) {
    case 0x00: enc28j60_process_rcr(di, s); break;
    case 0x20: enc28j60_process_rbm(di, s); break;
    case 0x40: enc28j60_process_wcr(di, s); break;
    case 0x60: enc28j60_process_wbm(di, s); break;
    case 0x80: enc28j60_process_bfs(di, s); break;
    case 0xA0: enc28j60_process_bfc(di, s); break;
    case 0xE0: enc28j60_process_src(di, s); break;
    default:
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_WARNING,
            "Warning: Unknown opcode.", "Warning");
        break;
    }
    s->active = 0;
}

static void enc28j60_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    enc28j60_state *s = (enc28j60_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        int new_cs = (fields && n_fields >= 2) ? fields[1].u8 : 0;
        if (new_cs == 0) {
            s->active = 1;
            s->cmd_ss = start_sample;
            s->byte_count = 0;
        } else {
            if (s->active) {
                s->cmd_es = end_sample;
                enc28j60_process_command(di, s);
                s->active = 0;
            }
        }
    } else if (strcmp(cmd, "DATA") == 0) {
        if (!s->active) return;
        if (n_fields < 17) return;

        int have_mosi = (fields[0].u8 & 1) ? 1 : 0;
        int have_miso = (fields[0].u8 & 2) ? 1 : 0;
        uint8_t mosi_byte = 0, miso_byte = 0;
        if (have_mosi)
            mosi_byte = fields[1].u8;
        if (have_miso)
            miso_byte = fields[9].u8;

        if (s->byte_count < MAX_BYTES) {
            s->mosi[s->byte_count] = mosi_byte;
            s->miso[s->byte_count] = miso_byte;
            s->ranges_ss[s->byte_count] = start_sample;
            s->ranges_es[s->byte_count] = end_sample;
            s->byte_count++;
        }
    }
}

static void enc28j60_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(enc28j60_state)));
    }
    enc28j60_state *s = (enc28j60_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(enc28j60_state));
}

static void enc28j60_start(struct srd_decoder_inst *di)
{
    enc28j60_state *s = (enc28j60_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "enc28j60");
}

static void enc28j60_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void enc28j60_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder enc28j60_c_decoder = {
    .id = "enc28j60_c",
    .name = "ENC28J60(C)",
    .longname = "Microchip ENC28J60 (C)",
    .desc = "Microchip ENC28J60 10Base-T Ethernet controller protocol. (C implementation)",
    .license = "mit",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = enc28j60_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = enc28j60_ann_rows,
    .inputs = enc28j60_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = enc28j60_tags,
    .num_tags = 2,
    .reset = enc28j60_reset,
    .start = enc28j60_start,
    .decode = enc28j60_decode,
    .destroy = enc28j60_destroy,
    .decode_upper = enc28j60_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &enc28j60_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}