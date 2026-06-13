/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Jeremy Swanson <jeremy@rakocontrols.com>
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

enum dsi_ann {
    ANN_BIT = 0,
    ANN_STARTBIT,
    ANN_LEVEL,
    ANN_RAW,
    NUM_ANN,
};

enum dsi_state {
    DSI_IDLE,
    DSI_PHASE0,
    DSI_PHASE1,
};

typedef struct {
    uint64_t samplerate;
    int halfbit;
    int old_dsi;
    int state;
    int phase0;
    int polarity_active_high; /* 1 if active-high, 0 if active-low */

    uint64_t edges[64];
    int edges_count;
    struct { uint64_t pos; int val; } bits[32];
    int bits_count;
    uint64_t ss_es_bits[32][2];
    int ss_es_bits_count;

    int out_ann;
} dsi_state;

static struct srd_channel dsi_channels[] = {
    { "dsi", "DSI", "DSI data line", 0, SRD_CHANNEL_SDATA, "dec_dsi_chan_dsi" },
};

static struct srd_decoder_option dsi_options[] = {
    { "polarity", "dec_dsi_opt_polarity", "Polarity", NULL, NULL },
};

static const char* dsi_ann_labels[][3] = {
    { "", "Bit", "Bit" },
    { "", "Start bit", "Start bit" },
    { "", "Dimmer level", "Dimmer level" },
    { "", "Raw data", "Raw data" },
};

static const int dsi_row_bits_classes[] = { ANN_BIT, -1 };
static const int dsi_row_raw_classes[] = { ANN_RAW, -1 };
static const int dsi_row_fields_classes[] = { ANN_STARTBIT, ANN_LEVEL, -1 };

static const struct srd_c_ann_row dsi_ann_rows[] = {
    { "bits", "Bits", dsi_row_bits_classes, 1 },
    { "raw", "Raw data", dsi_row_raw_classes, 1 },
    { "fields", "Fields", dsi_row_fields_classes, 2 },
};

static const char* dsi_inputs[] = { "logic" };
static const char* dsi_tags[] = { "Embedded/industrial", "Lighting" };

static void dsi_reset_decoder_state(dsi_state *s)
{
    s->edges_count = 0;
    s->bits_count = 0;
    s->ss_es_bits_count = 0;
    s->state = DSI_IDLE;
}

#define DSI_PUTB(di, s, bit1, bit2, cls, ...) do { \
    if ((bit1) >= 0 && (bit1) < (s)->ss_es_bits_count && (bit2) >= 0 && (bit2) < (s)->ss_es_bits_count) \
        c_put(di, (s)->ss_es_bits[bit1][0], (s)->ss_es_bits[bit2][1], (s)->out_ann, cls, __VA_ARGS__); \
} while(0)

static void dsi_handle_bits(struct srd_decoder_inst *di, dsi_state *s, int length)
{
    int f = 0;

    /* Individual raw bits */
    for (int i = 0; i < length && i < 32; i++) {
        uint64_t ss;
        if (i == 0)
            ss = s->bits[0].pos;
        else
            ss = s->ss_es_bits[i - 1][1];
        uint64_t es = s->bits[i].pos + (s->halfbit * 2);
        if (i < 32) {
            s->ss_es_bits[i][0] = ss;
            s->ss_es_bits[i][1] = es;
        }
        s->ss_es_bits_count = i + 1;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s->bits[i].val);
        DSI_PUTB(di, s, i, i, ANN_BIT, buf);
    }

    /* Bits[0]: Startbit */
    if (length > 0) {
        char st_buf[64], st_short[16], st_tiny[8], st_t[4], st_s[4];
        snprintf(st_buf, sizeof(st_buf), "Startbit: %d", s->bits[0].val);
        snprintf(st_short, sizeof(st_short), "ST: %d", s->bits[0].val);
        snprintf(st_tiny, sizeof(st_tiny), "ST");
        snprintf(st_t, sizeof(st_t), "S");
        snprintf(st_s, sizeof(st_s), "S");
        DSI_PUTB(di, s, 0, 0, ANN_STARTBIT, st_buf, st_short, st_tiny, st_t, st_s);
        DSI_PUTB(di, s, 0, 0, ANN_RAW, st_buf, st_short, st_tiny, st_t, st_s);
    }

    /* Bits[1:8] */
    for (int i = 0; i < 8 && (1 + i) < length; i++) {
        f |= (s->bits[1 + i].val << (7 - i));
    }
    double g = f / 2.55;

    if (length == 9) {
        /* BACKWARD Frame */
        char data_buf[64], data_short[32], data_tiny[16], data_t[8], data_s[4];
        snprintf(data_buf, sizeof(data_buf), "Data: %02X", f);
        snprintf(data_short, sizeof(data_short), "Dat: %02X", f);
        snprintf(data_tiny, sizeof(data_tiny), "Dat: %02X", f);
        snprintf(data_t, sizeof(data_t), "D: %02X", f);
        snprintf(data_s, sizeof(data_s), "D");
        if (s->ss_es_bits_count > 8)
            DSI_PUTB(di, s, 1, 8, ANN_RAW, data_buf, data_short, data_tiny, data_t, data_s);

        char lev_buf[64], lev_short[32], lev_tiny[16], lev_t[8], lev_s[4];
        snprintf(lev_buf, sizeof(lev_buf), "Level: %d%%", (int)g);
        snprintf(lev_short, sizeof(lev_short), "Lev: %d%%", (int)g);
        snprintf(lev_tiny, sizeof(lev_tiny), "Lev: %d%%", (int)g);
        snprintf(lev_t, sizeof(lev_t), "L: %d", (int)g);
        snprintf(lev_s, sizeof(lev_s), "D");
        if (s->ss_es_bits_count > 8)
            DSI_PUTB(di, s, 1, 8, ANN_LEVEL, lev_buf, lev_short, lev_tiny, lev_t, lev_s);
    }
}

static void dsi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(dsi_state)));
    }
    dsi_state *s = (dsi_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(dsi_state));
    s->out_ann = -1;
    s->state = DSI_IDLE;
}

static void dsi_start(struct srd_decoder_inst *di)
{
    dsi_state *s = (dsi_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "dsi");

    const char *polarity_str = c_opt_str(di, "polarity", "active-high");
    s->polarity_active_high = (strcmp(polarity_str, "active-high") == 0) ? 1 : 0;
    s->old_dsi = s->polarity_active_high ? 0 : 1;
}

static void dsi_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    dsi_state *s = (dsi_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->halfbit = (int)((s->samplerate * 0.0016667) / 2.0);
    }
}

static void dsi_decode(struct srd_decoder_inst *di)
{
    dsi_state *s = (dsi_state *)c_decoder_get_private(di);

    if (s->samplerate == 0) return;

    while (1) {
        /* Wait for edge OR timeout (skip to 1.5*halfbit after last edge).
         * This mirrors the Python decoder's per-sample timeout logic:
         *   elif self.samplenum == (self.edges[-1] + int(self.halfbit * 1.5)):
         * The C decoder can't process every sample efficiently, so we use
         * CW_SKIP to wake up at the timeout point if no edge arrives first. */
        uint64_t skip_to = 0;
        if (s->edges_count > 0 && s->state != DSI_IDLE) {
            uint64_t timeout_sample = s->edges[s->edges_count - 1] + (uint64_t)(s->halfbit * 3 / 2);
            if (timeout_sample > di_samplenum(di))
                skip_to = timeout_sample - di_samplenum(di);
        }

        int ret;
        if (skip_to > 0)
            ret = c_wait(di, CW_E(0), CW_OR, CW_SKIP(skip_to), CW_END);
        else
            ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int dsi = c_pin(di, 0);
        if (s->polarity_active_high)
            dsi ^= 1; /* Invert for active-high */

        if (s->state == DSI_IDLE) {
            if (s->old_dsi == dsi)
                continue;
            /* Add in the first half of the start bit */
            if (s->edges_count < 64) {
                s->edges[s->edges_count++] = di_samplenum(di) - s->halfbit;
                s->edges[s->edges_count++] = di_samplenum(di);
            }
            s->phase0 = dsi ^ 1;
            s->state = DSI_PHASE1;
            s->old_dsi = dsi;
            continue;
        }

        /* Check for edge or half-bit timeout */
        if (s->old_dsi != dsi) {
            /* Real edge detected */
            if (s->edges_count < 64)
                s->edges[s->edges_count++] = di_samplenum(di);
        } else if (s->edges_count > 0 && di_samplenum(di) == (s->edges[s->edges_count - 1] + (uint64_t)(s->halfbit * 3 / 2))) {
            /* Half-bit timeout: insert virtual edge */
            if (s->edges_count < 64)
                s->edges[s->edges_count++] = di_samplenum(di) - (uint64_t)(s->halfbit / 2);
        } else {
            continue;
        }

        int bit = s->old_dsi;
        if (s->state == DSI_PHASE0) {
            s->phase0 = bit;
            s->state = DSI_PHASE1;
        } else if (s->state == DSI_PHASE1) {
            if (bit == 1 && s->phase0 == 1) {
                /* Stop bit */
                if (s->bits_count == 17 || s->bits_count == 9) {
                    dsi_handle_bits(di, s, s->bits_count);
                }
                dsi_reset_decoder_state(s);
                s->old_dsi = dsi;
                continue;
            } else {
                if (s->bits_count < 32 && s->edges_count >= 3) {
                    s->bits[s->bits_count].pos = s->edges[s->edges_count - 3];
                    s->bits[s->bits_count].val = bit;
                    s->bits_count++;
                }
                s->state = DSI_PHASE0;
            }
        }

        s->old_dsi = dsi;
    }
}

static void dsi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder dsi_c_decoder = {
    .id = "dsi_c",
    .name = "DSI(C)",
    .longname = "Digital Serial Interface (C)",
    .desc = "Digital Serial Interface (DSI) lighting protocol.",
    .license = "gplv2+",
    .channels = dsi_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = dsi_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = dsi_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = dsi_ann_rows,
    .inputs = dsi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = dsi_tags,
    .num_tags = 2,
    .reset = dsi_reset,
    .start = dsi_start,
    .decode = dsi_decode,
    .destroy = dsi_destroy,
    .state_size = 0,
    .metadata = dsi_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &dsi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}