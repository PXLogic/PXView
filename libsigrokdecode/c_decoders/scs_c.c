/*
 * Copyright (C) 2020 Michael Stapelberg
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_SCS = 0,
    NUM_ANN,
};

typedef struct {
    int telegram_idx;
    uint8_t crc;
    int out_ann;
} scs_state;

static const char *scs_inputs[] = {"uart", NULL};
static const char *scs_tags[] = {"Embedded/industrial", "Networking", NULL};

static const char *scs_ann_labels[][3] = {
    {"", "scs", "SCS"},
};

static const int scs_row_scs_classes[] = {ANN_SCS};
static const struct srd_c_ann_row scs_ann_rows[] = {
    {"scs", "SCS", scs_row_scs_classes, 1},
};

static void scs_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    scs_state *s = (scs_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 1)
        return;

    uint8_t val = fields[0].u8;

    if (s->telegram_idx == 0 && val == 0xa8) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, "init");
    } else if (s->telegram_idx == 1) {
        s->crc = val;
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, "addr");
    } else if (s->telegram_idx == 2) {
        s->crc ^= val;
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, "??");
    } else if (s->telegram_idx == 3) {
        s->crc ^= val;
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, "request");
    } else if (s->telegram_idx == 4) {
        s->crc ^= val;
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, "??");
    } else if (s->telegram_idx == 5) {
        const char *crc_str = (s->crc == val) ? "good crc" : "bad crc";
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, crc_str);
    } else if (s->telegram_idx == 6) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCS, "term");
        s->telegram_idx = -1;
    }

    s->telegram_idx++;
}

static void scs_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(scs_state)));
    }
    scs_state *s = (scs_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(scs_state));
}

static void scs_start(struct srd_decoder_inst *di)
{
    scs_state *s = (scs_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "scs");
}

static void scs_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void scs_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder scs_c_decoder = {
    .id = "scs_c",
    .name = "SCS(C)",
    .longname = "Sistema Cablaggio Semplificato (C)",
    .desc = "Fieldbus network protocol for home automation (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = scs_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = scs_ann_rows,
    .inputs = scs_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = scs_tags,
    .num_tags = 2,
    .reset = scs_reset,
    .start = scs_start,
    .decode = scs_decode,
    .destroy = scs_destroy,
    .decode_upper = scs_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &scs_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}