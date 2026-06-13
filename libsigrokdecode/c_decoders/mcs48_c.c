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

enum mcs48_ann {
    ANN_ROMDATA = 0,
    NUM_ANN,
};

typedef struct {
    uint16_t addr;
    uint64_t addr_s;
    uint8_t data;
    uint64_t data_s;
    int started;
    int has_bank;
    int out_ann;
    int out_bin;
} mcs48_priv;

static struct srd_channel mcs48_channels[] = {
    { "ale",  "ALE",  "Address latch enable",  0,  SRD_CHANNEL_COMMON, NULL },
    { "psen", "/PSEN", "Program store enable", 1,  SRD_CHANNEL_COMMON, NULL },
    { "d0",   "D0",   "CPU data line 0",       2,  SRD_CHANNEL_SDATA,  NULL },
    { "d1",   "D1",   "CPU data line 1",       3,  SRD_CHANNEL_SDATA,  NULL },
    { "d2",   "D2",   "CPU data line 2",       4,  SRD_CHANNEL_SDATA,  NULL },
    { "d3",   "D3",   "CPU data line 3",       5,  SRD_CHANNEL_SDATA,  NULL },
    { "d4",   "D4",   "CPU data line 4",       6,  SRD_CHANNEL_SDATA,  NULL },
    { "d5",   "D5",   "CPU data line 5",       7,  SRD_CHANNEL_SDATA,  NULL },
    { "d6",   "D6",   "CPU data line 6",       8,  SRD_CHANNEL_SDATA,  NULL },
    { "d7",   "D7",   "CPU data line 7",       9,  SRD_CHANNEL_SDATA,  NULL },
    { "a8",   "A8",   "CPU address line 8",    10, SRD_CHANNEL_SDATA,  NULL },
    { "a9",   "A9",   "CPU address line 9",    11, SRD_CHANNEL_SDATA,  NULL },
    { "a10",  "A10",  "CPU address line 10",   12, SRD_CHANNEL_SDATA,  NULL },
    { "a11",  "A11",  "CPU address line 11",   13, SRD_CHANNEL_SDATA,  NULL },
};

static struct srd_channel mcs48_optional_channels[] = {
    { "a12",  "A12",  "CPU address line 12",   14, SRD_CHANNEL_SDATA,  NULL },
};

static const char *mcs48_ann_labels[][3] = {
    { "", "romdata", "Address:Data" },
};

static const int mcs48_row_data_classes[] = { ANN_ROMDATA, -1 };

static const struct srd_c_ann_row mcs48_ann_rows[] = {
    { "romdata", "Address:Data", mcs48_row_data_classes, 1 },
};

static const struct srd_decoder_binary mcs48_binary[] = {
    { 0, "romdata", "AAAA:DD" },
};

static const char *mcs48_inputs[] = { "logic" };
static const char *mcs48_tags[] = { "Retro computing" };

static void mcs48_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(mcs48_priv)));
    mcs48_priv *s = (mcs48_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mcs48_priv));
    s->out_ann = -1;
    s->out_bin = -1;
}

static void mcs48_start(struct srd_decoder_inst *di)
{
    mcs48_priv *s = (mcs48_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mcs48");
    s->out_bin = c_reg_out(di, SRD_OUTPUT_BINARY, "mcs48");
    s->has_bank = c_has_ch(di, 14);
}

static void mcs48_decode(struct srd_decoder_inst *di)
{
    mcs48_priv *s = (mcs48_priv *)c_decoder_get_private(di);
    while (1) {
        int ret = c_wait(di, CW_F(0), CW_OR, CW_R(1), CW_END);
        if (ret != SRD_OK)
            return;

        /* Read all channel values at di_samplenum(di) */
        int d[8], a[4], bank = 0;
        for (int i = 0; i < 8; i++)
            d[i] = c_pin(di, 2 + i);
        for (int i = 0; i < 4; i++)
            a[i] = c_pin(di, 10 + i);
        if (s->has_bank)
            bank = c_pin(di, 14);

        if (di_matched(di) & (1ULL << 0)) { /* ALE falling edge */
            s->started = 1;
            uint16_t addr = 0;
            for (int i = 0; i < 4; i++)
                addr |= (a[i] << (i + 8));
            for (int i = 0; i < 8; i++)
                addr |= (d[i] << i);
            if (s->has_bank)
                addr |= (bank << 12);
            s->addr = addr;
            s->addr_s = di_samplenum(di);
        }
        if (di_matched(di) & (1ULL << 1)) { /* /PSEN rising edge */
            uint8_t data = 0;
            for (int i = 0; i < 8; i++)
                data |= (d[i] << i);
            s->data = data;
            s->data_s = di_samplenum(di);
            if (s->started) {
                char text[16];
                snprintf(text, sizeof(text), "%04X:%02X", s->addr, s->data);
                c_put(di, s->addr_s, s->data_s, s->out_ann, ANN_ROMDATA, text);
                /* Binary output: 2 bytes addr (big-endian) + 1 byte data */
                uint8_t bindata[3];
                bindata[0] = (s->addr >> 8) & 0xFF;
                bindata[1] = s->addr & 0xFF;
                bindata[2] = s->data;
                c_put_bin(di, s->addr_s, s->data_s, s->out_bin, 0, 3, bindata);
            }
        }
    }
}

static void mcs48_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder mcs48_c_decoder = {
    .id = "mcs48_c",
    .name = "MCS-48(C)",
    .longname = "Intel MCS-48 (C)",
    .desc = "Intel MCS-48 external memory access protocol. (C implementation)",
    .license = "gplv2+",
    .channels = mcs48_channels,
    .num_channels = 14,
    .optional_channels = mcs48_optional_channels,
    .num_optional_channels = 1,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mcs48_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = mcs48_ann_rows,
    .inputs = mcs48_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .tags = mcs48_tags,
    .num_tags = 1,
    .binary = mcs48_binary,
    .num_binary = 1,
    .reset = mcs48_reset,
    .start = mcs48_start,
    .decode = mcs48_decode,
    .destroy = mcs48_destroy,
    .state_size = 0,
    .metadata = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &mcs48_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}