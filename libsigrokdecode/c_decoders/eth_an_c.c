/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2021 Quard <2014500726@smail.xtu.edu.cn>
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
    ANN_DATA = 0,    /* FLP data */
    ANN_FORMAT,      /* format describe */
    ANN_BITD,        /* Bit desc */
    ANN_BIT,         /* Bit */
    ANN_NLP,         /* Normal link pulses */
    NUM_ANN,
};

enum eth_an_state {
    STATE_BASE_PAGE,
    STATE_BASE_PAGE_ACK,
    STATE_NEXT_PAGE,
    STATE_NEXT_PAGE_ACK,
};

struct eth_an_priv {
    uint64_t samplerate;
    
    uint64_t pre_ss, pre_es;
    uint64_t last_valid_ss;
    uint16_t hex;
    uint16_t pre_hex;
    int index;
    int state;

    /* data_list: 16 entries, each with 4 uint64_t */
    uint64_t dl_start[16];
    uint64_t dl_end[16];
    uint64_t dl_pre_start[16];
    uint64_t dl_pre_end[16];

    int out_ann;
    uint64_t ss;
    uint64_t es;
};

static struct srd_channel eth_an_channels[] = {
    {"dp", "TX+", "ETH TX+ signal", 0, SRD_CHANNEL_ADATA, "dec_eth_an_chan_dp"},
};

static const char *eth_an_ann_labels[][3] = {
    {"", "data", "FLP data"},
    {"", "format", "format describe"},
    {"", "bitd", "Bit desc"},
    {"", "bit", "Bit"},
    {"", "nlp", "Normal link pulses"},
};

static const int eth_an_row_data_classes[] = {ANN_DATA};
static const int eth_an_row_format_classes[] = {ANN_FORMAT};
static const int eth_an_row_bitd_classes[] = {ANN_BITD};
static const int eth_an_row_bit_classes[] = {ANN_BIT};
static const int eth_an_row_nlp_classes[] = {ANN_NLP};
static const struct srd_c_ann_row eth_an_ann_rows[] = {
    {"data", "Data", eth_an_row_data_classes, 1},
    {"format", "Format", eth_an_row_format_classes, 1},
    {"bitd", "Bit desc", eth_an_row_bitd_classes, 1},
    {"bit", "Bit", eth_an_row_bit_classes, 1},
    {"nlp", "NLP", eth_an_row_nlp_classes, 1},
};

static const char *eth_an_inputs[] = {"logic", NULL};
static const char *eth_an_outputs[] = {"eth_an", NULL};
static const char *eth_an_tags[] = {"PC", NULL};

static void eth_an_change_state(struct eth_an_priv *s)
{
    if (s->pre_hex != s->hex) {
        if (s->state == STATE_BASE_PAGE) {
            if (((s->hex >> 14) & 0x3) == 0x3)
                s->state = STATE_BASE_PAGE_ACK;
        } else if (s->state == STATE_BASE_PAGE_ACK) {
            s->state = STATE_NEXT_PAGE;
        } else if (s->state == STATE_NEXT_PAGE) {
            if (((s->hex >> 14) & 0x3) == 0x1)
                s->state = STATE_NEXT_PAGE_ACK;
        } else if (s->state == STATE_NEXT_PAGE_ACK) {
            s->state = STATE_BASE_PAGE;
        }
    }
}

static void eth_an_decode_base_page(struct srd_decoder_inst *di, struct eth_an_priv *s)
{
    static const char *base_page_ta_dict[] = {
        NULL, NULL, NULL, NULL, NULL,
        "10BaseT-HD", "10BaseT-FD", "100BaseTX-HD",
        "100BaseTX-FD", "100BaseT4", "FC", "AsyFC",
        "Reserved", "RF", "ACK", "NP"
    };

    const char *type_desc;
    if ((s->hex & 0x1f) == 0x1)
        type_desc = "802.3";
    else if ((s->hex & 0x1f) == 0x2)
        type_desc = "802.9";
    else
        type_desc = "unknow";

    for (int i = 0; i < 16; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%x", (s->hex >> i) & 0x1);
        c_put(di, s->dl_pre_start[i], s->dl_end[i], s->out_ann, ANN_BIT, buf);

        if (i >= 5 && base_page_ta_dict[i]) {
            c_put(di, s->dl_pre_start[i], s->dl_end[i], s->out_ann, ANN_BITD, base_page_ta_dict[i]);
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "base page:0x%x", s->hex);
    c_put(di, s->dl_pre_start[0], s->dl_end[15], s->out_ann, ANN_DATA, buf);

    snprintf(buf, sizeof(buf), "Selector field:0x%x", s->hex & 0x1f);
    c_put(di, s->dl_pre_start[0], s->dl_end[4], s->out_ann, ANN_FORMAT, buf);

    c_put(di, s->dl_pre_start[0], s->dl_end[4], s->out_ann, ANN_BITD, type_desc);

    snprintf(buf, sizeof(buf), "Technology ability field:0x%x", (s->hex >> 5) & 0xff);
    c_put(di, s->dl_pre_start[5], s->dl_end[12], s->out_ann, ANN_FORMAT, buf);

    snprintf(buf, sizeof(buf), "Other fields:0x%x", (s->hex >> 13) & 0x7);
    c_put(di, s->dl_pre_start[13], s->dl_end[15], s->out_ann, ANN_FORMAT, buf);
}

static void eth_an_decode_next_page(struct srd_decoder_inst *di, struct eth_an_priv *s)
{
    static const char *next_page_ta_dict[] = {
        "1000BaseT M/S CFG EN", "1000BaseT M/S CFG Vale", "Port type",
        "1000BaseT-FD", "1000BaseT-HD", "10GBaseT-FD",
        "10GBaseT M/S CFG EN", "10GBaseT M/S CFG Vale",
        "Reserved", "Reserved", "Reserved"
    };

    static const char *next_page_ot_dict[] = {
        "T", "ACK2", "MP", "ACK", "NP"
    };

    int mp_flag = ((s->hex >> 13) & 0x1) == 0x1;
    const char *mp = mp_flag ? "Technology ability field:" : "Unformatted code field:";

    for (int i = 0; i < 16; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%x", (s->hex >> i) & 0x1);
        c_put(di, s->dl_pre_start[i], s->dl_end[i], s->out_ann, ANN_BIT, buf);

        if (i >= 11 && (i - 11) < 5) {
            c_put(di, s->dl_pre_start[i], s->dl_end[i], s->out_ann, ANN_BITD, next_page_ot_dict[i - 11]);
        }
        if (mp_flag && i <= 10) {
            c_put(di, s->dl_pre_start[i], s->dl_end[i], s->out_ann, ANN_BITD, next_page_ta_dict[i]);
        }
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "next page:0x%x", s->hex);
    c_put(di, s->dl_pre_start[0], s->dl_end[15], s->out_ann, ANN_DATA, buf);

    snprintf(buf, sizeof(buf), "%s0x%x", mp, s->hex & 0x7ff);
    c_put(di, s->dl_pre_start[0], s->dl_end[10], s->out_ann, ANN_FORMAT, buf);

    if (!mp_flag) {
        c_put(di, s->dl_pre_start[0], s->dl_end[10], s->out_ann, ANN_BITD, "Master-Slave seed value (MSB)");
    }

    snprintf(buf, sizeof(buf), "Other fields:0x%x", (s->hex >> 11) & 0x1f);
    c_put(di, s->dl_pre_start[11], s->dl_end[15], s->out_ann, ANN_FORMAT, buf);
}

static void eth_an_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct eth_an_priv)));
    }
    struct eth_an_priv *s = (struct eth_an_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct eth_an_priv));
    s->out_ann = -1;
}

static void eth_an_start(struct srd_decoder_inst *di)
{
    struct eth_an_priv *s = (struct eth_an_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "eth_an");
    s->samplerate = c_samplerate(di);
}

static void eth_an_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct eth_an_priv *s = (struct eth_an_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void eth_an_decode(struct srd_decoder_inst *di)
{
    struct eth_an_priv *s = (struct eth_an_priv *)c_decoder_get_private(di);
    if (s->samplerate == 0)
        return;

    while (1) {
        /* Wait for rising edge on channel 0 */
        int ret = c_wait(di, CW_R(0), CW_END);
        if (ret != SRD_OK)
            return;

        s->ss = di_samplenum(di);

        /* Wait for any edge on channel 0 */
        ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        s->es = di_samplenum(di);

        double length = (double)(s->es - s->ss) / (double)s->samplerate;
        if (length <= 1.0e-5 && length >= 1.0e-6) {
            /* NLP pulse detected */
            c_put(di, s->ss, s->es, s->out_ann, ANN_NLP, "NLP");

            uint64_t temp_length = s->ss - s->last_valid_ss;
            double len2 = (double)temp_length / (double)s->samplerate;
            s->last_valid_ss = s->ss;

            if (len2 <= 7.0e-5 && len2 >= 6.0e-5) {
                /* Logic 1: 60us ~ 70us interval */
                s->hex = s->hex | (0x1 << s->index);
                if (s->index < 16) {
                    s->dl_start[s->index] = s->ss;
                    s->dl_end[s->index] = s->es;
                    s->dl_pre_start[s->index] = s->pre_ss;
                    s->dl_pre_end[s->index] = s->pre_es;
                }
                s->index++;
                s->last_valid_ss = 0;
            } else if (len2 <= 1.4e-4 && len2 >= 1.2e-4) {
                /* Logic 0: 120us ~ 140us interval */
                if (s->index < 16) {
                    s->dl_start[s->index] = s->ss - (temp_length >> 1);
                    s->dl_end[s->index] = s->es - (temp_length >> 1);
                    s->dl_pre_start[s->index] = s->pre_ss;
                    s->dl_pre_end[s->index] = s->pre_es;
                }
                s->index++;
            }

            s->pre_ss = s->ss;
            s->pre_es = s->es;
        }

        if (s->index == 16) {
            eth_an_change_state(s);
            if (s->state == STATE_BASE_PAGE || s->state == STATE_BASE_PAGE_ACK) {
                eth_an_decode_base_page(di, s);
            } else if (s->state == STATE_NEXT_PAGE || s->state == STATE_NEXT_PAGE_ACK) {
                eth_an_decode_next_page(di, s);
            }
            /* updateOnceDecode */
            s->pre_hex = s->hex;
            s->index = 0;
            s->hex = 0;
        }
    }
}

static void eth_an_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder eth_an_c_decoder = {
    .id = "eth_an_c",
    .name = "ETH_AN(C)",
    .longname = "ETH Auto Negotiation (C)",
    .desc = "ETH Auto Negotiation protocol. (C implementation)",
    .license = "gplv2+",
    .channels = eth_an_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = eth_an_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = eth_an_ann_rows,
    .inputs = eth_an_inputs,
    .num_inputs = 1,
    .outputs = eth_an_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = eth_an_tags,
    .num_tags = 1,
    .reset = eth_an_reset,
    .start = eth_an_start,
    .decode = eth_an_decode,
    .destroy = eth_an_destroy,
    .state_size = 0,
    .metadata = eth_an_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &eth_an_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}