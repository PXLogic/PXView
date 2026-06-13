/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010-2014 Uwe Hermann <uwe@hermann-uwe.de>
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
    ANN_REG_0X00 = 0,
    ANN_REG_0X01,
    ANN_REG_0X02,
    ANN_REG_0X03,
    ANN_REG_0X04,
    ANN_REG_0X05,
    ANN_BIT_BZ,
    ANN_BIT_BC,
    ANN_BIT_AX,
    ANN_BIT_AY,
    ANN_BIT_AZ,
    ANN_NUNCHUK_WRITE,
    ANN_CMD_INIT,
    ANN_SUMMARY,
    ANN_WARNINGS,
    NUM_ANN,
};

enum nunchuk_state {
    NUNCHUK_IDLE,
    NUNCHUK_GET_SLAVE_ADDR,
    NUNCHUK_READ_REGS,
    NUNCHUK_WRITE_REGS,
};

typedef struct {
    enum nunchuk_state state;
    int reg;
    int sx, sy, ax, ay, az, bz, bc;
    uint8_t init_seq[2];
    int init_seq_count;
    
    uint64_t ss_block, es_block;
    int out_ann;
    uint64_t ss;
    uint64_t es;
} nunchuk_priv;

static const char *nunchuk_inputs[] = {"i2c", NULL};
static const char *nunchuk_tags[] = {"Sensor", NULL};

static const char *nunchuk_ann_labels[][3] = {
    {"", "reg-0x00", "Register 0x00 — Analog stick X"},
    {"", "reg-0x01", "Register 0x01 — Analog stick Y"},
    {"", "reg-0x02", "Register 0x02 — Accelerometer X[9:2]"},
    {"", "reg-0x03", "Register 0x03 — Accelerometer Y[9:2]"},
    {"", "reg-0x04", "Register 0x04 — Accelerometer Z[9:2]"},
    {"", "reg-0x05", "Register 0x05 — Buttons + accel LSB"},
    {"", "bit-bz", "BZ bit — Z button status"},
    {"", "bit-bc", "BC bit — C button status"},
    {"", "bit-ax", "AX bits — Accelerometer X[1:0]"},
    {"", "bit-ay", "AY bits — Accelerometer Y[1:0]"},
    {"", "bit-az", "AZ bits — Accelerometer Z[1:0]"},
    {"", "nunchuk-write", "Nunchuk write"},
    {"", "cmd-init", "Init command"},
    {"", "summary", "Summary"},
    {"", "warnings", "Warnings"},
};

static const int nunchuk_row_regs_classes[] = {
    ANN_REG_0X00, ANN_REG_0X01, ANN_REG_0X02, ANN_REG_0X03,
    ANN_REG_0X04, ANN_REG_0X05, ANN_BIT_BZ, ANN_BIT_BC,
    ANN_BIT_AX, ANN_BIT_AY, ANN_BIT_AZ, ANN_NUNCHUK_WRITE, ANN_CMD_INIT
};
static const int nunchuk_row_summary_classes[] = {ANN_SUMMARY};
static const int nunchuk_row_warnings_classes[] = {ANN_WARNINGS};

static const struct srd_c_ann_row nunchuk_ann_rows[] = {
    {"regs", "Registers", nunchuk_row_regs_classes, 13},
    {"summary", "Summary", nunchuk_row_summary_classes, 1},
    {"warnings", "Warnings", nunchuk_row_warnings_classes, 1},
};

static void nunchuk_handle_reg_0x00(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    s->ss_block = s->ss;
    s->sx = b;
    char buf[64], buf2[32];
    snprintf(buf, sizeof(buf), "Analog stick X position: 0x%02X", s->sx);
    snprintf(buf2, sizeof(buf2), "SX: 0x%02X", s->sx);
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0X00, buf, buf2);
}

static void nunchuk_handle_reg_0x01(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    s->sy = b;
    char buf[64], buf2[32];
    snprintf(buf, sizeof(buf), "Analog stick Y position: 0x%02X", s->sy);
    snprintf(buf2, sizeof(buf2), "SY: 0x%02X", s->sy);
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0X01, buf, buf2);
}

static void nunchuk_handle_reg_0x02(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    s->ax = b << 2;
    char buf[64], buf2[32];
    snprintf(buf, sizeof(buf), "Accelerometer X value bits[9:2]: 0x%03X", s->ax);
    snprintf(buf2, sizeof(buf2), "AX[9:2]: 0x%03X", s->ax);
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0X02, buf, buf2);
}

static void nunchuk_handle_reg_0x03(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    s->ay = b << 2;
    char buf[64], buf2[32];
    snprintf(buf, sizeof(buf), "Accelerometer Y value bits[9:2]: 0x%03X", s->ay);
    snprintf(buf2, sizeof(buf2), "AY[9:2]: 0x%03X", s->ay);
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0X03, buf, buf2);
}

static void nunchuk_handle_reg_0x04(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    s->az = b << 2;
    char buf[64], buf2[32];
    snprintf(buf, sizeof(buf), "Accelerometer Z value bits[9:2]: 0x%03X", s->az);
    snprintf(buf2, sizeof(buf2), "AZ[9:2]: 0x%03X", s->az);
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0X04, buf, buf2);
}

static void nunchuk_handle_reg_0x05(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    s->es_block = s->es;
    s->bz = (b >> 0) & 1;
    s->bc = (b >> 1) & 1;
    int ax_rest = (b >> 2) & 3;
    int ay_rest = (b >> 4) & 3;
    int az_rest = (b >> 6) & 3;
    s->ax |= ax_rest;
    s->ay |= ay_rest;
    s->az |= az_rest;

    char buf[64], buf2[32];
    const char *bz_s = (s->bz == 0) ? "" : "not ";
    snprintf(buf, sizeof(buf), "Z: %spressed", bz_s);
    snprintf(buf2, sizeof(buf2), "BZ: %d", s->bz);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_BZ, buf, buf2);

    const char *bc_s = (s->bc == 0) ? "" : "not ";
    snprintf(buf, sizeof(buf), "C: %spressed", bc_s);
    snprintf(buf2, sizeof(buf2), "BC: %d", s->bc);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_BC, buf, buf2);

    snprintf(buf, sizeof(buf), "Accelerometer X value bits[1:0]: 0x%X", ax_rest);
    snprintf(buf2, sizeof(buf2), "AX[1:0]: 0x%X", ax_rest);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_AX, buf, buf2);

    snprintf(buf, sizeof(buf), "Accelerometer Y value bits[1:0]: 0x%X", ay_rest);
    snprintf(buf2, sizeof(buf2), "AY[1:0]: 0x%X", ay_rest);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_AY, buf, buf2);

    snprintf(buf, sizeof(buf), "Accelerometer Z value bits[1:0]: 0x%X", az_rest);
    snprintf(buf2, sizeof(buf2), "AZ[1:0]: 0x%X", az_rest);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_AZ, buf, buf2);

    s->reg = 0x00;
}

static void nunchuk_handle_reg(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    switch (s->reg) {
    case 0x00: nunchuk_handle_reg_0x00(di, s, b); break;
    case 0x01: nunchuk_handle_reg_0x01(di, s, b); break;
    case 0x02: nunchuk_handle_reg_0x02(di, s, b); break;
    case 0x03: nunchuk_handle_reg_0x03(di, s, b); break;
    case 0x04: nunchuk_handle_reg_0x04(di, s, b); break;
    case 0x05: nunchuk_handle_reg_0x05(di, s, b); break;
    default: break;
    }
    if (s->reg < 0x05)
        s->reg++;
}

static void nunchuk_output_full_block_if_possible(struct srd_decoder_inst *di, nunchuk_priv *s)
{
    if (s->sx == -1 || s->sy == -1 || s->ax == -1 || s->ay == -1 ||
        s->az == -1 || s->bz == -1 || s->bc == -1)
        return;
    const char *bz = (s->bz == 0) ? "pressed" : "not pressed";
    const char *bc = (s->bc == 0) ? "pressed" : "not pressed";
    char buf[256];
    snprintf(buf, sizeof(buf),
        "Analog stick: %d/%d, accelerometer: %d/%d/%d, Z: %s, C: %s",
        s->sx, s->sy, s->ax, s->ay, s->az, bz, bc);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_SUMMARY, buf);
}

static void nunchuk_handle_reg_write(struct srd_decoder_inst *di, nunchuk_priv *s, uint8_t b)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "Nunchuk write: 0x%02X", b);
    c_put(di, s->ss, s->es, s->out_ann, ANN_NUNCHUK_WRITE, buf);
    if (s->init_seq_count < 2)
        s->init_seq[s->init_seq_count++] = b;
}

static void nunchuk_output_init_seq(struct srd_decoder_inst *di, nunchuk_priv *s)
{
    if (s->init_seq_count != 2) {
        char buf[64];
        snprintf(buf, sizeof(buf),
            "Init sequence was %d bytes long (2 expected)", s->init_seq_count);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_WARNINGS, buf);
        return;
    }
    if (s->init_seq[0] != 0x40 || s->init_seq[1] != 0x00) {
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_WARNINGS,
                  "Unknown init sequence (expected: 0x40 0x00)");
        return;
    }
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_CMD_INIT,
              "Initialize Nunchuk", "Init Nunchuk", "Init", "I");
}

static void nunchuk_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    nunchuk_priv *s = (nunchuk_priv *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (s->state == NUNCHUK_IDLE) {
        if (strcmp(cmd, "START") == 0) {
            s->state = NUNCHUK_GET_SLAVE_ADDR;
            s->ss_block = start_sample;
        }
    } else if (s->state == NUNCHUK_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            s->state = NUNCHUK_READ_REGS;
        } else if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            s->state = NUNCHUK_WRITE_REGS;
        }
    } else if (s->state == NUNCHUK_READ_REGS) {
        if (strcmp(cmd, "DATA READ") == 0) {
            nunchuk_handle_reg(di, s, databyte);
        } else if (strcmp(cmd, "STOP") == 0) {
            s->es_block = end_sample;
            nunchuk_output_full_block_if_possible(di, s);
            s->sx = s->sy = s->ax = s->ay = s->az = -1;
            s->bz = s->bc = -1;
            s->state = NUNCHUK_IDLE;
        }
    } else if (s->state == NUNCHUK_WRITE_REGS) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            nunchuk_handle_reg_write(di, s, databyte);
        } else if (strcmp(cmd, "STOP") == 0) {
            s->es_block = end_sample;
            nunchuk_output_init_seq(di, s);
            s->init_seq_count = 0;
            s->state = NUNCHUK_IDLE;
        }
    }
}

static void nunchuk_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(nunchuk_priv)));
    }
    nunchuk_priv *s = (nunchuk_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(nunchuk_priv));
    s->state = NUNCHUK_IDLE;
    s->sx = s->sy = s->ax = s->ay = s->az = -1;
    s->bz = s->bc = -1;
    s->reg = 0x00;
}

static void nunchuk_start(struct srd_decoder_inst *di)
{
    nunchuk_priv *s = (nunchuk_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "nunchuk");
}

static void nunchuk_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void nunchuk_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder nunchuk_c_decoder = {
    .id = "nunchuk_c",
    .name = "Nunchuk(C)",
    .longname = "Nintendo Wii Nunchuk (C)",
    .desc = "Nintendo Wii Nunchuk controller protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = nunchuk_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = nunchuk_ann_rows,
    .inputs = nunchuk_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = nunchuk_tags,
    .num_tags = 1,
    .reset = nunchuk_reset,
    .start = nunchuk_start,
    .decode = nunchuk_decode,
    .destroy = nunchuk_destroy,
    .decode_upper = nunchuk_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &nunchuk_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}