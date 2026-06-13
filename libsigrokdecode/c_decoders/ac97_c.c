/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Gerhard Sittig <gerhard.sittig@gmx.net>
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
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

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AC97_MAX_SLOTS 13
#define AC97_FRAME_TOTAL_BITS 256
#define AC97_MAX_FRAME_SS (AC97_FRAME_TOTAL_BITS + 2)

enum ac97_ann {
    ANN_BITS_OUT = 0,
    ANN_BITS_IN,
    ANN_SLOT_OUT_RAW,
    ANN_SLOT_OUT_TAG,
    ANN_SLOT_OUT_CMD_ADDR,
    ANN_SLOT_OUT_CMD_DATA,
    ANN_SLOT_OUT_03,
    ANN_SLOT_OUT_04,
    ANN_SLOT_OUT_05,
    ANN_SLOT_OUT_06,
    ANN_SLOT_OUT_07,
    ANN_SLOT_OUT_08,
    ANN_SLOT_OUT_09,
    ANN_SLOT_OUT_10,
    ANN_SLOT_OUT_11,
    ANN_SLOT_OUT_IO_CTRL,
    ANN_SLOT_IN_RAW,
    ANN_SLOT_IN_TAG,
    ANN_SLOT_IN_STS_ADDR,
    ANN_SLOT_IN_STS_DATA,
    ANN_SLOT_IN_03,
    ANN_SLOT_IN_04,
    ANN_SLOT_IN_05,
    ANN_SLOT_IN_06,
    ANN_SLOT_IN_07,
    ANN_SLOT_IN_08,
    ANN_SLOT_IN_09,
    ANN_SLOT_IN_10,
    ANN_SLOT_IN_11,
    ANN_SLOT_IN_IO_STS,
    ANN_WARN,
    ANN_ERROR,
    NUM_ANN,
};

enum ac97_bin {
    BIN_FRAME_OUT = 0,
    BIN_FRAME_IN,
    BIN_SLOT_RAW_OUT,
    BIN_SLOT_RAW_IN,
    NUM_BIN,
};

#define CH_SYNC 0
#define CH_CLK 1
#define CH_SDATA_OUT 2
#define CH_SDATA_IN 3
#define CH_RESET 4

static const int frame_slot_lens[] = {0, 16, 36, 56, 76, 96, 116, 136, 156, 176, 196, 216, 236, 256};

struct ac97_priv {
    int have_sdo;
    int have_sdi;
    int have_reset;

    uint64_t frame_ss_list[AC97_MAX_FRAME_SS];
    int frame_ss_count;

    uint8_t frame_bits_out[AC97_FRAME_TOTAL_BITS];
    int frame_bits_out_len;
    uint8_t frame_bits_in[AC97_FRAME_TOTAL_BITS];
    int frame_bits_in_len;

    uint32_t frame_slot_data_out[AC97_MAX_SLOTS];
    int frame_slot_data_out_len;
    uint32_t frame_slot_data_in[AC97_MAX_SLOTS];
    int frame_slot_data_in_len;

    int have_slots_out[AC97_MAX_SLOTS];
    int have_slots_in[AC97_MAX_SLOTS];

    int prev_sync[3];

    uint64_t samplerate;
    int out_ann;
    int out_binary;
};

static struct srd_channel ac97_channels[] = {
    { "sync", "SYNC", "Frame synchronization", 0, SRD_CHANNEL_COMMON, "dec_ac97_chan_sync" },
    { "clk", "BIT_CLK", "Data bits clock", 1, SRD_CHANNEL_SCLK, "dec_ac97_chan_clk" },
};

static struct srd_channel ac97_optional_channels[] = {
    { "out", "SDATA_OUT", "Data output", 2, SRD_CHANNEL_SDATA, "dec_ac97_opt_chan_out" },
    { "in", "SDATA_IN", "Data input", 3, SRD_CHANNEL_SDATA, "dec_ac97_opt_chan_in" },
    { "rst", "RESET#", "Reset line", 4, SRD_CHANNEL_COMMON, "dec_ac97_opt_chan_rst" },
};

static const char *ac97_ann_labels[][3] = {
    { "", "Output bits", "Output bits" },
    { "", "Input bits", "Input bits" },
    { "", "Output raw value", "Output raw value" },
    { "", "Output TAG", "Output TAG" },
    { "", "Output command address", "Output command address" },
    { "", "Output command data", "Output command data" },
    { "", "Output slot 3", "Output slot 3" },
    { "", "Output slot 4", "Output slot 4" },
    { "", "Output slot 5", "Output slot 5" },
    { "", "Output slot 6", "Output slot 6" },
    { "", "Output slot 7", "Output slot 7" },
    { "", "Output slot 8", "Output slot 8" },
    { "", "Output slot 9", "Output slot 9" },
    { "", "Output slot 10", "Output slot 10" },
    { "", "Output slot 11", "Output slot 11" },
    { "", "Output I/O control", "Output I/O control" },
    { "", "Input raw value", "Input raw value" },
    { "", "Input TAG", "Input TAG" },
    { "", "Input status address", "Input status address" },
    { "", "Input status data", "Input status data" },
    { "", "Input slot 3", "Input slot 3" },
    { "", "Input slot 4", "Input slot 4" },
    { "", "Input slot 5", "Input slot 5" },
    { "", "Input slot 6", "Input slot 6" },
    { "", "Input slot 7", "Input slot 7" },
    { "", "Input slot 8", "Input slot 8" },
    { "", "Input slot 9", "Input slot 9" },
    { "", "Input slot 10", "Input slot 10" },
    { "", "Input slot 11", "Input slot 11" },
    { "", "Input I/O status", "Input I/O status" },
    { "", "Warning", "Warning" },
    { "", "Error", "Error" },
};

static const int ac97_row_bits_out_classes[] = { ANN_BITS_OUT, -1 };
static const int ac97_row_slots_out_raw_classes[] = { ANN_SLOT_OUT_RAW, -1 };
static const int ac97_row_slots_out_classes[] = {
    ANN_SLOT_OUT_TAG, ANN_SLOT_OUT_CMD_ADDR, ANN_SLOT_OUT_CMD_DATA,
    ANN_SLOT_OUT_03, ANN_SLOT_OUT_04, ANN_SLOT_OUT_05, ANN_SLOT_OUT_06,
    ANN_SLOT_OUT_07, ANN_SLOT_OUT_08, ANN_SLOT_OUT_09, ANN_SLOT_OUT_10,
    ANN_SLOT_OUT_11, ANN_SLOT_OUT_IO_CTRL, -1
};
static const int ac97_row_bits_in_classes[] = { ANN_BITS_IN, -1 };
static const int ac97_row_slots_in_raw_classes[] = { ANN_SLOT_IN_RAW, -1 };
static const int ac97_row_slots_in_classes[] = {
    ANN_SLOT_IN_TAG, ANN_SLOT_IN_STS_ADDR, ANN_SLOT_IN_STS_DATA,
    ANN_SLOT_IN_03, ANN_SLOT_IN_04, ANN_SLOT_IN_05, ANN_SLOT_IN_06,
    ANN_SLOT_IN_07, ANN_SLOT_IN_08, ANN_SLOT_IN_09, ANN_SLOT_IN_10,
    ANN_SLOT_IN_11, ANN_SLOT_IN_IO_STS, -1
};
static const int ac97_row_warnings_classes[] = { ANN_WARN, -1 };
static const int ac97_row_errors_classes[] = { ANN_ERROR, -1 };

static const struct srd_c_ann_row ac97_ann_rows[] = {
    { "bits-out", "Output bits", ac97_row_bits_out_classes, 1 },
    { "slots-out-raw", "Output numbers", ac97_row_slots_out_raw_classes, 1 },
    { "slots-out", "Output slots", ac97_row_slots_out_classes, 13 },
    { "bits-in", "Input bits", ac97_row_bits_in_classes, 1 },
    { "slots-in-raw", "Input numbers", ac97_row_slots_in_raw_classes, 1 },
    { "slots-in", "Input slots", ac97_row_slots_in_classes, 13 },
    { "warnings", "Warnings", ac97_row_warnings_classes, 1 },
    { "errors", "Errors", ac97_row_errors_classes, 1 },
};

static const struct srd_decoder_binary ac97_binary[] = {
    { 0, "frame-out", "Frame bits, output data" },
    { 1, "frame-in", "Frame bits, input data" },
    { 2, "slot-raw-out", "Raw slot bits, output data" },
    { 3, "slot-raw-in", "Raw slot bits, input data" },
};

static const char *ac97_inputs[] = { "logic" };
static const char *ac97_tags[] = { "Audio", "PC" };

static uint32_t bits_to_int(const uint8_t *bits, int count)
{
    uint32_t value = 0;
    for (int i = 0; i < count; i++)
        value = (value << 1) | (bits[i] ? 1 : 0);
    return value;
}

static uint32_t get_bit_field(uint32_t data, int size, int off, int count)
{
    uint32_t shift = size - off - count;
    data >>= shift;
    uint32_t mask = (1U << count) - 1;
    return data & mask;
}

static void int_to_nibble_text(uint32_t value, int bitcount, char *text, int text_size)
{
    int digits = (bitcount + 3) / 4;
    snprintf(text, text_size, "%0*x", digits, value);
}

static void bits_to_bin_ann(const uint8_t *bits, int count, unsigned char *out, int *out_len)
{
    int pos = 0;
    int i = 0;
    while (i < count) {
        unsigned char byte_val = 0;
        int n = count - i;
        if (n > 8) n = 8;
        for (int j = 0; j < n; j++)
            byte_val = (byte_val << 1) | (bits[i + j] ? 1 : 0);
        if (n < 8)
            byte_val <<= (8 - n);
        out[pos++] = byte_val;
        i += 8;
    }
    *out_len = pos;
}

static void putf(struct srd_decoder_inst *di, struct ac97_priv *s,
    int frombit, int bitcount, int cls, const char *text)
{
    if (frombit + bitcount > s->frame_ss_count)
        return;
    uint64_t ss = s->frame_ss_list[frombit];
    uint64_t es = s->frame_ss_list[frombit + bitcount];
    C_ANN_PUT(di, ss, es, s->out_ann, cls, text);
}

static void putb(struct srd_decoder_inst *di, struct ac97_priv *s,
    int frombit, int bitcount, int bincls, const unsigned char *data, int n_fields)
{
    if (frombit + bitcount > s->frame_ss_count)
        return;
    uint64_t ss = s->frame_ss_list[frombit];
    uint64_t es = s->frame_ss_list[frombit + bitcount];
    c_decoder_put_binary(di, ss, es, s->out_binary, bincls, n_fields, data);
}

static void handle_slot_dummy(struct srd_decoder_inst *di, struct ac97_priv *s,
    int slotidx, int bitidx, int bitcount, int is_out, uint32_t data)
{
    (void)bitidx;
    int *have_slots = is_out ? s->have_slots_out : s->have_slots_in;
    if (!have_slots[slotidx])
        return;

    char text[32];
    int_to_nibble_text(data, bitcount, text, sizeof(text));
    int anncls = is_out ? ANN_SLOT_OUT_TAG : ANN_SLOT_IN_TAG;
    putf(di, s, bitidx, bitcount, anncls + slotidx, text);

    int bincls = is_out ? BIN_SLOT_RAW_OUT : BIN_SLOT_RAW_IN;
    uint16_t data_bin = (uint16_t)((data >> 4) & 0xffff);
    unsigned char bdata[2];
    bdata[0] = (unsigned char)(data_bin >> 8);
    bdata[1] = (unsigned char)(data_bin & 0xff);
    putb(di, s, bitidx, bitcount, bincls, bdata, 2);
}

static void handle_slot_00(struct srd_decoder_inst *di, struct ac97_priv *s,
    int slotidx, int bitidx, int bitcount, int is_out, uint32_t data)
{
    (void)bitidx;
    int slotpos = frame_slot_lens[slotidx];
    int fieldoff = 0;
    int anncls = is_out ? ANN_SLOT_OUT_TAG : ANN_SLOT_IN_TAG;

    int fieldlen = 1;
    uint32_t ready = get_bit_field(data, bitcount, fieldoff, fieldlen);
    if (ready)
        putf(di, s, slotpos + fieldoff, fieldlen, anncls, "READY: 1");
    else
        putf(di, s, slotpos + fieldoff, fieldlen, anncls, "ready: 0");
    fieldoff += fieldlen;

    fieldlen = 12;
    uint32_t valid = get_bit_field(data, bitcount, fieldoff, fieldlen);
    char text[32];
    snprintf(text, sizeof(text), "VALID: %03x", valid);
    putf(di, s, slotpos + fieldoff, fieldlen, anncls, text);

    int *have_slots = is_out ? s->have_slots_out : s->have_slots_in;
    have_slots[0] = 1;
    for (int idx = 0; idx < 12; idx++)
        have_slots[idx + 1] = (valid & (1 << (11 - idx))) ? 1 : 0;
    fieldoff += fieldlen;

    fieldlen = 1;
    uint32_t rsv = get_bit_field(data, bitcount, fieldoff, fieldlen);
    if (rsv != 0)
        putf(di, s, slotpos + fieldoff, fieldlen, ANN_ERROR, "reserved bit error");
    fieldoff += fieldlen;

    fieldlen = 2;
    uint32_t codec = get_bit_field(data, bitcount, fieldoff, fieldlen);
    snprintf(text, sizeof(text), "CODEC: %01x", codec);
    putf(di, s, slotpos + fieldoff, fieldlen, anncls, text);
}

static void handle_slot_01(struct srd_decoder_inst *di, struct ac97_priv *s,
    int slotidx, int bitidx, int bitcount, int is_out, uint32_t data)
{
    (void)bitidx;
    int slotpos = frame_slot_lens[slotidx];
    int *have_slots = is_out ? s->have_slots_out : s->have_slots_in;
    if (!have_slots[slotidx])
        return;
    int fieldoff = 0;
    int anncls = (is_out ? ANN_SLOT_OUT_TAG : ANN_SLOT_IN_TAG) + slotidx;

    int fieldlen = 1;
    if (is_out) {
        uint32_t is_read = get_bit_field(data, bitcount, fieldoff, fieldlen);
        putf(di, s, slotpos + fieldoff, fieldlen, anncls,
             is_read ? "READ" : "WRITE");
    } else {
        uint32_t rsv = get_bit_field(data, bitcount, fieldoff, fieldlen);
        if (rsv != 0)
            putf(di, s, slotpos + fieldoff, fieldlen, ANN_ERROR, "reserved bit error");
    }
    fieldoff += fieldlen;

    fieldlen = 7;
    uint32_t regaddr = get_bit_field(data, bitcount, fieldoff, fieldlen);
    char text[32];
    snprintf(text, sizeof(text), "ADDR: %02x", regaddr);
    putf(di, s, slotpos + fieldoff, fieldlen, anncls, text);
    if (regaddr & 0x01)
        putf(di, s, slotpos + fieldoff, fieldlen, ANN_ERROR, "odd register address");
    fieldoff += fieldlen;

    fieldlen = 10;
    uint32_t reqdata = get_bit_field(data, bitcount, fieldoff, fieldlen);
    if (is_out && reqdata != 0)
        putf(di, s, slotpos + fieldoff, fieldlen, ANN_ERROR, "reserved bit error");
    if (!is_out) {
        snprintf(text, sizeof(text), "REQ: %03x", reqdata);
        putf(di, s, slotpos + fieldoff, fieldlen, anncls, text);
    }
    fieldoff += fieldlen;

    fieldlen = 2;
    uint32_t rsv2 = get_bit_field(data, bitcount, fieldoff, fieldlen);
    if (rsv2 != 0)
        putf(di, s, slotpos + fieldoff, fieldlen, ANN_ERROR, "reserved bits error");
}

static void handle_slot_02(struct srd_decoder_inst *di, struct ac97_priv *s,
    int slotidx, int bitidx, int bitcount, int is_out, uint32_t data)
{
    (void)bitidx;
    int slotpos = frame_slot_lens[slotidx];
    int *have_slots = is_out ? s->have_slots_out : s->have_slots_in;
    if (!have_slots[slotidx])
        return;
    int fieldoff = 0;
    int anncls = (is_out ? ANN_SLOT_OUT_TAG : ANN_SLOT_IN_TAG) + slotidx;

    int fieldlen = 16;
    uint32_t rwdata = get_bit_field(data, bitcount, fieldoff, fieldlen);
    char text[32];
    snprintf(text, sizeof(text), "DATA: %04x", rwdata);
    putf(di, s, slotpos + fieldoff, fieldlen, anncls, text);
    fieldoff += fieldlen;

    fieldlen = 4;
    uint32_t rsv = get_bit_field(data, bitcount, fieldoff, fieldlen);
    if (rsv != 0)
        putf(di, s, slotpos + fieldoff, fieldlen, ANN_ERROR, "reserved bits error");
}

static void handle_slot(struct srd_decoder_inst *di, struct ac97_priv *s,
    int slotidx, int data_out_valid, uint32_t data_out,
    int data_in_valid, uint32_t data_in)
{
    int bitidx = frame_slot_lens[slotidx];
    int bitcount = frame_slot_lens[slotidx + 1] - bitidx;

    if (data_out_valid) {
        switch (slotidx) {
        case 0: handle_slot_00(di, s, slotidx, bitidx, bitcount, 1, data_out); break;
        case 1: handle_slot_01(di, s, slotidx, bitidx, bitcount, 1, data_out); break;
        case 2: handle_slot_02(di, s, slotidx, bitidx, bitcount, 1, data_out); break;
        default: handle_slot_dummy(di, s, slotidx, bitidx, bitcount, 1, data_out); break;
        }
    }
    if (data_in_valid) {
        switch (slotidx) {
        case 0: handle_slot_00(di, s, slotidx, bitidx, bitcount, 0, data_in); break;
        case 1: handle_slot_01(di, s, slotidx, bitidx, bitcount, 0, data_in); break;
        case 2: handle_slot_02(di, s, slotidx, bitidx, bitcount, 0, data_in); break;
        default: handle_slot_dummy(di, s, slotidx, bitidx, bitcount, 0, data_in); break;
        }
    }
}

static void flush_frame_bits(struct srd_decoder_inst *di, struct ac97_priv *s)
{
    if (s->frame_bits_out_len > 0) {
        unsigned char out[AC97_FRAME_TOTAL_BITS / 8 + 1];
        int out_len;
        bits_to_bin_ann(s->frame_bits_out, s->frame_bits_out_len, out, &out_len);
        putb(di, s, 0, s->frame_bits_out_len, BIN_FRAME_OUT, out, out_len);
    }
    if (s->frame_bits_in_len > 0) {
        unsigned char out[AC97_FRAME_TOTAL_BITS / 8 + 1];
        int out_len;
        bits_to_bin_ann(s->frame_bits_in, s->frame_bits_in_len, out, &out_len);
        putb(di, s, 0, s->frame_bits_in_len, BIN_FRAME_IN, out, out_len);
    }
}

static void start_frame(struct ac97_priv *s, uint64_t ss)
{
    if (s->frame_ss_count > 0)
        s->frame_ss_count = 0;
    s->frame_ss_list[0] = ss;
    s->frame_ss_count = 1;
    s->frame_bits_out_len = 0;
    s->frame_bits_in_len = 0;
    s->frame_slot_data_out_len = 0;
    s->frame_slot_data_in_len = 0;
    memset(s->have_slots_out, 0, sizeof(s->have_slots_out));
    memset(s->have_slots_in, 0, sizeof(s->have_slots_in));
}

static void handle_bits(struct srd_decoder_inst *di, struct ac97_priv *s,
    uint64_t ss, uint64_t es, int bit_out, int bit_in)
{
    if (bit_out >= 0 && s->have_sdo) {
        char text[4];
        snprintf(text, sizeof(text), "%d", bit_out);
        C_ANN_PUT(di, ss, es, s->out_ann, ANN_BITS_OUT, text);
    }
    if (bit_in >= 0 && s->have_sdi) {
        char text[4];
        snprintf(text, sizeof(text), "%d", bit_in);
        C_ANN_PUT(di, ss, es, s->out_ann, ANN_BITS_IN, text);
    }

    if (s->frame_ss_count == 0)
        return;

    if (s->frame_ss_count < AC97_MAX_FRAME_SS)
        s->frame_ss_list[s->frame_ss_count++] = es;

    int have_len = s->frame_ss_count - 1;
    if (have_len > AC97_FRAME_TOTAL_BITS)
        return;

    int slot_idx = 0;
    if (bit_out >= 0 && s->have_sdo) {
        if (s->frame_bits_out_len < AC97_FRAME_TOTAL_BITS)
            s->frame_bits_out[s->frame_bits_out_len++] = bit_out ? 1 : 0;
        slot_idx = s->frame_slot_data_out_len;
    }
    if (bit_in >= 0 && s->have_sdi) {
        if (s->frame_bits_in_len < AC97_FRAME_TOTAL_BITS)
            s->frame_bits_in[s->frame_bits_in_len++] = bit_in ? 1 : 0;
        slot_idx = s->frame_slot_data_in_len;
    }

    if (slot_idx + 1 >= AC97_MAX_SLOTS)
        return;
    int want_len = frame_slot_lens[slot_idx + 1];
    if (have_len != want_len)
        return;
    int prev_len = frame_slot_lens[slot_idx];

    uint32_t slot_data_out = 0;
    int slot_data_out_valid = 0;
    if (bit_out >= 0 && s->have_sdo) {
        slot_data_out = bits_to_int(s->frame_bits_out + prev_len, want_len - prev_len);
        if (s->frame_slot_data_out_len < AC97_MAX_SLOTS)
            s->frame_slot_data_out[s->frame_slot_data_out_len++] = slot_data_out;
        slot_data_out_valid = 1;
    }

    uint32_t slot_data_in = 0;
    int slot_data_in_valid = 0;
    if (bit_in >= 0 && s->have_sdi) {
        slot_data_in = bits_to_int(s->frame_bits_in + prev_len, want_len - prev_len);
        if (s->frame_slot_data_in_len < AC97_MAX_SLOTS)
            s->frame_slot_data_in[s->frame_slot_data_in_len++] = slot_data_in;
        slot_data_in_valid = 1;
    }

    int slot_len = have_len - prev_len;
    uint64_t slot_ss = s->frame_ss_list[prev_len];
    uint64_t slot_es = s->frame_ss_list[have_len];
    char slot_text[32];
    if (slot_data_out_valid) {
        int_to_nibble_text(slot_data_out, slot_len, slot_text, sizeof(slot_text));
        C_ANN_PUT(di, slot_ss, slot_es, s->out_ann, ANN_SLOT_OUT_RAW, slot_text);
    }
    if (slot_data_in_valid) {
        int_to_nibble_text(slot_data_in, slot_len, slot_text, sizeof(slot_text));
        C_ANN_PUT(di, slot_ss, slot_es, s->out_ann, ANN_SLOT_IN_RAW, slot_text);
    }

    handle_slot(di, s, slot_idx, slot_data_out_valid, slot_data_out,
                slot_data_in_valid, slot_data_in);
}

static void ac97_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct ac97_priv)));
    }
    struct ac97_priv *s = (struct ac97_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct ac97_priv));
    s->out_ann = -1;
    s->out_binary = -1;
    s->prev_sync[0] = -1;
    s->prev_sync[1] = -1;
    s->prev_sync[2] = -1;
}

static void ac97_start(struct srd_decoder_inst *di)
{
    struct ac97_priv *s = (struct ac97_priv *)c_decoder_get_private(di);
    s->out_ann = c_decoder_register_output(di, SRD_OUTPUT_ANN, "ac97");
    s->out_binary = c_decoder_register_output(di, SRD_OUTPUT_BINARY, "ac97");

    s->have_sdo = c_decoder_has_channel(di, CH_SDATA_OUT);
    s->have_sdi = c_decoder_has_channel(di, CH_SDATA_IN);
    s->have_reset = c_decoder_has_channel(di, CH_RESET);
}

static void ac97_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct ac97_priv *s = (struct ac97_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void ac97_decode(struct srd_decoder_inst *di)
{
    struct ac97_priv *s = (struct ac97_priv *)c_decoder_get_private(di);

    if (!s->have_sdo && !s->have_sdi)
        return;

    if (!s->samplerate) {
        s->samplerate = c_decoder_get_samplerate(di);
    }

    int ret = c_wait(di, CW_E(CH_CLK), CW_END);
    if (ret != SRD_OK)
        return;

    int clk = c_pin(di, CH_CLK);
    uint64_t bit_ss = di_samplenum(di);

    if (clk == 0) {
        s->prev_sync[2] = c_pin(di, CH_SYNC);
        ret = c_wait(di, CW_R(CH_CLK), CW_END);
        if (ret != SRD_OK)
            return;
        bit_ss = di_samplenum(di);
    }

    while (1) {
        ret = c_wait(di, CW_F(CH_CLK), CW_END);
        if (ret != SRD_OK)
            return;

        int sync = c_pin(di, CH_SYNC);
        s->prev_sync[0] = s->prev_sync[1];
        s->prev_sync[1] = s->prev_sync[2];
        s->prev_sync[2] = sync;

        /* Read SDATA_OUT/SDATA_IN at the falling edge (data is valid here),
         * matching Python which reads from the falling-edge wait() return. */
        int bit_out = s->have_sdo ? c_pin(di, CH_SDATA_OUT) : -1;
        int bit_in = s->have_sdi ? c_pin(di, CH_SDATA_IN) : -1;

        ret = c_wait(di, CW_R(CH_CLK), CW_END);
        if (ret != SRD_OK)
            return;

        if (s->prev_sync[0] == 0 && s->prev_sync[1] == 1) {
            if (s->frame_ss_count > 0)
                flush_frame_bits(di, s);
            start_frame(s, bit_ss);
        }

        handle_bits(di, s, bit_ss, di_samplenum(di), bit_out, bit_in);
        bit_ss = di_samplenum(di);
    }
}

static void ac97_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ac97_c_decoder = {
    .id = "ac97_c",
    .name = "AC '97(C)",
    .longname = "Audio Codec '97 (C)",
    .desc = "Audio and modem control for PC systems.",
    .license = "gplv2+",
    .channels = ac97_channels,
    .num_channels = 2,
    .optional_channels = ac97_optional_channels,
    .num_optional_channels = 3,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ac97_ann_labels,
    .num_annotation_rows = 8,
    .annotation_rows = ac97_ann_rows,
    .inputs = ac97_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = ac97_binary,
    .num_binary = NUM_BIN,
    .tags = ac97_tags,
    .num_tags = 2,
    .reset = ac97_reset,
    .start = ac97_start,
    .decode = ac97_decode,
    .destroy = ac97_destroy,
    .metadata = ac97_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ac97_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}