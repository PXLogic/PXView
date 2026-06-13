/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2024 DreamSourceLab <info@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum osw_ann {
    ANN_BIT = 0,
    ANN_BYTE,
    ANN_SAMPLE,
    ANN_WAIT,
    ANN_PB,
    NUM_ANN,
};

typedef struct {
    uint64_t bt_block_ss;
    uint64_t by_block_ss;
    int bit_index;
    uint8_t decoded_byte;
    int parity_bit;
    uint64_t threshold_samples;
    int64_t threshold_us;
    uint64_t samplerate;
    int out_ann;
} osw_priv;

static struct srd_channel osw_channels[] = {
    { "osw",  "OSW",  "OSW signal line",           0, SRD_CHANNEL_SDATA, NULL },
    { "strt", "Start Pulse", "OSW device start pulse signal", 1, SRD_CHANNEL_COMMON, NULL },
};

static struct srd_decoder_option osw_options[] = {
    { "threshold", NULL, "Threshold time value (us)", NULL, NULL },
};

static const char *osw_ann_labels[][3] = {
    { "", "bit",    "Bit" },
    { "", "byte",   "Byte" },
    { "", "sample", "Sample" },
    { "", "wait",   "Wait" },
    { "", "pb",     "PB (Parity Bit)" },
};

static const int osw_row_bits_classes[] = { ANN_BIT, ANN_WAIT, -1 };
static const int osw_row_bytes_classes[] = { ANN_BYTE, ANN_PB, -1 };
static const int osw_row_samples_classes[] = { ANN_SAMPLE, -1 };

static const struct srd_c_ann_row osw_ann_rows[] = {
    { "bits",    "Bits",    osw_row_bits_classes,    2 },
    { "bytes",   "Bytes",   osw_row_bytes_classes,   2 },
    { "samples", "Samples", osw_row_samples_classes, 1 },
};

static const char *osw_inputs[] = { "logic" };
static const char *osw_tags[] = { "Custom" };

static void osw_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(osw_priv)));
    osw_priv *s = (osw_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(osw_priv));
    s->out_ann = -1;
    s->threshold_us = 8;
}

static void osw_start(struct srd_decoder_inst *di)
{
    osw_priv *s = (osw_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "OneSingleWire");
    s->threshold_us = c_opt_int(di, "threshold", 8);
}

static void osw_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    osw_priv *s = (osw_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (value > 0)
            s->threshold_samples = (uint64_t)s->threshold_us * value / 1000000ULL;
    }
}

static void osw_decode(struct srd_decoder_inst *di)
{
    osw_priv *s = (osw_priv *)c_decoder_get_private(di);
    if (s->samplerate == 0)
        return;

    /* Phase 1: Wait for start pulse (strt rising edge) */
    int ret = c_wait(di, CW_R(1), CW_END);
    if (ret != SRD_OK)
        return;

    /* Phase 2: Wait for signal falling edge (osw) */
    ret = c_wait(di, CW_F(0), CW_END);
    if (ret != SRD_OK)
        return;

    s->bt_block_ss = di_samplenum(di);
    s->by_block_ss = di_samplenum(di);
    s->bit_index = 0;
    s->decoded_byte = 0;
    s->parity_bit = 0;

    /* Phase 3: Main decode loop */
    while (1) {
        ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t period_range = di_samplenum(di) - s->bt_block_ss;

        if (s->bit_index < 9) {
            int osw = (period_range < s->threshold_samples) ? 1 : 0;
            s->decoded_byte |= (osw << s->bit_index);
            s->parity_bit ^= osw;

            if (s->bit_index == 7) {
                char byte_str[32];
                snprintf(byte_str, sizeof(byte_str), "Byte: %d", s->decoded_byte);
                char val_str[8];
                snprintf(val_str, sizeof(val_str), "%d", s->decoded_byte);
                c_put(di, s->by_block_ss, di_samplenum(di), s->out_ann, ANN_BYTE, byte_str, val_str);
            } else if (s->bit_index == 8) {
                const char *pstr = (s->parity_bit == 0) ? "OK" : "ERR";
                char pb_str[32];
                if (s->parity_bit == 0)
                    snprintf(pb_str, sizeof(pb_str), "Parity check: %s", pstr);
                else
                    snprintf(pb_str, sizeof(pb_str), "%s", pstr);
                c_put(di, s->bt_block_ss, di_samplenum(di), s->out_ann, ANN_PB, pb_str, pstr);
            }

            char bit_str[16], bit_short[4];
            snprintf(bit_str, sizeof(bit_str), "Bit: %d", osw);
            snprintf(bit_short, sizeof(bit_short), "%d", osw);
            c_put(di, s->bt_block_ss, di_samplenum(di), s->out_ann, ANN_BIT, bit_str, bit_short);

            char samp_str[32], samp_short[16];
            snprintf(samp_str, sizeof(samp_str), "Samples: %d", (int)period_range);
            snprintf(samp_short, sizeof(samp_short), "%d", (int)period_range);
            c_put(di, s->bt_block_ss, di_samplenum(di), s->out_ann, ANN_SAMPLE, samp_str, samp_short);

            s->bit_index++;
        } else {
            c_put(di, s->bt_block_ss, di_samplenum(di), s->out_ann, ANN_WAIT, "Wait", "w");
            char samp_str[32], samp_short[16];
            snprintf(samp_str, sizeof(samp_str), "Samples: %d", (int)period_range);
            snprintf(samp_short, sizeof(samp_short), "%d", (int)period_range);
            c_put(di, s->bt_block_ss, di_samplenum(di), s->out_ann, ANN_SAMPLE, samp_str, samp_short);
            s->by_block_ss = di_samplenum(di);
            s->decoded_byte = 0;
            s->parity_bit = 0;
            s->bit_index = 0;
        }
        s->bt_block_ss = di_samplenum(di);
    }
}

static void osw_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder osw_c_decoder = {
    .id = "one_single_wire_c",
    .name = "OneSingleWire(C)",
    .longname = "OneSingleWire custom bus (C)",
    .desc = "Bidirectional, half-duplex, asynchronous serial bus. (C implementation)",
    .license = "gplv2+",
    .channels = osw_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = osw_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = osw_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = osw_ann_rows,
    .inputs = osw_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .tags = osw_tags,
    .num_tags = 1,
    .binary = NULL,
    .num_binary = 0,
    .reset = osw_reset,
    .start = osw_start,
    .decode = osw_decode,
    .destroy = osw_destroy,
    .state_size = 0,
    .metadata = osw_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    osw_options[0].def = g_variant_new_int64(8);
    return &osw_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}