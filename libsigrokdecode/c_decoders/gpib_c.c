/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 Rudolf Reuter <reuterru@arcor.de>
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

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_ITEMS = 0,
    ANN_GPIB,
    ANN_EOI,
    NUM_ANN,
};

struct gpib_priv {
    int items[16];
    int itemcount;
    int saved_item;
    int saved_ATN;
    int saved_EOI;
    uint64_t ss_item;
    uint64_t ss_word;
    int first;
    int64_t sample_total;
    int out_ann;
};

static struct srd_channel gpib_channels[] = {
    {"dio1", "DIO1", "Data I/O bit 1", 0, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio1"},
    {"dio2", "DIO2", "Data I/O bit 2", 1, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio2"},
    {"dio3", "DIO3", "Data I/O bit 3", 2, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio3"},
    {"dio4", "DIO4", "Data I/O bit 4", 3, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio4"},
    {"dio5", "DIO5", "Data I/O bit 5", 4, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio5"},
    {"dio6", "DIO6", "Data I/O bit 6", 5, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio6"},
    {"dio7", "DIO7", "Data I/O bit 7", 6, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio7"},
    {"dio8", "DIO8", "Data I/O bit 8", 7, SRD_CHANNEL_SDATA, "dec_gpib_chan_dio8"},
    {"eoi", "EOI", "End or identify", 8, SRD_CHANNEL_SDATA, "dec_gpib_chan_eoi"},
    {"dav", "DAV", "Data valid", 9, SRD_CHANNEL_SDATA, "dec_gpib_chan_dav"},
    {"nrfd", "NRFD", "Not ready for data", 10, SRD_CHANNEL_SDATA, "dec_gpib_chan_nrfd"},
    {"ndac", "NDAC", "Not data accepted", 11, SRD_CHANNEL_SDATA, "dec_gpib_chan_ndac"},
    {"ifc", "IFC", "Interface clear", 12, SRD_CHANNEL_SDATA, "dec_gpib_chan_ifc"},
    {"srq", "SRQ", "Service request", 13, SRD_CHANNEL_SDATA, "dec_gpib_chan_srq"},
    {"atn", "ATN", "Attention", 14, SRD_CHANNEL_SDATA, "dec_gpib_chan_atn"},
    {"ren", "REN", "Remote enable", 15, SRD_CHANNEL_SDATA, "dec_gpib_chan_ren"},
};

static struct srd_decoder_option gpib_options[] = {
    {"sample_total", "dec_gpib_opt_sample_total", "Total number of samples", NULL, NULL},
};

static const char *gpib_ann_labels[][3] = {
    {"", "items", "Items"},
    {"", "gpib", "DAT/CMD"},
    {"", "eoi", "EOI"},
};

static const int gpib_row_bytes_classes[] = {ANN_ITEMS};
static const int gpib_row_gpib_classes[] = {ANN_GPIB};
static const int gpib_row_eoi_classes[] = {ANN_EOI};
static const struct srd_c_ann_row gpib_ann_rows[] = {
    {"bytes", "Bytes", gpib_row_bytes_classes, 1},
    {"gpib", "DAT/CMD", gpib_row_gpib_classes, 1},
    {"eoi", "EOI", gpib_row_eoi_classes, 1},
};

static const char *gpib_inputs[] = {"logic", NULL};
static const char *gpib_outputs[] = {NULL};
static const char *gpib_tags[] = {"PC", NULL};

static void gpib_handle_bits(struct srd_decoder_inst *di, const uint8_t *pins, uint64_t samplenum)
{
    (void)samplenum;
    struct gpib_priv *s = (struct gpib_priv *)c_decoder_get_private(di);
    int item2 = 0;
    int item3 = 0;

    if (s->itemcount == 0) {
        s->ss_word = samplenum;
    }

    /* Get the bits for this item */
    int item = 0;
    for (int i = 0; i < 8; i++) {
        item |= pins[i] << i;
    }
    item = item ^ 0xff; /* Invert data byte (GPIB is active low) */

    s->items[s->itemcount] = item;
    s->itemcount++;

    if (pins[14] == 0)
        item2 = 1; /* ATN active */
    if (pins[8] == 0)
        item3 = 1; /* EOI active */

    if (s->first) {
        /* Save the start sample and item for later (no output yet) */
        s->ss_item = samplenum;
        s->first = 0;
        s->saved_item = item;
        s->saved_ATN = item2;
        s->saved_EOI = item3;
    } else {
        /* Output the saved item */
        int dbyte = s->saved_item;
        int dATN = s->saved_ATN;
        int dEOI = s->saved_EOI;
        uint64_t es_item = samplenum;

        char buf[32];
        snprintf(buf, sizeof(buf), "%02X", dbyte);
        c_put(di, s->ss_item, es_item, s->out_ann, ANN_ITEMS, buf);

        /* Encode item byte to GPIB convention */
        const char *strgpib = " ";
        char gpib_buf[8];
        if (dATN) {
            switch (dbyte) {
            case 0x01: strgpib = "GTL"; break;
            case 0x04: strgpib = "SDC"; break;
            case 0x05: strgpib = "PPC"; break;
            case 0x08: strgpib = "GET"; break;
            case 0x09: strgpib = "TCT"; break;
            case 0x11: strgpib = "LLO"; break;
            case 0x14: strgpib = "DCL"; break;
            case 0x15: strgpib = "PPU"; break;
            case 0x18: strgpib = "SPE"; break;
            case 0x19: strgpib = "SPD"; break;
            case 0x3f: strgpib = "UNL"; break;
            case 0x5f: strgpib = "UNT"; break;
            default:
                if (dbyte > 0x1f && dbyte < 0x3f) {
                    snprintf(gpib_buf, sizeof(gpib_buf), "L%c", (char)(dbyte + 0x10));
                    strgpib = gpib_buf;
                } else if (dbyte > 0x3f && dbyte < 0x5f) {
                    snprintf(gpib_buf, sizeof(gpib_buf), "T%c", (char)(dbyte - 0x10));
                    strgpib = gpib_buf;
                }
                break;
            }
        } else {
            if (dbyte > 0x1f && dbyte < 0x7f) {
                snprintf(gpib_buf, sizeof(gpib_buf), "%c", (char)dbyte);
                strgpib = gpib_buf;
            }
            if (dbyte == 0x0a)
                strgpib = "LF";
            if (dbyte == 0x0d)
                strgpib = "CR";
        }

        c_put(di, s->ss_item, es_item, s->out_ann, ANN_GPIB, strgpib);

        const char *strEOI = " ";
        if (dEOI)
            strEOI = "EOI";
        c_put(di, s->ss_item, es_item, s->out_ann, ANN_EOI, strEOI);

        s->ss_item = samplenum;
        s->saved_item = item;
        s->saved_ATN = item2;
        s->saved_EOI = item3;
    }

    if (s->itemcount < 16)
        return;

    s->itemcount = 0;
    memset(s->items, 0, sizeof(s->items));
}

static void gpib_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct gpib_priv)));
    }
    struct gpib_priv *s = (struct gpib_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct gpib_priv));
    s->first = 1;
    s->sample_total = 0;
    s->out_ann = -1;
}

static void gpib_start(struct srd_decoder_inst *di)
{
    struct gpib_priv *s = (struct gpib_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "gpib");
    s->sample_total = c_opt_int(di, "sample_total", 0);
}

static void gpib_decode(struct srd_decoder_inst *di)
{
    struct gpib_priv *s = (struct gpib_priv *)c_decoder_get_private(di);
    int first_wait = 1;

    while (1) {
        int ret;
        if (first_wait)
            ret = c_wait(di, CW_L(9), CW_END);
        else
            ret = c_wait(di, CW_F(9), CW_END);
        if (s->sample_total > 0)
            ret = c_wait(di, CW_OR, CW_SKIP((uint64_t)s->sample_total), CW_END);
        if (ret != SRD_OK)
            return;

        /* Read all 16 channels */
        uint8_t pins[16];
        for (int i = 0; i < 16; i++) {
            pins[i] = c_pin(di, i);
        }

        gpib_handle_bits(di, pins, di_samplenum(di));
        first_wait = 0;
    }
}

static void gpib_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder gpib_c_decoder = {
    .id = "gpib_c",
    .name = "GPIB(C)",
    .longname = "General Purpose Interface Bus (C)",
    .desc = "IEEE-488 General Purpose Interface Bus (GPIB / HPIB). (C implementation)",
    .license = "gplv2+",
    .channels = gpib_channels,
    .num_channels = 16,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = gpib_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = gpib_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = gpib_ann_rows,
    .inputs = gpib_inputs,
    .num_inputs = 1,
    .outputs = gpib_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = gpib_tags,
    .num_tags = 1,
    .reset = gpib_reset,
    .start = gpib_start,
    .decode = gpib_decode,
    .destroy = gpib_destroy,
    .state_size = 0,
    .metadata = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &gpib_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}