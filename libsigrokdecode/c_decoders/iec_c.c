/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Marcus Comstedt <marcus@mc.pp.se>
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

struct iec_priv {
    int step;
    int saved_ATN;
    int saved_EOI;
    uint64_t ss_item;
    uint64_t es_item;
    uint8_t bits;
    int numbits;
    int out_ann;
};

static struct srd_channel iec_channels[] = {
    {"data", "DATA", "Data I/O", 0, SRD_CHANNEL_SDATA, "dec_iec_chan_data"},
    {"clk", "CLK", "Clock", 1, SRD_CHANNEL_SCLK, "dec_iec_chan_clk"},
    {"atn", "ATN", "Attention", 2, SRD_CHANNEL_SDATA, "dec_iec_chan_atn"},
};

static struct srd_channel iec_optional_channels[] = {
    {"srq", "SRQ", "Service request", 0, SRD_CHANNEL_SDATA, "dec_iec_opt_chan_srq"},
};

static const char *iec_ann_labels[][3] = {
    {"", "items", "Items"},
    {"", "gpib", "DAT/CMD"},
    {"", "eoi", "EOI"},
};

static const int iec_row_bytes_classes[] = {ANN_ITEMS};
static const int iec_row_gpib_classes[] = {ANN_GPIB};
static const int iec_row_eoi_classes[] = {ANN_EOI};
static const struct srd_c_ann_row iec_ann_rows[] = {
    {"bytes", "Bytes", iec_row_bytes_classes, 1},
    {"gpib", "DAT/CMD", iec_row_gpib_classes, 1},
    {"eoi", "EOI", iec_row_eoi_classes, 1},
};

static const char *iec_inputs[] = {"logic", NULL};
static const char *iec_outputs[] = {NULL};
static const char *iec_tags[] = {"PC", "Retro computing", NULL};

static void iec_handle_bits(struct srd_decoder_inst *di, uint64_t samplenum)
{
    (void)samplenum;
    struct iec_priv *s = (struct iec_priv *)c_decoder_get_private(di);
    int dbyte = s->bits;
    int dATN = s->saved_ATN;
    int dEOI = s->saved_EOI;

    s->es_item = samplenum;

    char buf[32];
    snprintf(buf, sizeof(buf), "%02X", dbyte);
    c_put(di, s->ss_item, s->es_item, s->out_ann, ANN_ITEMS, buf);

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
            } else if (dbyte > 0x5f && dbyte < 0x70) {
                snprintf(gpib_buf, sizeof(gpib_buf), "R%c", (char)(dbyte - 0x30));
                strgpib = gpib_buf;
            } else if (dbyte > 0xdf && dbyte < 0xf0) {
                snprintf(gpib_buf, sizeof(gpib_buf), "C%c", (char)(dbyte - 0xb0));
                strgpib = gpib_buf;
            } else if (dbyte > 0xef) {
                snprintf(gpib_buf, sizeof(gpib_buf), "O%c", (char)(dbyte - 0xc0));
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

    c_put(di, s->ss_item, s->es_item, s->out_ann, ANN_GPIB, strgpib);

    const char *strEOI = " ";
    if (dEOI)
        strEOI = "EOI";
    c_put(di, s->ss_item, s->es_item, s->out_ann, ANN_EOI, strEOI);
}

static void iec_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct iec_priv)));
    }
    struct iec_priv *s = (struct iec_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct iec_priv));
    s->out_ann = -1;
}

static void iec_start(struct srd_decoder_inst *di)
{
    struct iec_priv *s = (struct iec_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "iec");
}

static void iec_decode(struct srd_decoder_inst *di)
{
    struct iec_priv *s = (struct iec_priv *)c_decoder_get_private(di);
    while (1) {
        int ret;
        switch (s->step) {
        case 0:
            /* [{2: 'f'}, {0: 'l', 1: 'h'}] */
            ret = c_wait(di, CW_F(2), CW_OR, CW_L(0), CW_H(1), CW_END);
            break;
        case 1:
            /* [{2: 'f'}, {0: 'h', 1: 'h'}, {1: 'l'}] */
            ret = c_wait(di, CW_F(2), CW_OR, CW_H(0), CW_H(1), CW_OR, CW_L(1), CW_END);
            break;
        case 2:
            /* [{2: 'f'}, {0: 'f'}, {1: 'l'}] */
            ret = c_wait(di, CW_F(2), CW_OR, CW_F(0), CW_OR, CW_L(1), CW_END);
            break;
        case 3:
            /* [{2: 'f'}, {1: 'e'}] */
            ret = c_wait(di, CW_F(2), CW_OR, CW_E(1), CW_END);
            break;
        default:
            return;
        }
        if (ret != SRD_OK)
            return;

        int data = c_pin(di, 0);
        int clk = c_pin(di, 1);
        int atn = c_pin(di, 2);

        /* ATN falling edge always resets step */
        if (di_matched(di) & 1) {
            s->step = 0;
        }

        if (s->step == 0) {
            if (data == 0 && clk == 1) {
                s->step = 1;
            }
        } else if (s->step == 1) {
            if (data == 1 && clk == 1) {
                s->ss_item = di_samplenum(di);
                s->saved_ATN = !atn;
                s->saved_EOI = 0;
                s->bits = 0;
                s->numbits = 0;
                s->step = 2;
            } else if (clk == 0) {
                s->step = 0;
            }
        } else if (s->step == 2) {
            if (data == 0 && clk == 1) {
                s->saved_EOI = 1;
            } else if (clk == 0) {
                s->step = 3;
            }
        } else if (s->step == 3) {
            if (di_matched(di) & 2) {
                if (clk == 1) {
                    /* Rising edge on CLK: latch DATA */
                    s->bits |= (uint8_t)(data << s->numbits);
                } else {
                    /* Falling edge on CLK: end of bit */
                    s->numbits++;
                    if (s->numbits == 8) {
                        iec_handle_bits(di, di_samplenum(di));
                        s->step = 0;
                    }
                }
            }
        }
    }
}

static void iec_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder iec_c_decoder = {
    .id = "iec_c",
    .name = "IEC(C)",
    .longname = "Commodore IEC bus (C)",
    .desc = "Commodore serial IEEE-488 (IEC) bus protocol. (C implementation)",
    .license = "gplv2+",
    .channels = iec_channels,
    .num_channels = 3,
    .optional_channels = iec_optional_channels,
    .num_optional_channels = 1,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = iec_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = iec_ann_rows,
    .inputs = iec_inputs,
    .num_inputs = 1,
    .outputs = iec_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = iec_tags,
    .num_tags = 2,
    .reset = iec_reset,
    .start = iec_start,
    .decode = iec_decode,
    .destroy = iec_destroy,
    .state_size = 0,
    .metadata = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &iec_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}