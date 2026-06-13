/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2018 Max Weller
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CMDBIT = 0,
    ANN_DATABIT,
    ANN_CMD,
    ANN_DATA,
    ANN_WARNING,
    NUM_ANN,
};

#define MAX_CMDBITS 24
#define MAX_DATABITS 8

typedef struct {
    /* cmdbits: stored as [value, ss, es] triples, LSB-first (new bit at index 0) */
    int cmdbits_val[MAX_CMDBITS];
    uint64_t cmdbits_ss[MAX_CMDBITS];
    uint64_t cmdbits_es[MAX_CMDBITS];
    int num_cmdbits;

    int databits_val[MAX_DATABITS];
    int num_databits;
    uint64_t datastart;

    int out_ann;
    uint64_t samplerate;
} sda2506_state;

#define CH_CLK 0
#define CH_D   1
#define CH_CE  2

static struct srd_channel sda2506_channels[] = {
    {"clk", "CLK", "Clock", 0, SRD_CHANNEL_SCLK, "dec_sda2506_chan_clk"},
    {"d",   "DATA", "Data",  1, SRD_CHANNEL_SDATA, "dec_sda2506_chan_d"},
    {"ce",  "CE#",  "Chip-enable", 2, SRD_CHANNEL_COMMON, "dec_sda2506_chan_ce"},
};

static const char *sda2506_ann_labels[][3] = {
    {"", "cmdbit", "Command bit"},
    {"", "databit", "Data bit"},
    {"", "cmd", "Command"},
    {"", "data", "Data byte"},
    {"", "warnings", "Human-readable warnings"},
};

static const int sda2506_row_bits_classes[] = {ANN_CMDBIT, ANN_DATABIT, -1};
static const int sda2506_row_commands_classes[] = {ANN_CMD, -1};
static const int sda2506_row_data_classes[] = {ANN_DATA, -1};
static const int sda2506_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row sda2506_ann_rows[] = {
    {"bits", "Bits", sda2506_row_bits_classes, 2},
    {"commands", "Commands", sda2506_row_commands_classes, 1},
    {"data", "Data", sda2506_row_data_classes, 1},
    {"warnings", "Warnings", sda2506_row_warnings_classes, 1},
};

static const char *sda2506_inputs[] = {"logic"};
static const char *sda2506_tags[] = {"IC", "Memory"};

static void sda2506_reset_state(sda2506_state *s)
{
    s->num_cmdbits = 0;
    s->num_databits = 0;
    s->datastart = 0;
}

static void sda2506_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(sda2506_state)));
    sda2506_state *s = (sda2506_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(sda2506_state));
    s->out_ann = -1;
}

static void sda2506_start(struct srd_decoder_inst *di)
{
    sda2506_state *s = (sda2506_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sda2506");
}

static void sda2506_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    sda2506_state *s = (sda2506_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void sda2506_putdata(struct srd_decoder_inst *di, sda2506_state *s,
                            uint64_t ss, uint64_t es)
{
    int value = 0;
    for (int i = 0; i < 8; i++)
        value = (value << 1) | s->databits_val[i];
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", value);
    c_put(di, ss, es, s->out_ann, ANN_DATA, buf);
}

static int sda2506_decode_bits(sda2506_state *s, int offset, int width,
                               uint64_t *out_ss, uint64_t *out_es)
{
    int out = 0;
    for (int i = 0; i < width; i++)
        out = (out << 1) | s->cmdbits_val[offset + i];
    if (out_ss) *out_ss = s->cmdbits_ss[offset + width - 1];
    if (out_es) *out_es = s->cmdbits_es[offset];
    return out;
}

static void sda2506_decode_field(struct srd_decoder_inst *di, sda2506_state *s,
                                 const char *name, int offset, int width)
{
    uint64_t ss, es;
    int val = sda2506_decode_bits(s, offset, width, &ss, &es);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %02X", name, val);
    c_put(di, ss, es, s->out_ann, ANN_DATA, buf);
}

static void sda2506_insert_cmdbit(sda2506_state *s, int val,
                                  uint64_t ss, uint64_t es)
{
    /* Shift existing entries right, insert new at index 0 (LSB-first) */
    for (int i = MAX_CMDBITS - 1; i > 0; i--) {
        s->cmdbits_val[i] = s->cmdbits_val[i - 1];
        s->cmdbits_ss[i] = s->cmdbits_ss[i - 1];
        s->cmdbits_es[i] = s->cmdbits_es[i - 1];
    }
    s->cmdbits_val[0] = val;
    s->cmdbits_ss[0] = ss;
    s->cmdbits_es[0] = es;
    if (s->num_cmdbits < MAX_CMDBITS)
        s->num_cmdbits++;
}

static void sda2506_decode(struct srd_decoder_inst *di)
{
    sda2506_state *s = (sda2506_state *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    while (1) {
        /* Wait for CLK edge or CE edge */
        int ret = c_wait(di, CW_E(CH_CLK), CW_OR, CW_E(CH_CE), CW_END);
        if (ret != SRD_OK)
            return;

        int clk = c_pin(di, CH_CLK);
        int d   = c_pin(di, CH_D);
        int ce  = c_pin(di, CH_CE);

        int clk_matched = (di_matched(di) & (1ULL << 0)) != 0;
        int ce_matched  = (di_matched(di) & (1ULL << 1)) != 0;

        if (clk_matched && ce == 1 && clk == 1) {
            /* Rising CLK edge and command mode: sample DATA */
            uint64_t bitstart = di_samplenum(di);

            /* Wait for CLK falling edge */
            ret = c_wait(di, CW_F(CH_CLK), CW_END);
            if (ret != SRD_OK)
                return;

            sda2506_insert_cmdbit(s, d, bitstart, di_samplenum(di));
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", d);
            c_put(di, bitstart, di_samplenum(di), s->out_ann, ANN_CMDBIT, bit_str);

        } else if (clk_matched && ce == 0 && clk == 0) {
            /* Falling CLK edge and data mode */
            uint64_t bitstart = di_samplenum(di);

            /* Wait ~25us for data ready, or CLK rising edge, or CE edge */
            uint64_t skip_count = (uint64_t)(2.5 * (1e6 / (double)s->samplerate));
            ret = c_wait(di, CW_SKIP(skip_count), CW_OR, CW_R(CH_CLK), CW_OR, CW_E(CH_CE), CW_END);
            if (ret != SRD_OK)
                return;

            /* If CE edge di_matched(di) (and not skip/clk), wait for CLK rise or CE edge */
            if ((di_matched(di) & (1ULL << 2)) && !(di_matched(di) & 0b011)) {
                ret = c_wait(di, CW_R(CH_CLK), CW_OR, CW_E(CH_CE), CW_END);
                if (ret != SRD_OK)
                    return;
            }

            d = c_pin(di, CH_D);

            if (s->num_databits == 0)
                s->datastart = bitstart;

            /* Insert at beginning (LSB-first) */
            for (int i = MAX_DATABITS - 1; i > 0; i--)
                s->databits_val[i] = s->databits_val[i - 1];
            s->databits_val[0] = d;
            s->num_databits++;

            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", d);
            c_put(di, bitstart, di_samplenum(di), s->out_ann, ANN_DATABIT, bit_str);

            if (s->num_databits == 8) {
                sda2506_putdata(di, s, s->datastart, di_samplenum(di));
                s->num_databits = 0;
            }

        } else if (ce_matched && ce == 0) {
            /* CE falling edge: parse command */
            if (s->num_cmdbits < 8) {
                sda2506_reset_state(s);
                continue;
            }

            sda2506_decode_field(di, s, "addr", 1, 7);
            sda2506_decode_field(di, s, "CB", 0, 1);

            if (s->cmdbits_val[0] == 0) {
                /* Read command */
                if (s->num_cmdbits >= 14) {
                    sda2506_decode_field(di, s, "read", 1, 7);
                    uint64_t ss;
                    sda2506_decode_bits(s, 7, 1, &ss, NULL);
                    c_put(di, ss, di_samplenum(di), s->out_ann, ANN_CMD, "read");
                }
            } else if (d == 0) {
                /* Write command */
                if (s->num_cmdbits >= 16) {
                    sda2506_decode_field(di, s, "data", 8, 8);
                    uint64_t ss1, es1, ss2, es2;
                    int addr = sda2506_decode_bits(s, 1, 7, &ss1, &es1);
                    int data = sda2506_decode_bits(s, 8, 8, &ss2, &es2);
                    (void)ss2; (void)es2;

                    uint64_t cmdstart = di_samplenum(di);
                    /* Wait for CE rising edge */
                    ret = c_wait(di, CW_R(CH_CE), CW_END);
                    if (ret != SRD_OK)
                        return;

                    char buf[64];
                    snprintf(buf, sizeof(buf), "Write to %02X: %02X", addr, data);
                    c_put(di, cmdstart, di_samplenum(di), s->out_ann, ANN_CMD, buf);
                }
            } else {
                /* Erase command */
                if (s->num_cmdbits >= 8) {
                    uint64_t ss, es;
                    int val = sda2506_decode_bits(s, 1, 7, &ss, &es);
                    (void)ss; (void)es;

                    uint64_t cmdstart = di_samplenum(di);
                    /* Wait for CE rising edge */
                    ret = c_wait(di, CW_R(CH_CE), CW_END);
                    if (ret != SRD_OK)
                        return;

                    char buf[64];
                    snprintf(buf, sizeof(buf), "Erase: %02X", val);
                    c_put(di, cmdstart, di_samplenum(di), s->out_ann, ANN_CMD, buf);
                }
            }
            s->num_databits = 0;
        }
    }
}

static void sda2506_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sda2506_c_decoder = {
    .id = "sda2506_c",
    .name = "SDA2506(C)",
    .longname = "Siemens SDA 2506-5 (C)",
    .desc = "Serial nonvolatile 1-Kbit EEPROM. (C implementation)",
    .license = "gplv2+",
    .channels = sda2506_channels,
    .num_channels = 3,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = sda2506_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = sda2506_ann_rows,
    .inputs = sda2506_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sda2506_tags,
    .num_tags = 2,
    .reset = sda2506_reset,
    .start = sda2506_start,
    .decode = sda2506_decode,
    .destroy = sda2506_destroy,
    .state_size = 0,
    .metadata = sda2506_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &sda2506_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}