/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2019 Mickael Bosch <mickael.bosch@linux.com>
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
    ANN_REGISTER = 0,
    ANN_VALUE,
    ANN_WARNING,
    NUM_ANN,
};

enum pca9571_state {
    PCA9571_IDLE,
    PCA9571_GET_SLAVE_ADDR,
    PCA9571_READ_DATA,
    PCA9571_WRITE_DATA,
};

#define PCA9571_I2C_ADDRESS 0x25

typedef struct {
    enum pca9571_state state;
    uint8_t last_write;
    uint64_t last_write_es;
    uint64_t ss;
    uint64_t es;
    int out_ann;
    int out_logic;
} pca9571_priv;

static const char *pca9571_inputs[] = {"i2c", NULL};
static const char *pca9571_tags[] = {"Embedded/industrial", "IC", NULL};

static const char *pca9571_ann_labels[][3] = {
    {"", "register", "Register type"},
    {"", "value", "Register value"},
    {"", "warning", "Warning"},
};

static const int pca9571_row_regs_classes[] = {ANN_REGISTER, ANN_VALUE};
static const int pca9571_row_warnings_classes[] = {ANN_WARNING};
static const struct srd_c_ann_row pca9571_ann_rows[] = {
    {"regs", "Registers", pca9571_row_regs_classes, 2},
    {"warnings", "Warnings", pca9571_row_warnings_classes, 1},
};

static void pca9571_handle_io(struct srd_decoder_inst *di, pca9571_priv *s, uint8_t b)
{
    char buf[64];
    if (s->state == PCA9571_READ_DATA) {
        snprintf(buf, sizeof(buf), "Outputs read: %02X", b);
        c_put(di, s->ss, s->es, s->out_ann, ANN_VALUE, buf);
        if (b != s->last_write) {
            snprintf(buf, sizeof(buf),
                "Warning: read value and last write value (%02X) are different",
                s->last_write);
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING, buf);
        }
    } else {
        snprintf(buf, sizeof(buf), "Outputs set: %02X", b);
        c_put(di, s->ss, s->es, s->out_ann, ANN_VALUE, buf);
        s->last_write = b;
        s->last_write_es = s->es;
        /* Output logic signal */
        uint8_t logic_val = b;
        c_put_logic(di, s->last_write_es, s->es, s->out_logic, 0xFF, &logic_val, 8);
    }
}

static void pca9571_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    pca9571_priv *s = (pca9571_priv *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "ACK") == 0 || strcmp(cmd, "BITS") == 0) {
        /* Discard ACK and BITS */
    } else if (strcmp(cmd, "START") == 0 || strcmp(cmd, "START REPEAT") == 0) {
        s->state = PCA9571_GET_SLAVE_ADDR;
    } else if (strcmp(cmd, "NACK") == 0 || strcmp(cmd, "STOP") == 0) {
        s->state = PCA9571_IDLE;
    } else if (strcmp(cmd, "ADDRESS READ") == 0 || strcmp(cmd, "ADDRESS WRITE") == 0) {
        if (s->state == PCA9571_GET_SLAVE_ADDR) {
            if (databyte != PCA9571_I2C_ADDRESS) {
                char buf[64];
                snprintf(buf, sizeof(buf),
                    "Warning: I²C slave 0x%02X not a PCA9571 compatible chip.",
                    databyte);
                c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING, buf);
                s->state = PCA9571_IDLE;
                return;
            }
            if (strcmp(cmd, "ADDRESS READ") == 0)
                s->state = PCA9571_READ_DATA;
            else
                s->state = PCA9571_WRITE_DATA;
        } else {
            s->state = PCA9571_IDLE;
        }
    } else if (strcmp(cmd, "DATA READ") == 0 || strcmp(cmd, "DATA WRITE") == 0) {
        if (s->state == PCA9571_READ_DATA || s->state == PCA9571_WRITE_DATA) {
            pca9571_handle_io(di, s, databyte);
        } else {
            s->state = PCA9571_IDLE;
        }
    }
}

static void pca9571_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(pca9571_priv)));
    }
    pca9571_priv *s = (pca9571_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(pca9571_priv));
    s->state = PCA9571_IDLE;
    s->last_write = 0xFF; /* Chip port default state is high */
}

static void pca9571_start(struct srd_decoder_inst *di)
{
    pca9571_priv *s = (pca9571_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pca9571");
    s->out_logic = c_reg_out(di, SRD_OUTPUT_LOGIC, "pca9571");
}

static void pca9571_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void pca9571_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder pca9571_c_decoder = {
    .id = "pca9571_c",
    .name = "PCA9571(C)",
    .longname = "NXP PCA9571 (C)",
    .desc = "NXP PCA9571 8-bit I²C output expander. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = pca9571_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = pca9571_ann_rows,
    .inputs = pca9571_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = pca9571_tags,
    .num_tags = 2,
    .reset = pca9571_reset,
    .start = pca9571_start,
    .decode = pca9571_decode,
    .destroy = pca9571_destroy,
    .decode_upper = pca9571_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &pca9571_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}