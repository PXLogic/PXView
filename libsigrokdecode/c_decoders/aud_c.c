/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 fenugrec <fenugrec users.sourceforge.net>
 * Copyright (C) 2019 DreamSourceLab <support@dreamsourcelab.com>
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
    ANN_DEST = 0,
    NUM_ANN,
};

typedef struct {
    int ncnt;
    int nmax;
    uint32_t addr;
    uint32_t lastaddr;
    uint64_t ss;
    int out_ann;
} aud_priv;

static struct srd_channel aud_channels[] = {
    {"audck",    "AUDCK",    "AUD clock",       0, SRD_CHANNEL_SCLK,  "dec_aud_chan_audck"},
    {"naudsync", "nAUDSYNC", "AUD sync",        1, SRD_CHANNEL_SDATA, "dec_aud_chan_naudsync"},
    {"audata3",  "AUDATA3",  "AUD data line 3", 2, SRD_CHANNEL_ADATA, "dec_aud_chan_audata3"},
    {"audata2",  "AUDATA2",  "AUD data line 2", 3, SRD_CHANNEL_ADATA, "dec_aud_chan_audata2"},
    {"audata1",  "AUDATA1",  "AUD data line 1", 4, SRD_CHANNEL_ADATA, "dec_aud_chan_audata1"},
    {"audata0",  "AUDATA0",  "AUD data line 0", 5, SRD_CHANNEL_ADATA, "dec_aud_chan_audata0"},
};

static const char *aud_ann_labels[][3] = {
    {"", "dest", "Destination address"},
};

static const int aud_row_addresses_classes[] = {ANN_DEST, -1};
static const struct srd_c_ann_row aud_ann_rows[] = {
    {"addresses", "Addresses", aud_row_addresses_classes, 1},
};

static const char *aud_inputs[] = {"logic"};
static const char *aud_tags[] = {"Debug/trace"};

static void aud_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(aud_priv)));
    }
    aud_priv *s = (aud_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(aud_priv));
    s->out_ann = -1;
}

static void aud_start(struct srd_decoder_inst *di)
{
    aud_priv *s = (aud_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "aud");
}

static void aud_decode(struct srd_decoder_inst *di)
{
    aud_priv *s = (aud_priv *)c_decoder_get_private(di);
    while (1) {
        int ret = c_wait(di, CW_R(0), CW_END);
        if (ret != SRD_OK)
            return;

        /* Read all pins */
        int sync = c_pin(di, 1);
        int d3   = c_pin(di, 2);
        int d2   = c_pin(di, 3);
        int d1   = c_pin(di, 4);
        int d0   = c_pin(di, 5);

        /* Reconstruct nibble: audata3=MSB, audata0=LSB */
        int nib = (d3 << 3) | (d2 << 2) | (d1 << 1) | d0;

        if (sync == 1) {
            /* sync == 1: annotate if finished; update cmd */
            if (s->ncnt == s->nmax && s->nmax != 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%08X", s->addr);
                c_put(di, s->ss, di_samplenum(di), s->out_ann, ANN_DEST, buf);
                s->lastaddr = s->addr;
            }
            s->ncnt = 0;
            s->addr = s->lastaddr;
            s->ss = di_samplenum(di);
            if (nib == 0x08)
                s->nmax = 1;
            else if (nib == 0x09)
                s->nmax = 2;
            else if (nib == 0x0A)
                s->nmax = 4;
            else if (nib == 0x0B)
                s->nmax = 8;
            else
                s->nmax = 0;
        } else {
            /* sync == 0, valid cmd: start or continue shifting in nibbles */
            if (s->nmax > 0) {
                /* Clear target nibble, then set */
                s->addr &= ~(0x0F << (s->ncnt * 4));
                s->addr |= nib << (s->ncnt * 4);
                s->ncnt++;
            }
        }
    }
}

static void aud_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder aud_c_decoder = {
    .id = "aud_c",
    .name = "AUD(C)",
    .longname = "Advanced User Debugger(C)",
    .desc = "Renesas/Hitachi Advanced User Debugger (AUD) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = aud_channels,
    .num_channels = 6,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = aud_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = aud_ann_rows,
    .inputs = aud_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = aud_tags,
    .num_tags = 1,
    .reset = aud_reset,
    .start = aud_start,
    .decode = aud_decode,
    .destroy = aud_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &aud_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}