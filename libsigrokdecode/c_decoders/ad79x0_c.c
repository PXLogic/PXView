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
    ANN_MODE = 0,
    ANN_VOLTAGE,
    ANN_VALIDATION,
    NUM_ANN,
};

typedef struct {
    int samples_bit;
    uint64_t ss;
    uint64_t start_sample;
    int previous_state;
    uint32_t data;
    int out_ann;
    double vref;
} ad79x0_state;

static const char *ad79x0_inputs[] = {"spi", NULL};
static const char *ad79x0_tags[] = {"IC", "Analog/digital", NULL};

static struct srd_decoder_option ad79x0_options[] = {
    {"vref", "dec_ad79x0_opt_vref", "Reference voltage (V)", NULL, NULL},
};

static const char *ad79x0_ann_labels[][3] = {
    {"", "mode", "Mode"},
    {"", "voltage", "Voltage"},
    {"", "validation", "Validation"},
};

static const int ad79x0_row_modes_classes[] = {ANN_MODE};
static const int ad79x0_row_voltages_classes[] = {ANN_VOLTAGE};
static const int ad79x0_row_validation_classes[] = {ANN_VALIDATION};
static const struct srd_c_ann_row ad79x0_ann_rows[] = {
    {"modes", "Modes", ad79x0_row_modes_classes, 1},
    {"voltages", "Voltages", ad79x0_row_voltages_classes, 1},
    {"data_validation", "Data validation", ad79x0_row_validation_classes, 1},
};

static void ad79x0_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ad79x0_state *s = (ad79x0_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        uint8_t cs_old = (n_fields > 0) ? fields[0].u8 : 0xFF;
        uint8_t cs_new = (n_fields > 1) ? fields[1].u8 : 0;

        if (cs_old == 0 && cs_new == 1) {
            if (s->samples_bit == -1) return;
            s->data >>= 1;
            int nb_bits = (int)((start_sample - s->ss) / s->samples_bit);

            if (nb_bits >= 10) {
                if (s->data == 0xFFF) {
                    c_put(di, s->start_sample, end_sample, s->out_ann,
                              ANN_MODE, "Power Up Mode");
                    c_put(di, s->start_sample, end_sample, s->out_ann,
                              ANN_VALIDATION, "Invalid data");
                    s->previous_state = 0;
                } else {
                    c_put(di, s->start_sample, end_sample, s->out_ann,
                              ANN_MODE, "Normal Mode");
                    if (nb_bits == 16)
                        c_put(di, s->start_sample, end_sample, s->out_ann,
                                  ANN_VALIDATION, "Complete conversion");
                    else
                        c_put(di, s->start_sample, end_sample, s->out_ann,
                                  ANN_VALIDATION, "Incomplete conversion");
                    double vin = ((double)s->data / 4095.0) * s->vref;
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%.6fV", vin);
                    c_put(di, s->start_sample, end_sample, s->out_ann,
                              ANN_VOLTAGE, buf);
                    snprintf(buf, sizeof(buf), "%.2fV", vin);
                    c_put(di, s->start_sample, end_sample, s->out_ann,
                              ANN_VOLTAGE, buf);
                }
            } else {
                c_put(di, s->start_sample, end_sample, s->out_ann,
                          ANN_MODE, "Power Down Mode");
                c_put(di, s->start_sample, end_sample, s->out_ann,
                          ANN_VALIDATION, "Invalid data");
                s->previous_state = 1;
            }

            s->ss = (uint64_t)-1;
            s->samples_bit = -1;
            s->data = 0;
        } else if (cs_old == 1 && cs_new == 0) {
            s->start_sample = start_sample;
            s->samples_bit = -1;
        }
    } else if (strcmp(cmd, "BITS") == 0) {
        if (n_fields < 2) return;
        int pos = 0;
        uint8_t flags = fields[pos++].u8;
        int have_mosi = (flags & 1) ? 1 : 0;
        int have_miso = (flags & 2) ? 1 : 0;
        
        

        /* Skip MOSI bits */
        if (have_mosi) {
            int mosi_count = (int)fields[pos++].u8;
            pos += mosi_count * 17;
        }

        /* Parse MISO bits */
        if (have_miso) {
            if (pos < (int)n_fields && fields[pos].u8 == 0x00) pos++;
            int miso_count = (pos < (int)n_fields) ? (int)fields[pos++].u8 : 0;

            for (int i = 0; i < miso_count && pos + 17 <= (int)n_fields; i++) {
                uint8_t bit_val = fields[pos++].u8;
                uint64_t bit_ss = 0, bit_es = 0;
                for (int b = 0; b < 8; b++)
                    bit_ss |= ((uint64_t)fields[pos++].u8) << (8 * b);
                for (int b = 0; b < 8; b++)
                    bit_es |= ((uint64_t)fields[pos++].u8) << (8 * b);

                if (s->samples_bit == -1 && i == 0) {
                    s->samples_bit = (int)(bit_es - bit_ss);
                }
                if (s->ss == (uint64_t)-1) {
                    s->ss = bit_ss;
                }
                s->data = s->data | bit_val;
                s->data <<= 1;
            }
        }
    }
}

static void ad79x0_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ad79x0_state)));
    }
    ad79x0_state *s = (ad79x0_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ad79x0_state));
    s->samples_bit = -1;
    s->ss = (uint64_t)-1;
    s->vref = 1.5;
}

static void ad79x0_start(struct srd_decoder_inst *di)
{
    ad79x0_state *s = (ad79x0_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ad79x0");
    s->vref = c_opt_dbl(di, "vref", 1.5);
}

static void ad79x0_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ad79x0_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ad79x0_c_decoder = {
    .id = "ad79x0_c",
    .name = "AD79x0(C)",
    .longname = "Analog Devices AD79x0 (C)",
    .desc = "Analog Devices AD7910/AD7920 12-bit ADC. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ad79x0_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ad79x0_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ad79x0_ann_rows,
    .inputs = ad79x0_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ad79x0_tags,
    .num_tags = 2,
    .reset = ad79x0_reset,
    .start = ad79x0_start,
    .decode = ad79x0_decode,
    .destroy = ad79x0_destroy,
    .decode_upper = ad79x0_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ad79x0_options[0].def = g_variant_new_double(1.5);
    return &ad79x0_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}