/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2023 perigoso
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RVSWD_MAX_BITS 128

#define CH_CLK 0
#define CH_DIO 1

enum rvswd_ann {
    ANN_START = 0,
    ANN_STOP,
    ANN_ADDRESS_HOST,
    ANN_ADDRESS_TARGET,
    ANN_DATA_HOST,
    ANN_DATA_TARGET,
    ANN_PARITY_HOST,
    ANN_PARITY_TARGET,
    ANN_OPERATION,
    ANN_STATUS,
    ANN_BIT,
    NUM_ANN,
};

struct rvswd_bit {
    int val;
    uint64_t start;
    uint64_t end;
};

struct rvswd_priv {
    struct rvswd_bit bits[RVSWD_MAX_BITS];
    int bits_len;
    int curr_bit_val;
    uint64_t curr_bit_start;
    int in_packet;
    int out_ann;
    int out_python;
};

static struct srd_channel rvswd_channels[] = {
    { "clk", "CLK", "Serial clock line", 0, SRD_CHANNEL_SCLK, NULL },
    { "dio", "DIO", "Serial data line", 1, SRD_CHANNEL_SDATA, NULL },
};

static const char *rvswd_ann_labels[][3] = {
    { "", "Start condition", "START" },
    { "", "Stop condition", "STOP" },
    { "", "Address host", "ADDR HOST" },
    { "", "Address target", "ADDR TARGET" },
    { "", "Data host", "DATA HOST" },
    { "", "Data target", "DATA TARGET" },
    { "", "Parity host", "PARITY HOST" },
    { "", "Parity target", "PARITY TARGET" },
    { "", "Operation", "OPERATION" },
    { "", "Status", "STATUS" },
    { "", "Bit", "BIT" },
};

static const int rvswd_row_addr_data_classes[] = {
    ANN_START, ANN_STOP, ANN_ADDRESS_HOST, ANN_ADDRESS_TARGET,
    ANN_DATA_HOST, ANN_DATA_TARGET, ANN_PARITY_HOST, ANN_PARITY_TARGET,
    ANN_OPERATION, ANN_STATUS, -1
};
static const int rvswd_row_bits_classes[] = { ANN_BIT, -1 };

static const struct srd_c_ann_row rvswd_ann_rows[] = {
    { "addr-data", "Address/data", rvswd_row_addr_data_classes, 10 },
    { "bits", "Bits", rvswd_row_bits_classes, 1 },
};

static const char *rvswd_inputs[] = { "logic" };
static const char *rvswd_tags[] = { "Debug/trace" };

static void put_annotation_bits(struct srd_decoder_inst *di, struct rvswd_priv *s,
    int start_idx, int count, int ann_id)
{
    if (count <= 0 || start_idx + count > s->bits_len)
        return;

    uint64_t ss = s->bits[start_idx].start;
    uint64_t es = s->bits[start_idx + count - 1].end;

    uint64_t data = 0;
    for (int i = start_idx; i < start_idx + count; i++)
        data = (data << 1) | s->bits[i].val;

    char text1[64], text2[64], text3[64];
    switch (ann_id) {
    case ANN_ADDRESS_HOST:
        snprintf(text1, sizeof(text1), "ADDR HOST 0x%02llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "AH 0x%02llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%02llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_ADDRESS_TARGET:
        snprintf(text1, sizeof(text1), "ADDR TARGET 0x%02llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "AT 0x%02llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%02llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_DATA_HOST:
        snprintf(text1, sizeof(text1), "DATA HOST 0x%08llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "DH 0x%08llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%08llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_DATA_TARGET:
        snprintf(text1, sizeof(text1), "DATA TARGET 0x%08llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "DT 0x%08llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%08llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_PARITY_HOST:
        snprintf(text1, sizeof(text1), "PARITY HOST 0x%01llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "PH 0x%01llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%01llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_PARITY_TARGET:
        snprintf(text1, sizeof(text1), "PARITY TARGET 0x%01llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "PT 0x%01llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%01llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_OPERATION:
        snprintf(text1, sizeof(text1), "OPERATION 0x%01llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "OP 0x%01llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%01llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    case ANN_STATUS:
        snprintf(text1, sizeof(text1), "STATUS 0x%01llx", (unsigned long long)data);
        snprintf(text2, sizeof(text2), "ST 0x%01llx", (unsigned long long)data);
        snprintf(text3, sizeof(text3), "%01llx", (unsigned long long)data);
        c_put_v(di, ss, es, s->out_ann, ann_id, data, text1, text2, text3);
        break;
    default:
        break;
    }
}

static void process_short_packet(struct srd_decoder_inst *di, struct rvswd_priv *s)
{
    for (int i = 0; i < s->bits_len && i < 52; i++) {
        char text1[32], text2[8];
        snprintf(text1, sizeof(text1), "BIT %d: %d", i, s->bits[i].val);
        snprintf(text2, sizeof(text2), "%d", s->bits[i].val);
        c_put(di, s->bits[i].start, s->bits[i].end, s->out_ann, ANN_BIT, text1, text2);
    }

    put_annotation_bits(di, s, 0, 7, ANN_ADDRESS_HOST);
    put_annotation_bits(di, s, 7, 1, ANN_OPERATION);
    put_annotation_bits(di, s, 8, 1, ANN_PARITY_HOST);
    put_annotation_bits(di, s, 14, 32, ANN_DATA_TARGET);
    put_annotation_bits(di, s, 46, 1, ANN_PARITY_TARGET);
}

static void process_long_packet(struct srd_decoder_inst *di, struct rvswd_priv *s)
{
    for (int i = 0; i < s->bits_len && i < 84; i++) {
        char text1[32], text2[8];
        snprintf(text1, sizeof(text1), "BIT %d: %d", i, s->bits[i].val);
        snprintf(text2, sizeof(text2), "%d", s->bits[i].val);
        c_put(di, s->bits[i].start, s->bits[i].end, s->out_ann, ANN_BIT, text1, text2);
    }

    put_annotation_bits(di, s, 0, 7, ANN_ADDRESS_HOST);
    put_annotation_bits(di, s, 7, 32, ANN_DATA_HOST);
    put_annotation_bits(di, s, 39, 2, ANN_OPERATION);
    put_annotation_bits(di, s, 41, 1, ANN_PARITY_HOST);
    put_annotation_bits(di, s, 42, 7, ANN_ADDRESS_TARGET);
    put_annotation_bits(di, s, 49, 32, ANN_DATA_TARGET);
    put_annotation_bits(di, s, 81, 2, ANN_STATUS);
    put_annotation_bits(di, s, 83, 1, ANN_PARITY_TARGET);
}

static void process_packet(struct srd_decoder_inst *di, struct rvswd_priv *s)
{
    if (s->bits_len == 52)
        process_short_packet(di, s);
    else if (s->bits_len == 84)
        process_long_packet(di, s);
}

static void rvswd_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct rvswd_priv)));
    }
    struct rvswd_priv *s = (struct rvswd_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct rvswd_priv));
    s->curr_bit_val = -1;
    s->curr_bit_start = (uint64_t)-1;
    s->out_ann = -1;
    s->out_python = -1;
}

static void rvswd_start(struct srd_decoder_inst *di)
{
    struct rvswd_priv *s = (struct rvswd_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rvswd");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "rvswd");
}

static void rvswd_decode(struct srd_decoder_inst *di)
{
    struct rvswd_priv *s = (struct rvswd_priv *)c_decoder_get_private(di);
    while (1) {
        if (!s->in_packet) {
            int ret = c_wait(di, CW_H(CH_CLK), CW_F(CH_DIO), CW_END);
            if (ret != SRD_OK)
                return;

            s->bits_len = 0;
            s->curr_bit_val = -1;
            s->curr_bit_start = (uint64_t)-1;
            c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_START, "START", "S");
            s->in_packet = 1;
        } else {
            int ret = c_wait(di, CW_R(CH_CLK), CW_OR, CW_F(CH_CLK), CW_OR, CW_H(CH_CLK), CW_R(CH_DIO), CW_END);
            if (ret != SRD_OK)
                return;

            int dio = c_pin(di, CH_DIO);

            if (di_matched(di) == (1ULL << 2)) {
                /* STOP condition - ONLY condition 2 di_matched(di) */
                process_packet(di, s);
                c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_STOP, "STOP", "P");
                s->in_packet = 0;
            } else if (di_matched(di) & (1ULL << 0)) {
                /* CLK rising edge: sample DIO */
                if (s->curr_bit_start == (uint64_t)-1)
                    s->curr_bit_start = di_samplenum(di);
                s->curr_bit_val = dio;
            } else if (di_matched(di) & (1ULL << 1)) {
                /* CLK falling edge: terminate current bit */
                if (s->curr_bit_val != -1 && s->curr_bit_start != (uint64_t)-1 &&
                    s->bits_len < RVSWD_MAX_BITS) {
                    s->bits[s->bits_len].val = s->curr_bit_val;
                    s->bits[s->bits_len].start = s->curr_bit_start;
                    s->bits[s->bits_len].end = di_samplenum(di);
                    s->bits_len++;
                }
                s->curr_bit_val = -1;
                s->curr_bit_start = di_samplenum(di);
            }
        }
    }
}

static void rvswd_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder rvswd_c_decoder = {
    .id = "rvswd_c",
    .name = "RVSWD(C)",
    .longname = "RISC-V Serial Wire Debug (WCH) (C)",
    .desc = "WCH RISC-V Serial Wire Debug protocol.",
    .license = "gplv2+",
    .channels = rvswd_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = rvswd_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = rvswd_ann_rows,
    .inputs = rvswd_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rvswd_tags,
    .num_tags = 1,
    .reset = rvswd_reset,
    .start = rvswd_start,
    .decode = rvswd_decode,
    .destroy = rvswd_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &rvswd_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}