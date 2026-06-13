/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
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
    ANN_TEXT = 0,
    NUM_ANN,
};

enum mxc6225xu_state {
    MXC6225XU_IDLE,
    MXC6225XU_GET_SLAVE_ADDR,
    MXC6225XU_GET_REG_ADDR,
    MXC6225XU_WRITE_REGS,
    MXC6225XU_READ_REGS,
    MXC6225XU_READ_REGS2,
};

#define MXC6225XU_I2C_ADDRESS 0x2A

typedef struct {
    enum mxc6225xu_state state;
    int reg;
    
    int out_ann;
    uint64_t ss;
    uint64_t es;
} mxc6225xu_priv;

static const char *mxc6225xu_inputs[] = {"i2c", NULL};
static const char *mxc6225xu_tags[] = {"IC", "Sensor", NULL};

static const char *mxc6225xu_ann_labels[][3] = {
    {"", "text", "Human-readable text"},
};

static const int mxc6225xu_row_text_classes[] = {ANN_TEXT};
static const struct srd_c_ann_row mxc6225xu_ann_rows[] = {
    {"text", "Human-readable text", mxc6225xu_row_text_classes, 1},
};

static const char *mxc6225xu_sh_str[] = {
    "none", "shake left", "shake right", "undefined"
};
static const char *mxc6225xu_ori_str[] = {
    "vertical in upright orientation",
    "rotated 90 degrees clockwise",
    "vertical in inverted orientation",
    "rotated 90 degrees counterclockwise",
};
static const char *mxc6225xu_shth_str[] = {"0.5g", "1.0g", "1.5g", "2.0g"};
static const char *mxc6225xu_shc_str[] = {"16", "32", "64", "128"};
static const char *mxc6225xu_orc_str[] = {"16", "32", "64", "128"};

static void mxc6225xu_handle_reg_0x00(struct srd_decoder_inst *di, mxc6225xu_priv *s, uint8_t b)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "XOUT: %d", (int8_t)b);
    c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT, buf);
}

static void mxc6225xu_handle_reg_0x01(struct srd_decoder_inst *di, mxc6225xu_priv *s, uint8_t b)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "YOUT: %d", (int8_t)b);
    c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT, buf);
}

static void mxc6225xu_handle_reg_0x02(struct srd_decoder_inst *di, mxc6225xu_priv *s, uint8_t b)
{
    char buf[512];
    int pos = 0;

    int int_val = (b >> 7) & 1;
    const char *int_s = (int_val == 0) ? "unchanged and no" : "changed or";
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "INT = %d: Orientation %s shake event occurred\n", int_val, int_s);

    int sh = (((b >> 6) & 1) << 1) | ((b >> 5) & 1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "SH[1:0] = %d: Shake event: %s\n", sh, mxc6225xu_sh_str[sh]);

    int tilt = (b >> 4) & 1;
    const char *tilt_s = (tilt == 0) ? "" : "not ";
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "TILT = %d: Orientation measurement is %svalid\n", tilt, tilt_s);

    int ori = (((b >> 3) & 1) << 1) | ((b >> 2) & 1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "ORI[1:0] = %d: %s\n", ori, mxc6225xu_ori_str[ori]);

    int or_val = (((b >> 1) & 1) << 1) | ((b >> 0) & 1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "OR[1:0] = %d: %s\n", or_val, mxc6225xu_ori_str[or_val]);

    c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT, buf);
}

static void mxc6225xu_handle_reg_0x03(struct srd_decoder_inst *di, mxc6225xu_priv *s, uint8_t b)
{
    char buf[512];
    int pos = 0;

    int pd = (b >> 7) & 1;
    const char *pd_s = (pd == 0) ? "Do not power down" : "Power down";
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "PD = %d: %s the device (into a low-power state)\n", pd, pd_s);

    int shm = (b >> 6) & 1;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "SHM = %d: Set shake mode to %d\n", shm, shm);

    int shth = (((b >> 5) & 1) << 1) | ((b >> 4) & 1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "SHTH[1:0] = %d: Set shake threshold to %s\n", shth, mxc6225xu_shth_str[shth]);

    int shc = (((b >> 3) & 1) << 1) | ((b >> 2) & 1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "SHC[1:0] = %d: Set shake count to %s readings\n", shc, mxc6225xu_shc_str[shc]);

    int orc = (((b >> 1) & 1) << 1) | ((b >> 0) & 1);
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "ORC[1:0] = %d: Set orientation count to %s readings\n", orc, mxc6225xu_orc_str[orc]);

    c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT, buf);
}

static void mxc6225xu_handle_reg(struct srd_decoder_inst *di, mxc6225xu_priv *s, uint8_t b)
{
    switch (s->reg) {
    case 0x00: mxc6225xu_handle_reg_0x00(di, s, b); break;
    case 0x01: mxc6225xu_handle_reg_0x01(di, s, b); break;
    case 0x02: mxc6225xu_handle_reg_0x02(di, s, b); break;
    case 0x03: mxc6225xu_handle_reg_0x03(di, s, b); break;
    default: break;
    }
}

static void mxc6225xu_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    mxc6225xu_priv *s = (mxc6225xu_priv *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (s->state == MXC6225XU_IDLE) {
        if (strcmp(cmd, "START") == 0)
            s->state = MXC6225XU_GET_SLAVE_ADDR;
    } else if (s->state == MXC6225XU_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            if (databyte != MXC6225XU_I2C_ADDRESS) {
                char buf[64];
                snprintf(buf, sizeof(buf),
                    "Warning: I²C slave 0x%02X not an MXC6225XU compatible sensor.", databyte);
                c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT, buf);
                s->state = MXC6225XU_IDLE;
                return;
            }
            s->state = MXC6225XU_GET_REG_ADDR;
        }
    } else if (s->state == MXC6225XU_GET_REG_ADDR) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg = databyte;
            s->state = MXC6225XU_WRITE_REGS;
        }
    } else if (s->state == MXC6225XU_WRITE_REGS) {
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = MXC6225XU_READ_REGS;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            mxc6225xu_handle_reg(di, s, databyte);
            s->reg++;
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = MXC6225XU_IDLE;
        }
    } else if (s->state == MXC6225XU_READ_REGS) {
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            if (databyte != MXC6225XU_I2C_ADDRESS) {
                char buf[64];
                snprintf(buf, sizeof(buf),
                    "Warning: I²C slave 0x%02X not an MXC6225XU compatible sensor.", databyte);
                c_put(di, s->ss, s->es, s->out_ann, ANN_TEXT, buf);
                s->state = MXC6225XU_IDLE;
                return;
            }
            s->state = MXC6225XU_READ_REGS2;
        }
    } else if (s->state == MXC6225XU_READ_REGS2) {
        if (strcmp(cmd, "DATA READ") == 0) {
            mxc6225xu_handle_reg(di, s, databyte);
            s->reg++;
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = MXC6225XU_IDLE;
        }
    }
}

static void mxc6225xu_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(mxc6225xu_priv)));
    }
    mxc6225xu_priv *s = (mxc6225xu_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mxc6225xu_priv));
    s->state = MXC6225XU_IDLE;
}

static void mxc6225xu_start(struct srd_decoder_inst *di)
{
    mxc6225xu_priv *s = (mxc6225xu_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mxc6225xu");
}

static void mxc6225xu_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void mxc6225xu_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder mxc6225xu_c_decoder = {
    .id = "mxc6225xu_c",
    .name = "MXC6225XU(C)",
    .longname = "MEMSIC MXC6225XU (C)",
    .desc = "Digital Thermal Orientation Sensor (DTOS) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mxc6225xu_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = mxc6225xu_ann_rows,
    .inputs = mxc6225xu_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = mxc6225xu_tags,
    .num_tags = 2,
    .reset = mxc6225xu_reset,
    .start = mxc6225xu_start,
    .decode = mxc6225xu_decode,
    .destroy = mxc6225xu_destroy,
    .decode_upper = mxc6225xu_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &mxc6225xu_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}