/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2019 Shirow Miura <shirowmiura@gmail.com>
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
    ANN_BIT0 = 0,
    ANN_BIT1,
    ANN_START,
    ANN_STOP,
    ANN_SIGNATURE,
    NUM_ANN,
};

#define CH_START 0
#define CH_STOP  1
#define CH_CLK   2
#define CH_DATA  3

/* Symbol map: 4-bit nibble -> character (HP 5004A style, bit-reversed keys) */
static const char symbol_map[16] = {
    '0', '8', '4', 'F', '2', 'A', '6', 'P',
    '1', '9', '5', 'H', '3', 'C', '7', 'U',
};

typedef struct {
    int gate_is_open;
    uint64_t sample_start;
    int started;
    uint64_t last_samplenum;
    int prev_start;
    int prev_stop;
    uint16_t shiftreg;

    int start_edge_rising;  /* 1=rising, 0=falling */
    int stop_edge_rising;
    int annbits;

    int out_ann;
    uint64_t samplerate;
} sig_state;

static struct srd_channel sig_channels[] = {
    {"start", "START", "START channel", 0, SRD_CHANNEL_COMMON, "dec_signature_chan_start"},
    {"stop",  "STOP",  "STOP channel",  1, SRD_CHANNEL_COMMON, "dec_signature_chan_stop"},
    {"clk",   "CLOCK", "CLOCK channel", 2, SRD_CHANNEL_SCLK,   "dec_signature_chan_clk"},
    {"data",  "DATA",  "DATA channel",  3, SRD_CHANNEL_SDATA,  "dec_signature_chan_data"},
};

static struct srd_decoder_option sig_options[] = {
    {"start_edge", "dec_signature_opt_start_edge", "START edge polarity", NULL, NULL},
    {"stop_edge",  "dec_signature_opt_stop_edge",  "STOP edge polarity",  NULL, NULL},
    {"clk_edge",   "dec_signature_opt_clk_edge",   "CLOCK edge polarity", NULL, NULL},
    {"annbits",    "dec_signature_opt_annbits",     "Enable bit level annotations", NULL, NULL},
};

static const char *sig_ann_labels[][3] = {
    {"", "bit0", "Bit0"},
    {"", "bit1", "Bit1"},
    {"", "start", "START"},
    {"", "stop", "STOP"},
    {"", "signature", "Signature"},
};

static const int sig_row_bits_classes[] = {ANN_BIT0, ANN_BIT1, ANN_START, ANN_STOP, -1};
static const int sig_row_signatures_classes[] = {ANN_SIGNATURE, -1};

static const struct srd_c_ann_row sig_ann_rows[] = {
    {"bits", "Bits", sig_row_bits_classes, 4},
    {"signatures", "Signatures", sig_row_signatures_classes, 1},
};

static const char *sig_inputs[] = {"logic"};
static const char *sig_tags[] = {"Debug/trace", "Util", "Encoding"};

static int popcount16(uint16_t x)
{
    int count = 0;
    while (x) {
        count += x & 1;
        x >>= 1;
    }
    return count;
}

static void sig_putsig(struct srd_decoder_inst *di, sig_state *s,
                       uint64_t ss, uint64_t es, uint16_t signature)
{
    char buf[8];
    buf[0] = symbol_map[(signature >>  0) & 0x0f];
    buf[1] = symbol_map[(signature >>  4) & 0x0f];
    buf[2] = symbol_map[(signature >>  8) & 0x0f];
    buf[3] = symbol_map[(signature >> 12) & 0x0f];
    buf[4] = '\0';
    c_put(di, ss, es, s->out_ann, ANN_SIGNATURE, buf);
}

static void sig_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(sig_state)));
    sig_state *s = (sig_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(sig_state));
    s->out_ann = -1;
    s->prev_start = 0; /* default: rising edge active */
    s->prev_stop = 0;
}

static void sig_start(struct srd_decoder_inst *di)
{
    sig_state *s = (sig_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "signature");

    const char *start_edge = c_opt_str(di, "start_edge", "rising");
    s->start_edge_rising = (strcmp(start_edge, "rising") == 0) ? 1 : 0;
    s->prev_start = s->start_edge_rising ? 0 : 1;

    const char *stop_edge = c_opt_str(di, "stop_edge", "rising");
    s->stop_edge_rising = (strcmp(stop_edge, "rising") == 0) ? 1 : 0;
    s->prev_stop = s->stop_edge_rising ? 0 : 1;

    const char *annbits_str = c_opt_str(di, "annbits", "no");
    s->annbits = (strcmp(annbits_str, "yes") == 0) ? 1 : 0;
}

static void sig_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    sig_state *s = (sig_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void sig_decode(struct srd_decoder_inst *di)
{
    sig_state *s = (sig_state *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);

    const char *clk_edge_str = c_opt_str(di, "clk_edge", "falling");
    int clk_edge_rising = (strcmp(clk_edge_str, "rising") == 0) ? 1 : 0;

    while (1) {
        int ret;
        if (clk_edge_rising)
            ret = c_wait(di, CW_R(CH_CLK), CW_END);
        else
            ret = c_wait(di, CW_F(CH_CLK), CW_END);
        if (ret != SRD_OK)
            return;

        int start = c_pin(di, CH_START);
        int stop  = c_pin(di, CH_STOP);
        int data  = c_pin(di, CH_DATA);

        if (start != s->prev_start && !s->gate_is_open) {
            s->gate_is_open = s->start_edge_rising ? (start == 1) : (start == 0);
            if (s->gate_is_open) {
                s->sample_start = di_samplenum(di);
                s->started = 1;
            }
        } else if (stop != s->prev_stop && s->gate_is_open) {
            int stop_active = s->stop_edge_rising ? (stop == 1) : (stop == 0);
            s->gate_is_open = !stop_active;
            if (!s->gate_is_open) {
                if (s->annbits)
                    c_put(di, s->last_samplenum, di_samplenum(di), s->out_ann, ANN_STOP, "STOP", "STP", "P");
                sig_putsig(di, s, s->sample_start, di_samplenum(di), s->shiftreg);
                s->shiftreg = 0;
                s->sample_start = 0;
            }
        }

        if (s->gate_is_open) {
            if (s->annbits) {
                if (s->started) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "<%d>", data);
                    char buf2[128], buf3[128];
                    snprintf(buf2, sizeof(buf2), "START%s", buf + 1);
                    snprintf(buf3, sizeof(buf3), "S%s", buf + 1);
                    c_put(di, s->last_samplenum, di_samplenum(di), s->out_ann, ANN_START, buf2, buf3, buf);
                    s->started = 0;
                } else {
                    char bit_str[16];
                    snprintf(bit_str, sizeof(bit_str), "%d", data);
                    int ann_cls = data ? ANN_BIT1 : ANN_BIT0;
                    c_put(di, s->last_samplenum, di_samplenum(di), s->out_ann, ann_cls, bit_str);
                }
            }
            int incoming = (popcount16(s->shiftreg & 0x0291) + data) & 1;
            s->shiftreg = (uint16_t)((incoming << 15) | (s->shiftreg >> 1));
        }

        s->prev_start = start;
        s->prev_stop = stop;
        s->last_samplenum = di_samplenum(di);
    }
}

static void sig_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder signature_c_decoder = {
    .id = "signature_c",
    .name = "Signature(C)",
    .longname = "Signature analysis (C)",
    .desc = "Annotate signature of logic patterns. (C implementation)",
    .license = "gplv2+",
    .channels = sig_channels,
    .num_channels = 4,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = sig_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = sig_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = sig_ann_rows,
    .inputs = sig_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sig_tags,
    .num_tags = 3,
    .reset = sig_reset,
    .start = sig_start,
    .decode = sig_decode,
    .destroy = sig_destroy,
    .state_size = 0,
    .metadata = sig_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    sig_options[0].def = g_variant_new_string("rising");
    GSList *start_vals = NULL;
    start_vals = g_slist_append(start_vals, g_variant_new_string("rising"));
    start_vals = g_slist_append(start_vals, g_variant_new_string("falling"));
    sig_options[0].values = start_vals;

    sig_options[1].def = g_variant_new_string("rising");
    GSList *stop_vals = NULL;
    stop_vals = g_slist_append(stop_vals, g_variant_new_string("rising"));
    stop_vals = g_slist_append(stop_vals, g_variant_new_string("falling"));
    sig_options[1].values = stop_vals;

    sig_options[2].def = g_variant_new_string("falling");
    GSList *clk_vals = NULL;
    clk_vals = g_slist_append(clk_vals, g_variant_new_string("rising"));
    clk_vals = g_slist_append(clk_vals, g_variant_new_string("falling"));
    sig_options[2].values = clk_vals;

    sig_options[3].def = g_variant_new_string("no");
    GSList *ann_vals = NULL;
    ann_vals = g_slist_append(ann_vals, g_variant_new_string("yes"));
    ann_vals = g_slist_append(ann_vals, g_variant_new_string("no"));
    sig_options[3].values = ann_vals;

    return &signature_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}