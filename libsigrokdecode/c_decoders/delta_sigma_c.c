/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2023 Rikka0w0 <929513338@qq.com>
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
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum delta_sigma_ann {
    ANN_BIT_STREAM = 0,
    ANN_FILTERED,
    ANN_CONVERTED,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    uint64_t last_samplenum;
    int current_dat;
    uint64_t last_filternum;
    int first_sample;

    int64_t sinc_DELTA1;
    int64_t sinc_CN1;
    int64_t sinc_CN2;
    int64_t sinc_DN0;
    int64_t sinc_DN1;
    int64_t sinc_DN3;
    int64_t sinc_DN5;
    int64_t sinc_CNTR;

    int clock_mode;     /* 0=normal, 1=manchester */
    int filter_type;    /* 0=sinc_fast, 1=sinc1, 2=sinc2, 3=sinc3 */
    int osr;
    int shift;
    double scale;
    int out_ann;
} delta_sigma_state;

static struct srd_channel delta_sigma_channels[] = {
    { "dat", "DAT", "Data", 0, SRD_CHANNEL_SDATA, NULL },
    { "clk", "CLK", "Clock", 1, SRD_CHANNEL_SCLK, NULL },
};

static struct srd_decoder_option delta_sigma_options[] = {
    { "clock_mode", NULL, "Clock Mode", NULL, NULL },
    { "filter_type", NULL, "Filter type", NULL, NULL },
    { "osr", NULL, "Oversampling Factor", NULL, NULL },
    { "shift", NULL, "Right shift the result by", NULL, NULL },
    { "scale", NULL, "Code-Actual scaler", NULL, NULL },
};

static const char* delta_sigma_ann_labels[][3] = {
    { "", "Bit Stream", "Bit Stream" },
    { "", "Filtered", "Filtered" },
    { "", "Converted", "Converted" },
};

static const int delta_sigma_row_bit_streams_classes[] = { ANN_BIT_STREAM, -1 };
static const int delta_sigma_row_filtereds_classes[] = { ANN_FILTERED, -1 };
static const int delta_sigma_row_converteds_classes[] = { ANN_CONVERTED, -1 };

static const struct srd_c_ann_row delta_sigma_ann_rows[] = {
    { "bit-streams", "Bit Stream", delta_sigma_row_bit_streams_classes, 1 },
    { "filtereds", "Filtered", delta_sigma_row_filtereds_classes, 1 },
    { "converteds", "Converted", delta_sigma_row_converteds_classes, 1 },
};

static const char* delta_sigma_inputs[] = { "logic" };
static const char* delta_sigma_tags[] = { "Util" };

static void delta_sigma_put_result(struct srd_decoder_inst *di, delta_sigma_state *s, int64_t code, uint64_t samplenum)
{
    (void)samplenum;
    code = code >> s->shift;
    char buf1[64], buf2[64];
    snprintf(buf1, sizeof(buf1), "%lld", (long long)code);
    snprintf(buf2, sizeof(buf2), "%lld", (long long)(code * s->scale));
    c_put(di, s->last_filternum, samplenum, s->out_ann, ANN_FILTERED, buf1);
    c_put(di, s->last_filternum, samplenum, s->out_ann, ANN_CONVERTED, buf2);
}

static void delta_sigma_run_sinc1(struct srd_decoder_inst *di, delta_sigma_state *s, int dat, uint64_t samplenum)
{
    (void)samplenum;
    int64_t sinc_DELTA1;
    if (dat > 0)
        sinc_DELTA1 = s->sinc_DELTA1 + 1;
    else
        sinc_DELTA1 = s->sinc_DELTA1 - 1;

    s->sinc_CNTR = s->sinc_CNTR + 1;
    if (s->sinc_CNTR == s->osr) {
        s->sinc_CNTR = 0;
        int64_t sinc_DN0 = s->sinc_DELTA1;
        int64_t sinc_DN1 = s->sinc_DN0;
        int64_t sinc_CN3 = s->sinc_DN0 - s->sinc_DN1;

        s->sinc_DN0 = sinc_DN0;
        s->sinc_DN1 = sinc_DN1;

        delta_sigma_put_result(di, s, sinc_CN3, samplenum);
        s->last_filternum = samplenum;
    }
    s->sinc_DELTA1 = sinc_DELTA1;
}

static void delta_sigma_run_sinc2(struct srd_decoder_inst *di, delta_sigma_state *s, int dat, uint64_t samplenum)
{
    (void)samplenum;
    int64_t sinc_DELTA1;
    if (dat > 0)
        sinc_DELTA1 = s->sinc_DELTA1 + 1;
    else
        sinc_DELTA1 = s->sinc_DELTA1 - 1;

    int64_t sinc_CN1 = s->sinc_CN1 + s->sinc_DELTA1;

    s->sinc_CNTR = s->sinc_CNTR + 1;
    if (s->sinc_CNTR == s->osr) {
        s->sinc_CNTR = 0;
        int64_t sinc_DN0 = s->sinc_CN1;
        int64_t sinc_DN1 = s->sinc_DN0;
        int64_t sinc_CN3 = s->sinc_DN0 - s->sinc_DN1;
        int64_t sinc_CN4 = sinc_CN3 - s->sinc_DN3;

        s->sinc_DN0 = sinc_DN0;
        s->sinc_DN1 = sinc_DN1;
        s->sinc_DN3 = sinc_CN3;

        delta_sigma_put_result(di, s, sinc_CN4, samplenum);
        s->last_filternum = samplenum;
    }
    s->sinc_DELTA1 = sinc_DELTA1;
    s->sinc_CN1 = sinc_CN1;
}

static void delta_sigma_run_sinc3(struct srd_decoder_inst *di, delta_sigma_state *s, int dat, uint64_t samplenum)
{
    (void)samplenum;
    int64_t sinc_DELTA1;
    if (dat > 0)
        sinc_DELTA1 = s->sinc_DELTA1 + 1;
    else
        sinc_DELTA1 = s->sinc_DELTA1 - 1;

    int64_t sinc_CN1 = s->sinc_CN1 + s->sinc_DELTA1;
    int64_t sinc_CN2 = s->sinc_CN1 + s->sinc_CN2;

    s->sinc_CNTR = s->sinc_CNTR + 1;
    if (s->sinc_CNTR == s->osr) {
        s->sinc_CNTR = 0;
        int64_t sinc_DN0 = s->sinc_CN2;
        int64_t sinc_DN1 = s->sinc_DN0;
        int64_t sinc_CN3 = s->sinc_DN0 - s->sinc_DN1;
        int64_t sinc_CN4 = sinc_CN3 - s->sinc_DN3;
        int64_t sinc_CN5 = sinc_CN4 - s->sinc_DN5;

        s->sinc_DN0 = sinc_DN0;
        s->sinc_DN1 = sinc_DN1;
        s->sinc_DN3 = sinc_CN3;
        s->sinc_DN5 = sinc_CN4;

        delta_sigma_put_result(di, s, sinc_CN5, samplenum);
        s->last_filternum = samplenum;
    }
    s->sinc_DELTA1 = sinc_DELTA1;
    s->sinc_CN1 = sinc_CN1;
    s->sinc_CN2 = sinc_CN2;
}

static void delta_sigma_find_clk_edge(struct srd_decoder_inst *di, delta_sigma_state *s, int dat, uint64_t samplenum)
{
    (void)samplenum;
    if (!s->first_sample) {
        s->first_sample = 1;
        s->last_samplenum = 0;
        c_put(di, 0, samplenum, s->out_ann, ANN_BIT_STREAM, "X");
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s->current_dat);
        c_put(di, s->last_samplenum, samplenum, s->out_ann, ANN_BIT_STREAM, buf);
    }

    if (s->filter_type == 1)
        delta_sigma_run_sinc1(di, s, dat, samplenum);
    else if (s->filter_type == 2)
        delta_sigma_run_sinc2(di, s, dat, samplenum);
    else /* sinc3 or sinc_fast (fallback to sinc3) */
        delta_sigma_run_sinc3(di, s, dat, samplenum);

    s->last_samplenum = samplenum;
    s->current_dat = dat;
}

static void delta_sigma_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(delta_sigma_state)));
    }
    delta_sigma_state *s = (delta_sigma_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(delta_sigma_state));
    s->out_ann = -1;
    s->current_dat = -1;
    s->first_sample = 0;
}

static void delta_sigma_start(struct srd_decoder_inst *di)
{
    delta_sigma_state *s = (delta_sigma_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "delta-sigma");

    const char *clock_mode_str = c_opt_str(di, "clock_mode", "normal");
    s->clock_mode = (strcmp(clock_mode_str, "manchester") == 0) ? 1 : 0;

    const char *filter_type_str = c_opt_str(di, "filter_type", "sinc3");
    if (strcmp(filter_type_str, "sinc_fast") == 0)
        s->filter_type = 0;
    else if (strcmp(filter_type_str, "sinc1") == 0)
        s->filter_type = 1;
    else if (strcmp(filter_type_str, "sinc2") == 0)
        s->filter_type = 2;
    else
        s->filter_type = 3;

    s->osr = (int)c_opt_int(di, "osr", 4);
    s->shift = (int)c_opt_int(di, "shift", 0);
    s->scale = c_opt_dbl(di, "scale", 1.0);
}

static void delta_sigma_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    delta_sigma_state *s = (delta_sigma_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void delta_sigma_decode(struct srd_decoder_inst *di)
{
    delta_sigma_state *s = (delta_sigma_state *)c_decoder_get_private(di);

    if (s->samplerate == 0) return;

    if (!c_has_ch(di, 0)) return;
    if (!c_has_ch(di, 1)) return;

    while (1) {
        int ret = c_wait(di, CW_R(1), CW_END);
        if (ret != SRD_OK)
            return;

        int dat = c_pin(di, 0);
        delta_sigma_find_clk_edge(di, s, dat, di_samplenum(di));
    }
}

static void delta_sigma_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder delta_sigma_c_decoder = {
    .id = "delta-sigma_c",
    .name = "Delta-Sigma(C)",
    .longname = "Delta-Sigma Decoder (C)",
    .desc = "Clocked.",
    .license = "gplv2+",
    .channels = delta_sigma_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = delta_sigma_options,
    .num_options = 5,
    .num_annotations = NUM_ANN,
    .ann_labels = delta_sigma_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = delta_sigma_ann_rows,
    .inputs = delta_sigma_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = delta_sigma_tags,
    .num_tags = 1,
    .reset = delta_sigma_reset,
    .start = delta_sigma_start,
    .decode = delta_sigma_decode,
    .destroy = delta_sigma_destroy,
    .state_size = 0,
    .metadata = delta_sigma_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &delta_sigma_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}