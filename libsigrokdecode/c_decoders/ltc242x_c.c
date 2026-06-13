/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Analog Devices Inc.
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
    ANN_CH0_VOLTAGE = 0,
    ANN_CH1_VOLTAGE,
    NUM_ANN,
};

typedef struct {
    int out_ann;
    uint32_t data;
    uint64_t ss;
    uint64_t es;
    double vref;
} ltc242x_state;

static const char *ltc242x_inputs[] = {"spi", NULL};
static const char *ltc242x_tags[] = {"IC", "Analog/digital", NULL};

static struct srd_decoder_option ltc242x_options[] = {
    {"vref", "dec_ltc242x_opt_vref", "Reference voltage (V)", NULL, NULL},
};

static const char *ltc242x_ann_labels[][3] = {
    {"", "ch0_voltage", "CH0 voltage"},
    {"", "ch1_voltage", "CH1 voltage"},
};

static const int ltc242x_row_ch0_classes[] = {ANN_CH0_VOLTAGE, -1};
static const int ltc242x_row_ch1_classes[] = {ANN_CH1_VOLTAGE, -1};

static const struct srd_c_ann_row ltc242x_ann_rows[] = {
    {"ch0_voltages", "CH0 voltages", ltc242x_row_ch0_classes, 1},
    {"ch1_voltages", "CH1 voltages", ltc242x_row_ch1_classes, 1},
};

static void ltc242x_handle_voltage(struct srd_decoder_inst *di, ltc242x_state *s)
{
    uint32_t raw = s->data & 0x3FFFFF;
    double input_voltage = -(double)(0x200000 - raw);
    input_voltage = (input_voltage / 0xFFFFF) * s->vref;

    int channel = (s->data >> 22) & 1;

    char v1[32], v2[32];
    snprintf(v1, sizeof(v1), "%.6fV", input_voltage);
    snprintf(v2, sizeof(v2), "%.2fV", input_voltage);
    c_put(di, s->ss, s->es, s->out_ann, channel, v1, v2);
}

static void ltc242x_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ltc242x_state *s = (ltc242x_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        if (!fields || n_fields < 2) return;
        int cs_old = fields[0].u8;
        int cs_new = fields[1].u8;
        if (cs_old == 0 && cs_new == 1) {
            s->es = end_sample;
            s->data >>= 1;
            ltc242x_handle_voltage(di, s);
            s->data = 0;
        } else if (cs_old == 1 && cs_new == 0) {
            s->ss = start_sample;
        }
    } else if (strcmp(cmd, "BITS") == 0) {
        if (!fields || n_fields < 2) return;

        uint8_t flags = fields[0].u8;
        int have_mosi = (flags & 1) ? 1 : 0;
        int have_miso = (flags & 2) ? 1 : 0;
        (void)have_mosi;
        
        int mosi_cnt = fields[1].u8;
        int pos = 2;

        /* Skip MOSI bits */
        pos += mosi_cnt * 17;

        /* Skip reserved byte */
        if (pos >= (int)n_fields) return;
        pos++;

        if (pos >= (int)n_fields) return;
        int miso_cnt = fields[pos++].u8;

        if (have_miso && miso_cnt > 0) {
            for (int i = 0; i < miso_cnt && pos + 17 <= (int)n_fields; i++) {
                int bit_val = fields[pos].u8;
                pos += 17;
                s->data = (s->data | bit_val) << 1;
            }
        }
    }
}

static void ltc242x_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ltc242x_state)));
    }
    ltc242x_state *s = (ltc242x_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ltc242x_state));
    s->vref = 1.5;
}

static void ltc242x_start(struct srd_decoder_inst *di)
{
    ltc242x_state *s = (ltc242x_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ltc242x");
    s->vref = c_opt_dbl(di, "vref", 1.5);
}

static void ltc242x_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ltc242x_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ltc242x_c_decoder = {
    .id = "ltc242x_c",
    .name = "LTC242x(C)",
    .longname = "Linear Technology LTC242x (C)",
    .desc = "Linear Technology LTC2421/LTC2422 1-/2-channel 20-bit ADC. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ltc242x_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ltc242x_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = ltc242x_ann_rows,
    .inputs = ltc242x_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ltc242x_tags,
    .num_tags = 2,
    .reset = ltc242x_reset,
    .start = ltc242x_start,
    .decode = ltc242x_decode,
    .destroy = ltc242x_destroy,
    .decode_upper = ltc242x_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ltc242x_options[0].def = g_variant_new_double(1.5);
    return &ltc242x_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}