/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Benjamin Larsson <benjamin@southpole.se>
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

enum em4100_ann {
    ANN_BIT = 0,
    ANN_HEADER,
    ANN_VERSION_CUSTOMER,
    ANN_DATA,
    ANN_ROWPARITY_OK,
    ANN_ROWPARITY_ERR,
    ANN_COLPARITY_OK,
    ANN_COLPARITY_ERR,
    ANN_STOPBIT,
    ANN_TAG,
    NUM_ANN,
};

enum em4100_state {
    EM_HEADER,
    EM_PAYLOAD,
    EM_TRAILER,
};

typedef struct {
    uint64_t samplerate;
    int oldpin;
    uint64_t last_samplenum;
    uint64_t lastlast_samplenum;
    uint64_t last_edge;
    double bit_width;
    double halfbit_limit;
    int oldpp;
    uint64_t oldpl;
    uint64_t oldsamplenum;
    uint64_t last_bit_pos;
    uint64_t ss_first;
    int first_one;
    int state;
    int data;
    int data_bits;
    uint64_t ss_data;
    int data_parity;
    int payload_cnt;
    int data_col_parity[6];
    int col_parity[6];
    uint64_t col_parity_pos[6][2];
    int col_parity_pos_count;
    uint64_t tag;
    int all_row_parity_ok;
    int polarity;
    int out_ann;
    int initialized;
} em4100_state;

static struct srd_channel em4100_channels[] = {
    { "data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_em4100_chan_data" },
};

static struct srd_decoder_option em4100_options[] = {
    { "polarity", "dec_em4100_opt_polarity", "Polarity", NULL, NULL },
    { "datarate", "dec_em4100_opt_datarate", "Data rate", NULL, NULL },
    { "coilfreq", "dec_em4100_opt_coilfreq", "Coil frequency", NULL, NULL },
};

static const char* em4100_ann_labels[][3] = {
    { "", "Bit", "Bit" },
    { "", "Header", "Header" },
    { "", "Version/customer", "Version/customer" },
    { "", "Data", "Data" },
    { "", "Row parity OK", "Row parity OK" },
    { "", "Row parity error", "Row parity error" },
    { "", "Column parity OK", "Column parity OK" },
    { "", "Column parity error", "Column parity error" },
    { "", "Stop bit", "Stop bit" },
    { "", "Tag", "Tag" },
};

static const int em4100_row_bits_classes[] = { ANN_BIT, -1 };
static const int em4100_row_fields_classes[] = { ANN_HEADER, ANN_VERSION_CUSTOMER, ANN_DATA, ANN_ROWPARITY_OK, ANN_ROWPARITY_ERR, ANN_COLPARITY_OK, ANN_COLPARITY_ERR, ANN_STOPBIT, -1 };
static const int em4100_row_tags_classes[] = { ANN_TAG, -1 };

static const struct srd_c_ann_row em4100_ann_rows[] = {
    { "bits", "Bits", em4100_row_bits_classes, 1 },
    { "fields", "Fields", em4100_row_fields_classes, 8 },
    { "tags", "Tags", em4100_row_tags_classes, 1 },
};

static const char* em4100_inputs[] = { "logic" };
static const char* em4100_tags[] = { "IC", "RFID" };

static void em4100_putbit(struct srd_decoder_inst *di, em4100_state *s, int bit, uint64_t ss, uint64_t es)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", bit);
    c_put(di, ss, es, s->out_ann, ANN_BIT, buf);

    if (s->state == EM_HEADER) {
        if (bit == 1) {
            if (s->first_one > 0)
                s->first_one++;
            if (s->first_one == 9) {
                c_put(di, s->ss_first, es, s->out_ann, ANN_HEADER, "Header", "Head", "He", "H");
                s->first_one = 0;
                s->state = EM_PAYLOAD;
                return;
            }
            if (s->first_one == 0) {
                s->first_one = 1;
                s->ss_first = ss;
            }
        }
        if (bit == 0)
            s->first_one = 0;
        return;
    }

    if (s->state == EM_PAYLOAD) {
        s->payload_cnt++;
        if (s->data_bits == 0) {
            s->ss_data = ss;
            s->data = 0;
            s->data_parity = 0;
        }
        s->data_bits++;
        if (s->data_bits == 5) {
            char data_buf[64], short_buf[32];
            const char *label = (s->payload_cnt <= 10) ? "Version/customer" : "Data";
            int cls = (s->payload_cnt <= 10) ? ANN_VERSION_CUSTOMER : ANN_DATA;
            snprintf(data_buf, sizeof(data_buf), "%s: %X", label, s->data);
            snprintf(short_buf, sizeof(short_buf), "%X", s->data);
            c_put(di, s->ss_data, ss, s->out_ann, cls, data_buf, short_buf);

            const char *parity_str = (s->data_parity == bit) ? "OK" : "ERROR";
            int parity_cls = (s->data_parity == bit) ? ANN_ROWPARITY_OK : ANN_ROWPARITY_ERR;
            if (s->data_parity != bit)
                s->all_row_parity_ok = 0;
            char rp_buf[128], rp_short[32], rp_tiny[8], rp_t[4];
            snprintf(rp_buf, sizeof(rp_buf), "Row parity: %s", parity_str);
            snprintf(rp_short, sizeof(rp_short), "RP: %s", parity_str);
            snprintf(rp_tiny, sizeof(rp_tiny), "RP");
            snprintf(rp_t, sizeof(rp_t), "R");
            c_put(di, ss, es, s->out_ann, parity_cls, rp_buf, rp_short, rp_tiny, rp_t);

            s->tag = (s->tag << 4) | s->data;
            s->data_bits = 0;
            if (s->payload_cnt == 50) {
                s->state = EM_TRAILER;
                s->payload_cnt = 0;
            }
        }
        s->data_parity ^= bit;
        s->data_col_parity[s->data_bits] ^= bit;
        s->data = (s->data << 1) | bit;
        return;
    }

    if (s->state == EM_TRAILER) {
        s->payload_cnt++;
        if (s->data_bits == 0) {
            s->ss_data = ss;
            s->data = 0;
            s->data_parity = 0;
        }
        s->data_bits++;
        s->col_parity[s->data_bits] = bit;
        if (s->data_bits <= 5) {
            s->col_parity_pos[s->data_bits - 1][0] = ss;
            s->col_parity_pos[s->data_bits - 1][1] = es;
        }

        if (s->data_bits == 5) {
            c_put(di, ss, es, s->out_ann, ANN_STOPBIT, "Stop bit", "SB", "S");

            for (int i = 1; i <= 4; i++) {
                const char *cp_str = (s->data_col_parity[i] == s->col_parity[i]) ? "OK" : "ERROR";
                int cp_cls = (s->data_col_parity[i] == s->col_parity[i]) ? ANN_COLPARITY_OK : ANN_COLPARITY_ERR;
                char cp_buf[128], cp_short[32], cp_tiny[16], cp_t[4];
                snprintf(cp_buf, sizeof(cp_buf), "Column parity %d: %s", i, cp_str);
                snprintf(cp_short, sizeof(cp_short), "CP%d: %s", i, cp_str);
                snprintf(cp_tiny, sizeof(cp_tiny), "CP%d", i);
                snprintf(cp_t, sizeof(cp_t), "C");
                c_put(di, s->col_parity_pos[i - 1][0], s->col_parity_pos[i - 1][1],
                          s->out_ann, cp_cls, cp_buf, cp_short, cp_tiny, cp_t);
            }

            int all_col_parity_ok = 1;
            for (int i = 1; i <= 4; i++) {
                if (s->data_col_parity[i] != s->col_parity[i]) {
                    all_col_parity_ok = 0;
                    break;
                }
            }
            if (all_col_parity_ok && s->all_row_parity_ok) {
                char tag_buf[64], tag_short[16], tag_tiny[8];
                snprintf(tag_buf, sizeof(tag_buf), "Tag: %010llX", (unsigned long long)s->tag);
                snprintf(tag_short, sizeof(tag_short), "Tag");
                snprintf(tag_tiny, sizeof(tag_tiny), "T");
                c_put(di, s->ss_first, es, s->out_ann, ANN_TAG, tag_buf, tag_short, tag_tiny);
            }

            s->tag = 0;
            s->data_bits = 0;

            if (s->payload_cnt == 5) {
                s->state = EM_HEADER;
                s->payload_cnt = 0;
                memset(s->data_col_parity, 0, sizeof(s->data_col_parity));
                memset(s->col_parity, 0, sizeof(s->col_parity));
                s->col_parity_pos_count = 0;
                s->all_row_parity_ok = 1;
            }
        }
    }
}

static void em4100_manchester_decode(struct srd_decoder_inst *di, em4100_state *s,
                                      uint64_t pl, int pp, int pin, uint64_t samplenum)
{
    (void)samplenum;
    (void)pp;
    (void)pin;
    int bit = s->oldpin ^ s->polarity;
    if (pl > s->halfbit_limit) {
        uint64_t es = (uint64_t)(samplenum - pl / 2);
        uint64_t ss;
        if (s->oldpl > s->halfbit_limit)
            ss = (uint64_t)(s->oldsamplenum - s->oldpl / 2);
        else
            ss = (uint64_t)(s->oldsamplenum - s->oldpl);
        em4100_putbit(di, s, bit, ss, es);
        s->last_bit_pos = (uint64_t)(samplenum - pl / 2);
    } else {
        uint64_t es = samplenum;
        if (s->oldpl > s->halfbit_limit) {
            uint64_t ss = (uint64_t)(s->oldsamplenum - s->oldpl / 2);
            em4100_putbit(di, s, bit, ss, es);
            s->last_bit_pos = samplenum;
        } else {
            if (s->last_bit_pos <= s->oldsamplenum - s->oldpl) {
                uint64_t ss = (uint64_t)(s->oldsamplenum - s->oldpl);
                em4100_putbit(di, s, bit, ss, es);
                s->last_bit_pos = samplenum;
            }
        }
    }
}

static void em4100_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(em4100_state)));
    }
    em4100_state *s = (em4100_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(em4100_state));
    s->out_ann = -1;
    s->state = EM_HEADER;
    s->all_row_parity_ok = 1;
    s->initialized = 0;
}

static void em4100_start(struct srd_decoder_inst *di)
{
    em4100_state *s = (em4100_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "em4100");
}

static void em4100_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    em4100_state *s = (em4100_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
    if (s->samplerate == 0) return;

    int64_t datarate = c_opt_int(di, "datarate", 64);
    int64_t coilfreq = c_opt_int(di, "coilfreq", 125000);
    const char *polarity_str = c_opt_str(di, "polarity", "active-high");

    s->bit_width = ((double)s->samplerate / (double)coilfreq) * (double)datarate;
    s->halfbit_limit = s->bit_width / 2.0 + s->bit_width / 4.0;
    s->polarity = (strcmp(polarity_str, "active-low") == 0) ? 0 : 1;
}

static void em4100_decode(struct srd_decoder_inst *di)
{
    em4100_state *s = (em4100_state *)c_decoder_get_private(di);

    if (s->samplerate == 0) return;

    /* Initialize internal state from the very first sample */
    if (!s->initialized) {
        uint64_t cur_sample = 0;
        if (c_wait(di, CW_END) != SRD_OK)
            return;
        s->oldpin = c_pin(di, 0);
        s->last_samplenum = cur_sample;
        s->lastlast_samplenum = cur_sample;
        s->last_edge = cur_sample;
        s->oldpl = 0;
        s->oldpp = 0;
        s->oldsamplenum = 0;
        s->last_bit_pos = 0;
        s->initialized = 1;
    }

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int pin = c_pin(di, 0);
        uint64_t pl = di_samplenum(di) - s->oldsamplenum;
        int pp = pin;
        em4100_manchester_decode(di, s, pl, pp, pin, di_samplenum(di));
        s->oldpl = pl;
        s->oldpp = pp;
        s->oldsamplenum = di_samplenum(di);
        s->oldpin = pin;
    }
}

static void em4100_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder em4100_c_decoder = {
    .id = "em4100_c",
    .name = "EM4100(C)",
    .longname = "RFID EM4100 (C)",
    .desc = "EM4100 100-150kHz RFID protocol.",
    .license = "gplv2+",
    .channels = em4100_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = em4100_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = em4100_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = em4100_ann_rows,
    .inputs = em4100_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = em4100_tags,
    .num_tags = 2,
    .reset = em4100_reset,
    .start = em4100_start,
    .decode = em4100_decode,
    .destroy = em4100_destroy,
    .state_size = 0,
    .metadata = em4100_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &em4100_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}