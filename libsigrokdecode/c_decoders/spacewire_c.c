/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2022 Theo Hussey <husseytg@gmail.com>
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
    STATE_IDLE,
    STATE_SYNC,
};

enum {
    ANN_DATA = 0,
    ANN_PARITY,
    ANN_DCF,
    ANN_CTRL_CHAR,
    ANN_DATA_CHAR,
    ANN_CODE,
    ANN_TIME,
    ANN_WARNING,
    NUM_ANN,
};

#define CHAR_LEN_CONTROL 3
#define CHAR_LEN_DATA    9
#define PACKET_MASK_CONTROL 0b111

#define FCT 0x1
#define EOP 0x5
#define EEP 0x3
#define ESC 0x7

#define CH_DATA   0
#define CH_STROBE 1

#define MAX_SAMPLENUMS 15

typedef struct {
    int state;
    int index;
    int char_len;
    int last_len;
    int data_val;
    int last_data_val;

    uint64_t last_samplenums[MAX_SAMPLENUMS];
    int num_samplenums;

    int out_ann;
} spacewire_state;

static const int reverse3[] = {0, 4, 2, 6, 1, 5, 3, 7};

static int reverse_bits(int val, int n)
{
    int result = 0;
    for (int i = 0; i < n; i++)
        result |= ((val >> i) & 1) << (n - 1 - i);
    return result;
}

static void spacewire_put(spacewire_state *s, struct srd_decoder_inst *di,
                          uint64_t ss, uint64_t es, int ann_class, const char **txts)
{
    struct srd_c_annotation ann;
    ann.ann_class = ann_class;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, ss, es, s->out_ann, &ann);
}

static void sn_insert(spacewire_state *s, uint64_t samplenum)
{
    (void)samplenum;
    if (s->num_samplenums < MAX_SAMPLENUMS) {
        memmove(&s->last_samplenums[1], &s->last_samplenums[0],
                (MAX_SAMPLENUMS - 1) * sizeof(uint64_t));
        s->last_samplenums[0] = samplenum;
        if (s->num_samplenums < MAX_SAMPLENUMS)
            s->num_samplenums++;
    } else {
        memmove(&s->last_samplenums[1], &s->last_samplenums[0],
                (MAX_SAMPLENUMS - 1) * sizeof(uint64_t));
        s->last_samplenums[0] = samplenum;
    }
}

static uint64_t sn_get(spacewire_state *s, int idx)
{
    if (idx < 0 || idx >= s->num_samplenums)
        return 0;
    return s->last_samplenums[idx];
}

static struct srd_channel spacewire_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_spacewire_chan_data"},
    {"strobe", "Strobe", "Strobe line", 1, SRD_CHANNEL_SCLK, "dec_spacewire_chan_strobe"},
};

static const char *spacewire_ann_labels[][3] = {
    {"", "D", "Data"},
    {"", "P", "Parity"},
    {"", "DCF", "Data Control Flag"},
    {"", "ctrl-char", "Control Character"},
    {"", "data-char", "Data Character"},
    {"", "code", "Control Code"},
    {"", "time", "Control Code"},
    {"", "warning", "Warning"},
};

static const int spacewire_row_bits_classes[] = {ANN_DATA, ANN_PARITY, ANN_DCF};
static const int spacewire_row_chars_classes[] = {ANN_CTRL_CHAR, ANN_DATA_CHAR};
static const int spacewire_row_codes_classes[] = {ANN_CODE, ANN_TIME};
static const int spacewire_row_warnings_classes[] = {ANN_WARNING};
static const struct srd_c_ann_row spacewire_ann_rows[] = {
    {"bits", "Bits", spacewire_row_bits_classes, 3},
    {"characters", "Characters", spacewire_row_chars_classes, 2},
    {"codes", "Control Codes", spacewire_row_codes_classes, 2},
    {"warnings", "Warnings", spacewire_row_warnings_classes, 1},
};

static const char *spacewire_inputs[] = {"logic"};
static const char *spacewire_outputs[] = {"spacewire"};
static const char *spacewire_tags[] = {"Aerospace"};

static void spacewire_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(spacewire_state)));
    spacewire_state *s = (spacewire_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(spacewire_state));
    s->state = STATE_IDLE;
    s->char_len = CHAR_LEN_CONTROL;
    s->last_len = 0;
    s->data_val = 0;
    s->last_data_val = 0;
    s->out_ann = 0;
}

static void spacewire_start(struct srd_decoder_inst *di)
{
    spacewire_state *s = (spacewire_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "spacewire");
}

static void spacewire_decode(struct srd_decoder_inst *di)
{
    spacewire_state *s = (spacewire_state *)c_decoder_get_private(di);
    while (1) {
        int ret = c_wait(di, CW_E(CH_DATA), CW_OR, CW_E(CH_STROBE), CW_END);
        if (ret != SRD_OK)
            return;

        sn_insert(s, di_samplenum(di));

        uint8_t data = c_pin(di, CH_DATA);

        /* Match Python: check data_val BEFORE shifting, then shift at end */
        if (s->state == STATE_IDLE) {
            if ((s->data_val & 0b1111111) == 0b1110100) {
                /* NULL code detected */
                /* Parity bit (bit 7) */
                char p_str[4];
                snprintf(p_str, sizeof(p_str), "%d", (s->data_val >> 7) & 1);
                const char *p_txts[] = {p_str, NULL};
                spacewire_put(s, di, sn_get(s, 7 + 1), sn_get(s, 7), ANN_PARITY, p_txts);

                /* Data bits 0-6 */
                for (int i = 6; i >= 0; i--) {
                    char d_str[4];
                    snprintf(d_str, sizeof(d_str), "%d", (s->data_val >> i) & 1);
                    const char *d_txts[] = {d_str, NULL};
                    spacewire_put(s, di, sn_get(s, i + 1), sn_get(s, i), ANN_DATA, d_txts);
                }

                /* NULL control code annotation */
                const char *null_txts[] = {"NULL", NULL};
                spacewire_put(s, di, sn_get(s, 7 + 1), sn_get(s, 0), ANN_CODE, null_txts);

                s->state = STATE_SYNC;
                s->last_len = CHAR_LEN_CONTROL;
                s->index = 0;
            }
        } else if (s->state == STATE_SYNC) {
            if (s->index == 1) {
                /* DCF bit */
                if (s->data_val & 0b1)
                    s->char_len = CHAR_LEN_CONTROL;
                else
                    s->char_len = CHAR_LEN_DATA;

                char dcf_str[4];
                snprintf(dcf_str, sizeof(dcf_str), "%d", s->data_val & 1);
                const char *dcf_txts[] = {dcf_str, NULL};
                spacewire_put(s, di, sn_get(s, 1), sn_get(s, 0), ANN_DCF, dcf_txts);

                /* Parity check */
                int parity = 0;
                for (int i = 0; i < s->last_len - 1; i++)
                    parity ^= (s->last_data_val >> i) & 1;
                parity ^= s->data_val & 1;
                parity ^= 1; /* odd parity */

                char par_str[4];
                snprintf(par_str, sizeof(par_str), "%d", (s->data_val >> 1) & 1);
                const char *par_txts[] = {par_str, NULL};
                spacewire_put(s, di, sn_get(s, 2), sn_get(s, 1), ANN_PARITY, par_txts);

                if (parity != ((s->data_val >> 1) & 1)) {
                    const char *warn_txts[] = {"PE", "Parity Error", NULL};
                    spacewire_put(s, di, sn_get(s, 2), sn_get(s, 1), ANN_WARNING, warn_txts);
                }

                s->index++;
            } else if (s->index == s->char_len) {
                /* Character complete */
                /* Display data bit values */
                for (int i = s->char_len - 2; i >= 0; i--) {
                    char d_str[4];
                    snprintf(d_str, sizeof(d_str), "%d", (s->data_val >> i) & 1);
                    const char *d_txts[] = {d_str, NULL};
                    spacewire_put(s, di, sn_get(s, i + 1), sn_get(s, i), ANN_DATA, d_txts);
                }

                if (s->char_len == CHAR_LEN_CONTROL) {
                    /* Control character */
                    int control_char = reverse3[s->data_val & PACKET_MASK_CONTROL];

                    if (control_char == FCT) {
                        const char *txts[] = {"FCT", NULL};
                        spacewire_put(s, di, sn_get(s, s->char_len + 1), sn_get(s, 0), ANN_CTRL_CHAR, txts);
                    } else if (control_char == ESC) {
                        const char *txts[] = {"ESC", NULL};
                        spacewire_put(s, di, sn_get(s, s->char_len + 1), sn_get(s, 0), ANN_CTRL_CHAR, txts);
                    } else if (control_char == EEP) {
                        const char *txts[] = {"EEP", NULL};
                        spacewire_put(s, di, sn_get(s, s->char_len + 1), sn_get(s, 0), ANN_CTRL_CHAR, txts);
                    } else if (control_char == EOP) {
                        const char *txts[] = {"EOP", NULL};
                        spacewire_put(s, di, sn_get(s, s->char_len + 1), sn_get(s, 0), ANN_CTRL_CHAR, txts);
                    } else {
                        char cc_str[32];
                        snprintf(cc_str, sizeof(cc_str), "%d %d", control_char, s->data_val & PACKET_MASK_CONTROL);
                        const char *txts[] = {cc_str, NULL};
                        spacewire_put(s, di, sn_get(s, s->char_len + 1), sn_get(s, 0), ANN_CTRL_CHAR, txts);
                    }

                    /* Detect NULL control code: last was ESC, current is FCT */
                    if (s->last_len == CHAR_LEN_CONTROL) {
                        int last_cc = reverse3[s->last_data_val & PACKET_MASK_CONTROL];
                        if (last_cc == ESC && control_char == FCT) {
                            const char *null_txts[] = {"NULL", NULL};
                            spacewire_put(s, di, sn_get(s, CHAR_LEN_CONTROL * 2 + 2), sn_get(s, 0), ANN_CODE, null_txts);
                        }
                    }
                } else if (s->char_len == CHAR_LEN_DATA) {
                    /* Data character */
                    int data_val_reversed = reverse_bits(s->data_val & 0xff, 8);
                    char dc_str[16];
                    snprintf(dc_str, sizeof(dc_str), "0x%02x", data_val_reversed);
                    const char *txts[] = {dc_str, NULL};
                    spacewire_put(s, di, sn_get(s, s->char_len + 1), sn_get(s, 0), ANN_DATA_CHAR, txts);

                    /* Detect Time code: last was ESC */
                    if (s->last_len == CHAR_LEN_CONTROL) {
                        int last_cc = reverse3[s->last_data_val & PACKET_MASK_CONTROL];
                        if (last_cc == ESC) {
                            const char *time_txts[] = {"Time", NULL};
                            spacewire_put(s, di, sn_get(s, CHAR_LEN_CONTROL + s->char_len + 2), sn_get(s, 0), ANN_TIME, time_txts);
                        }
                    }
                }

                s->last_len = s->char_len;
                s->last_data_val = s->data_val;
                s->index = 0;
            } else {
                s->index++;
            }
        }

        /* Shift in the data at the end — match Python's ordering */
        s->data_val = (s->data_val << 1) | data;
    }
}

static void spacewire_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder spacewire_c_decoder = {
    .id = "spacewire_c",
    .name = "Spacewire(C)",
    .longname = "Spacewire (C)",
    .desc = "High speed data transfer protocol used for communication between spacecraft subsystems (C implementation)",
    .license = "gplv2+",
    .channels = spacewire_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = spacewire_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = spacewire_ann_rows,
    .reset = spacewire_reset,
    .start = spacewire_start,
    .decode = spacewire_decode,
    .destroy = spacewire_destroy,
    .state_size = 0,
    .inputs = spacewire_inputs,
    .num_inputs = 1,
    .outputs = spacewire_outputs,
    .num_outputs = 1,
    .tags = spacewire_tags,
    .num_tags = 1,
    .binary = NULL,
    .num_binary = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &spacewire_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}