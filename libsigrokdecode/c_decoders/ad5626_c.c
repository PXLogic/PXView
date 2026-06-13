/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Analog Devices Inc.
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
    ANN_VOLTAGE = 0,
    NUM_ANN,
};

typedef struct {
    uint32_t data;
    uint64_t ss;
    int out_ann;
} ad5626_state;

static const char *ad5626_inputs[] = {"spi", NULL};
static const char *ad5626_tags[] = {"IC", "Analog/digital", NULL};

static const char *ad5626_ann_labels[][3] = {
    {"", "voltage", "Voltage"},
};

static const int ad5626_row_voltage_classes[] = {ANN_VOLTAGE};
static const struct srd_c_ann_row ad5626_ann_rows[] = {
    {"voltage", "Voltage", ad5626_row_voltage_classes, 1},
};

static void ad5626_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ad5626_state *s = (ad5626_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        uint8_t cs_old = (n_fields > 0) ? fields[0].u8 : 0xFF;
        uint8_t cs_new = (n_fields > 1) ? fields[1].u8 : 0;

        if (cs_old == 0 && cs_new == 1) {
            s->data >>= 1;
            double voltage = (double)s->data / 1000.0;
            char buf[32];
            snprintf(buf, sizeof(buf), "%.3fV", voltage);
            c_put(di, s->ss, end_sample, s->out_ann, ANN_VOLTAGE, buf);
            s->data = 0;
        } else if (cs_old == 1 && cs_new == 0) {
            s->ss = start_sample;
        }
    } else if (strcmp(cmd, "BITS") == 0) {
        if (n_fields < 2) return;
        int pos = 0;
        uint8_t flags = fields[pos++].u8;
        int have_mosi = (flags & 1) ? 1 : 0;
        
        if (have_mosi) {
            int mosi_count = (int)fields[pos++].u8;
            for (int i = 0; i < mosi_count && pos + 17 <= (int)n_fields; i++) {
                uint8_t bit_val = fields[pos].u8;
                pos += 17;
                s->data = s->data | bit_val;
                s->data <<= 1;
            }
        }
    }
}

static void ad5626_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ad5626_state)));
    }
    ad5626_state *s = (ad5626_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ad5626_state));
}

static void ad5626_start(struct srd_decoder_inst *di)
{
    ad5626_state *s = (ad5626_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ad5626");
}

static void ad5626_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ad5626_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ad5626_c_decoder = {
    .id = "ad5626_c",
    .name = "AD5626(C)",
    .longname = "Analog Devices AD5626 (C)",
    .desc = "Analog Devices AD5626 12-bit nanoDAC. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ad5626_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = ad5626_ann_rows,
    .inputs = ad5626_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ad5626_tags,
    .num_tags = 2,
    .reset = ad5626_reset,
    .start = ad5626_start,
    .decode = ad5626_decode,
    .destroy = ad5626_destroy,
    .decode_upper = ad5626_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ad5626_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}