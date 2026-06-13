/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2018 Mike Jagdis <mjagdis@eris-associates.co.uk>
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_SWIM 0

enum swim_ann {
    ANN_BIT = 0,
    ANN_ENTERSEQ,
    ANN_START_HOST,
    ANN_START_TARGET,
    ANN_PARITY,
    ANN_ACK,
    ANN_NACK,
    ANN_BYTE_WRITE,
    ANN_BYTE_READ,
    ANN_CMD_UNKNOWN,
    ANN_CMD,
    ANN_BYTES,
    ANN_ADDRESS,
    ANN_DATA_WRITE,
    ANN_DATA_READ,
    ANN_DEBUG,
    NUM_ANN,
};

enum swim_bin {
    BIN_TX = 0,
    BIN_RX,
    NUM_BIN,
};

enum swim_proto_state {
    PROTO_CMD = 0,
    PROTO_N,
    PROTO_ADDR_E,
    PROTO_ADDR_H,
    PROTO_ADDR_L,
    PROTO_DATA,
};

struct swim_priv {
    double HSI;
    double HSI_min;
    double HSI_max;
    double swim_clock;
    uint64_t bit_reflen;
    uint64_t sync_reflen_min;
    uint64_t sync_reflen_max;
    uint64_t eseq_reflen;
    int debug;

    int eseq_edge_val[2];
    uint64_t eseq_edge_ss[2];
    int eseq_pairnum;
    uint64_t eseq_pairstart;

    int bit_edge_val[2];
    uint64_t bit_edge_ss[2];
    int bit_maxlen;

    int bitseq_len;
    uint64_t bitseq_start;
    uint64_t bitseq_end;
    uint8_t bitseq_value;
    int bitseq_dir;

    int proto_state;
    int proto_byte_count;
    uint32_t proto_addr;
    uint64_t proto_addr_start;

    uint64_t samplerate;
    int out_ann;
    int out_binary;
};

static struct srd_channel swim_channels[] = {
    { "swim", "SWIM", "SWIM data line", 0, SRD_CHANNEL_SDATA, "dec_swim_chan_swim" },
};

static struct srd_decoder_option swim_options[] = {
    { "debug", "dec_swim_opt_debug", "Debug", NULL, NULL },
};

static const char *swim_ann_labels[][3] = {
    { "", "Bit", "Bit" },
    { "", "SWIM enter sequence", "SWIM enter sequence" },
    { "", "Start bit (host)", "Start bit (host)" },
    { "", "Start bit (target)", "Start bit (target)" },
    { "", "Parity bit", "Parity bit" },
    { "", "Acknowledgement", "ACK" },
    { "", "Negative acknowledgement", "NACK" },
    { "", "Byte write", "Byte write" },
    { "", "Byte read", "Byte read" },
    { "", "Unknown SWIM command", "Unknown SWIM command" },
    { "", "SWIM command", "SWIM command" },
    { "", "Byte count", "Byte count" },
    { "", "Address", "Address" },
    { "", "Data write", "Data write" },
    { "", "Data read", "Data read" },
    { "", "Debug", "Debug" },
};

static const int swim_row_bits_classes[] = { ANN_BIT, -1 };
static const int swim_row_framing_classes[] = { ANN_START_HOST, ANN_START_TARGET, ANN_PARITY, ANN_ACK, ANN_NACK, ANN_BYTE_WRITE, ANN_BYTE_READ, -1 };
static const int swim_row_protocol_classes[] = { ANN_ENTERSEQ, ANN_CMD_UNKNOWN, ANN_CMD, ANN_BYTES, ANN_ADDRESS, ANN_DATA_WRITE, ANN_DATA_READ, -1 };
static const int swim_row_debug_classes[] = { ANN_DEBUG, -1 };

static const struct srd_c_ann_row swim_ann_rows[] = {
    { "bits", "Bits", swim_row_bits_classes, 1 },
    { "framing", "Framing", swim_row_framing_classes, 7 },
    { "protocol", "Protocol", swim_row_protocol_classes, 7 },
    { "debug", "Debug", swim_row_debug_classes, 1 },
};

static const struct srd_decoder_binary swim_binary[] = {
    { 0, "tx", "Dump of data written to target" },
    { 1, "rx", "Dump of data read from target" },
};

static const char *swim_inputs[] = { "logic" };
static const char *swim_tags[] = { "Debug/trace" };

static void adjust_timings(struct swim_priv *s)
{
    s->bit_reflen = (uint64_t)ceil((double)s->samplerate * 22.0 / s->swim_clock);
}

static void swim_protocol(struct srd_decoder_inst *di, struct swim_priv *s)
{
    if (s->proto_state == PROTO_CMD) {
        if (s->bitseq_value == 0x00) {
            c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ANN_CMD, "system reset");
        } else if (s->bitseq_value == 0x01) {
            s->proto_state = PROTO_N;
            c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ANN_CMD, "read on-the-fly");
        } else if (s->bitseq_value == 0x02) {
            s->proto_state = PROTO_N;
            c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ANN_CMD, "write on-the-fly");
        } else {
            c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ANN_CMD_UNKNOWN, "unknown");
        }
    } else if (s->proto_state == PROTO_N) {
        s->proto_byte_count = s->bitseq_value;
        s->proto_state = PROTO_ADDR_E;
        char text[32];
        snprintf(text, sizeof(text), "byte count 0x%02x", s->bitseq_value);
        c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ANN_BYTES, text);
    } else if (s->proto_state == PROTO_ADDR_E) {
        s->proto_addr = s->bitseq_value;
        s->proto_addr_start = s->bitseq_start;
        s->proto_state = PROTO_ADDR_H;
    } else if (s->proto_state == PROTO_ADDR_H) {
        s->proto_addr = (s->proto_addr << 8) | s->bitseq_value;
        s->proto_state = PROTO_ADDR_L;
    } else if (s->proto_state == PROTO_ADDR_L) {
        s->proto_addr = (s->proto_addr << 8) | s->bitseq_value;
        s->proto_state = PROTO_DATA;
        char text[32];
        snprintf(text, sizeof(text), "address 0x%06x", s->proto_addr);
        c_put(di, s->proto_addr_start, s->bitseq_end, s->out_ann, ANN_ADDRESS, text);
    } else {
        if (s->proto_byte_count > 0) {
            s->proto_byte_count--;
            if (s->proto_byte_count == 0)
                s->proto_state = PROTO_CMD;
        }
        char text[16];
        snprintf(text, sizeof(text), "0x%02x", s->bitseq_value);
        int ann = (s->bitseq_dir == 0) ? ANN_DATA_WRITE : ANN_DATA_READ;
        c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ann, text);

        uint8_t buf = s->bitseq_value;
        int bincls = (s->bitseq_dir == 0) ? BIN_TX : BIN_RX;
        c_put_bin(di, s->bitseq_start, s->bitseq_end, s->out_binary, bincls, 1, &buf);

        if (s->debug) {
            char dbuf[16];
            snprintf(dbuf, sizeof(dbuf), "%d more", s->proto_byte_count);
            c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ANN_DEBUG, dbuf);
        }
    }
}

static void swim_bitseq(struct srd_decoder_inst *di, struct swim_priv *s,
    uint64_t bitstart, uint64_t bitend, int bit)
{
    if (s->bitseq_len == 0) {
        s->bit_reflen = bitend - bitstart;
        s->bitseq_value = 0;
        s->bitseq_dir = bit;
        s->bitseq_len = 1;
        int ann = (bit == 0) ? ANN_START_HOST : ANN_START_TARGET;
        c_put(di, bitstart, bitend, s->out_ann, ann, "start", "s");
    } else if ((s->proto_state == PROTO_CMD && s->bitseq_len == 4) ||
               (s->proto_state != PROTO_CMD && s->bitseq_len == 9)) {
        s->bitseq_end = bitstart;
        s->bitseq_len++;
        c_put(di, bitstart, bitend, s->out_ann, ANN_PARITY, "parity");
        s->bitseq_value &= 0xff;
        char text[16];
        snprintf(text, sizeof(text), "0x%02x", s->bitseq_value);
        int ann = (s->bitseq_dir == 0) ? ANN_BYTE_WRITE : ANN_BYTE_READ;
        c_put(di, s->bitseq_start, s->bitseq_end, s->out_ann, ann, text);
    } else if ((s->proto_state == PROTO_CMD && s->bitseq_len == 5) ||
               (s->proto_state != PROTO_CMD && s->bitseq_len == 10)) {
        if (bit)
            c_put(di, bitstart, bitend, s->out_ann, ANN_ACK, "ack");
        else
            c_put(di, bitstart, bitend, s->out_ann, ANN_NACK, "nack");

        if (bit)
            swim_protocol(di, s);

        s->bitseq_len = 0;
    } else {
        if (s->bitseq_len == 1)
            s->bitseq_start = bitstart;
        s->bitseq_value = (s->bitseq_value << 1) | bit;
        s->bitseq_len++;
    }
}

static void swim_decode_bit(struct srd_decoder_inst *di, struct swim_priv *s,
    uint64_t start, uint64_t mid, uint64_t end)
{
    int bit;
    if (mid - start >= end - mid) {
        bit = 0;
        c_put(di, start, end, s->out_ann, ANN_BIT, "0");
    } else {
        bit = 1;
        c_put(di, start, end, s->out_ann, ANN_BIT, "1");
    }
    swim_bitseq(di, s, start, end, bit);
}

static void detect_synchronize_frame(struct srd_decoder_inst *di, struct swim_priv *s,
    uint64_t start, uint64_t end)
{
    (void)start;
    uint64_t low_duration = end - s->eseq_edge_ss[1];
    if (low_duration >= s->sync_reflen_min && low_duration <= s->sync_reflen_max) {
        c_put(di, s->eseq_edge_ss[1], end, s->out_ann, ANN_ENTERSEQ,
                  "synchronization frame", "synchronization", "sync", "s");

        s->bit_edge_val[0] = -1; s->bit_edge_ss[0] = 0;
        s->bit_edge_val[1] = -1; s->bit_edge_ss[1] = 0;
        s->bit_maxlen = -1;
        s->bitseq_len = 0;
        s->bitseq_end = 0;
        s->proto_state = PROTO_CMD;

        if (end != s->eseq_edge_ss[1])
            s->swim_clock = 128.0 * ((double)s->samplerate / (double)(end - s->eseq_edge_ss[1]));
        adjust_timings(s);
    }
}

static void detect_enter_sequence(struct srd_decoder_inst *di, struct swim_priv *s, uint64_t start, uint64_t end)
{
    if (s->eseq_pairnum == 0 || llabs((int64_t)s->eseq_reflen - (int64_t)(end - start)) > 2) {
        s->eseq_pairstart = start;
        s->eseq_reflen = end - start;
        s->eseq_pairnum = 1;
    } else if (s->eseq_pairnum < 4) {
        s->eseq_pairnum++;
        if (s->eseq_pairnum == 4)
            s->eseq_reflen /= 2;
    } else {
        s->eseq_pairnum++;
        if (s->eseq_pairnum == 8) {
            /* Emit enter sequence annotation, matching Python */
            c_put(di, s->eseq_pairstart, end, s->out_ann, ANN_ENTERSEQ,
                      "enter sequence", "enter seq", "enter", "ent", "e");
            s->eseq_pairnum = 0;
        }
    }
}

static void swim_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct swim_priv)));
    }
    struct swim_priv *s = (struct swim_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct swim_priv));

    s->HSI = 8000000.0;
    s->HSI_min = s->HSI * 0.9;
    s->HSI_max = s->HSI * 1.1;
    s->swim_clock = s->HSI_min / 2.0;

    s->eseq_edge_val[0] = -1; s->eseq_edge_ss[0] = 0;
    s->eseq_edge_val[1] = -1; s->eseq_edge_ss[1] = 0;
    s->eseq_pairnum = 0;
    s->eseq_pairstart = 0;

    s->bit_edge_val[0] = -1; s->bit_edge_ss[0] = 0;
    s->bit_edge_val[1] = -1; s->bit_edge_ss[1] = 0;
    s->bit_maxlen = -1;
    s->bitseq_len = 0;
    s->proto_state = PROTO_CMD;
    s->out_ann = -1;
    s->out_binary = -1;
}

static void swim_start(struct srd_decoder_inst *di)
{
    struct swim_priv *s = (struct swim_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "swim");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "swim");

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);

    s->sync_reflen_min = (uint64_t)floor((double)s->samplerate * 64.0 / s->HSI_max);
    s->sync_reflen_max = (uint64_t)ceil((double)s->samplerate * 128.0 / (s->HSI_min / 2.0));

    const char *debug_str = c_opt_str(di, "debug", "no");
    s->debug = (strcmp(debug_str, "yes") == 0) ? 1 : 0;

    s->eseq_reflen = (uint64_t)ceil((double)s->samplerate / 2048.0);
    adjust_timings(s);
}

static void swim_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct swim_priv *s = (struct swim_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void swim_decode(struct srd_decoder_inst *di)
{
    struct swim_priv *s = (struct swim_priv *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (!s->samplerate) return;
        s->sync_reflen_min = (uint64_t)floor((double)s->samplerate * 64.0 / s->HSI_max);
        s->sync_reflen_max = (uint64_t)ceil((double)s->samplerate * 128.0 / (s->HSI_min / 2.0));
        s->eseq_reflen = (uint64_t)ceil((double)s->samplerate / 2048.0);
        adjust_timings(s);
    }

    while (1) {
        int swim_val;
        if (s->bit_maxlen >= 0) {
            int ret = c_wait(di, CW_END);
            if (ret != SRD_OK)
                return;
            swim_val = c_pin(di, CH_SWIM);
            s->bit_maxlen--;
        } else {
            int ret = c_wait(di, CW_E(CH_SWIM), CW_END);
            if (ret != SRD_OK)
                return;
            swim_val = c_pin(di, CH_SWIM);
        }

        if (swim_val != s->eseq_edge_val[1]) {
            if (swim_val == 1 && s->eseq_edge_ss[1] != 0) {
                detect_synchronize_frame(di, s, s->eseq_edge_ss[1], di_samplenum(di));
                if (s->eseq_edge_ss[0] != 0)
                    detect_enter_sequence(di, s, s->eseq_edge_ss[0], di_samplenum(di));
            }
            s->eseq_edge_val[0] = s->eseq_edge_val[1];
            s->eseq_edge_ss[0] = s->eseq_edge_ss[1];
            s->eseq_edge_val[1] = swim_val;
            s->eseq_edge_ss[1] = di_samplenum(di);
        }

        int cur_swim = swim_val;
        if ((cur_swim != s->bit_edge_val[1] && (cur_swim != 1 || s->bit_edge_val[1] != -1)) ||
            s->bit_maxlen == 0) {
            if (s->bit_maxlen == 0 && s->bit_edge_val[1] == 1)
                cur_swim = -1;

            if (s->bit_edge_val[1] != 0 && cur_swim == 0)
                s->bit_maxlen = (int)s->bit_reflen;

            if (s->bit_edge_val[0] == 0 && s->bit_edge_val[1] == 1 &&
                di_samplenum(di) - s->bit_edge_ss[0] <= s->bit_reflen + 10) {
                swim_decode_bit(di, s, s->bit_edge_ss[0], s->bit_edge_ss[1], di_samplenum(di));
            }

            s->bit_edge_val[0] = s->bit_edge_val[1];
            s->bit_edge_ss[0] = s->bit_edge_ss[1];
            s->bit_edge_val[1] = cur_swim;
            s->bit_edge_ss[1] = di_samplenum(di);
        }
    }
}

static void swim_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder swim_c_decoder = {
    .id = "swim_c",
    .name = "SWIM(C)",
    .longname = "STM8 SWIM bus (C)",
    .desc = "STM8 Single Wire Interface Module (SWIM) protocol.",
    .license = "gplv2+",
    .channels = swim_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = swim_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = swim_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = swim_ann_rows,
    .inputs = swim_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = swim_binary,
    .num_binary = NUM_BIN,
    .tags = swim_tags,
    .num_tags = 1,
    .reset = swim_reset,
    .start = swim_start,
    .decode = swim_decode,
    .destroy = swim_destroy,
    .state_size = 0,
    .metadata = swim_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &swim_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}