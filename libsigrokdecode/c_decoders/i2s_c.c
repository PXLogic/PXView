/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 * Copyright (C) 2019 DreamSourceLab <support@dreamsourcelab.com>
 * Copyright (C) 2025 C port (v4 API)
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

/* Channel indices — match Python channels tuple */
#define CH_SCK 0
#define CH_WS  1
#define CH_SD  2

#define ANN_LEFT  0
#define ANN_RIGHT 1
#define ANN_WARN  2
#define NUM_ANN   3

/* Decoder state struct — C_DECODER_STATE auto-generates i2s_s typedef,
 * i2s_reset (calloc), and i2s_destroy (free). */
C_DECODER_STATE(i2s, {
    int bit_depth;
    int msb_first;
    int ws_polarity_left_high;
    int clk_rising_edge;
    int bit_shift;
    int bit_align_left;
    int oldws;
    int bitcount;
    uint32_t data;
    uint32_t last;
    int samplesreceived;
    uint64_t ss_block;
    int wrote_wav_header;
    uint64_t samplerate;

    /* Output IDs */
    int out_ann;
    int out_python;
    int out_binary;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel i2s_channels[] = {
    {"sck", "SCK", "Bit clock line", 0, SRD_CHANNEL_SCLK, "dec_i2s_chan_sck"},
    {"ws", "WS", "Word select line", 1, SRD_CHANNEL_COMMON, "dec_i2s_chan_ws"},
    {"sd", "SD", "Serial data line", 2, SRD_CHANNEL_SDATA, "dec_i2s_chan_sd"},
};

/* Options — match Python options tuple */
static struct srd_decoder_option i2s_options[] = {
    {"ws_polarity", "dec_i2s_opt_ws_polarity", "WS polarity", NULL, NULL},
    {"clk_edge", "dec_i2s_opt_clk_edge", "SCK active edge", NULL, NULL},
    {"bit_shift", "dec_i2s_opt_bit_shift", "Bit shift", NULL, NULL},
    {"bit_align", "dec_i2s_opt_bit_align", "Bit align", NULL, NULL},
    {"bitorder", "dec_i2s_opt_bitorder", "Bit order", NULL, NULL},
    {"wordsize", "dec_i2s_opt_wordsize", "Word size", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *i2s_ann_labels[][3] = {
    {"", "left", "Left channel"},
    {"", "right", "Right channel"},
    {"", "warnings", "Warnings"},
};

static const char *i2s_inputs[] = {"logic", NULL};
static const char *i2s_outputs[] = {"i2s", NULL};
static const char *i2s_tags[] = {"Audio", "PC", NULL};

static const struct srd_decoder_binary i2s_binary[] = {
    {0, "wav", "WAV file"},
};

/* ---- Helper: write WAV header — match Python wav_header() ---- */
static void i2s_wav_header(struct srd_decoder_inst *di, i2s_s *s)
{
    unsigned char h[44];
    int wordlength = s->bit_depth;
    int num_channels = 2;
    int bytes_per_sample = (wordlength + 7) / 8;
    uint32_t sample_rate = 16000;
    uint32_t byte_rate = sample_rate * num_channels * bytes_per_sample;
    uint16_t block_align = num_channels * bytes_per_sample;
    uint16_t bits_per_sample = wordlength;
    uint32_t data_size = 0xFFFFFFFF;

    /* RIFF chunk descriptor */
    memcpy(h, "RIFF", 4);
    uint32_t chunk_size = 36 + data_size;
    h[4] = chunk_size & 0xFF;
    h[5] = (chunk_size >> 8) & 0xFF;
    h[6] = (chunk_size >> 16) & 0xFF;
    h[7] = (chunk_size >> 24) & 0xFF;
    memcpy(h + 8, "WAVE", 4);

    /* fmt subchunk */
    memcpy(h + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    h[16] = fmt_size & 0xFF;
    h[17] = (fmt_size >> 8) & 0xFF;
    h[18] = (fmt_size >> 16) & 0xFF;
    h[19] = (fmt_size >> 24) & 0xFF;
    uint16_t audio_format = 1; /* PCM */
    h[20] = audio_format & 0xFF;
    h[21] = (audio_format >> 8) & 0xFF;
    h[22] = num_channels & 0xFF;
    h[23] = (num_channels >> 8) & 0xFF;
    h[24] = sample_rate & 0xFF;
    h[25] = (sample_rate >> 8) & 0xFF;
    h[26] = (sample_rate >> 16) & 0xFF;
    h[27] = (sample_rate >> 24) & 0xFF;
    h[28] = byte_rate & 0xFF;
    h[29] = (byte_rate >> 8) & 0xFF;
    h[30] = (byte_rate >> 16) & 0xFF;
    h[31] = (byte_rate >> 24) & 0xFF;
    h[32] = block_align & 0xFF;
    h[33] = (block_align >> 8) & 0xFF;
    h[34] = bits_per_sample & 0xFF;
    h[35] = (bits_per_sample >> 8) & 0xFF;

    /* data subchunk */
    memcpy(h + 36, "data", 4);
    h[40] = data_size & 0xFF;
    h[41] = (data_size >> 8) & 0xFF;
    h[42] = (data_size >> 16) & 0xFF;
    h[43] = (data_size >> 24) & 0xFF;

    c_put_bin(di, 0, 0, s->out_binary, 0, sizeof(h), h);
    s->wrote_wav_header = 1;
}

/* ---- start callback — match Python start() ---- */
static void i2s_start(struct srd_decoder_inst *di)
{
    i2s_s *s = (i2s_s *)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "i2s");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "i2s");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "i2s");

    const char *ws_pol = c_opt_str(di, "ws_polarity", "left-high");
    s->ws_polarity_left_high = (strcmp(ws_pol, "left-high") == 0) ? 1 : 0;

    const char *clk_edge = c_opt_str(di, "clk_edge", "rising-edge");
    s->clk_rising_edge = (strcmp(clk_edge, "rising-edge") == 0) ? 1 : 0;

    const char *bit_shift = c_opt_str(di, "bit_shift", "none");
    s->bit_shift = (strcmp(bit_shift, "right-shifted by one") == 0) ? 1 : 0;

    const char *bit_align = c_opt_str(di, "bit_align", "left-aligned");
    s->bit_align_left = (strcmp(bit_align, "left-aligned") == 0) ? 1 : 0;

    s->bit_depth = (int)c_opt_int(di, "wordsize", 16);
    const char *msb = c_opt_str(di, "bitorder", "msb-first");
    s->msb_first = (strcmp(msb, "msb-first") == 0) ? 1 : 0;

    /* Non-zero defaults that calloc didn't set */
    s->oldws = 1;
    s->samplerate = c_samplerate(di);
}

/* ---- metadata callback — match Python metadata() ---- */
static void i2s_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    i2s_s *s = (i2s_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* ---- decode callback — match Python decode() ---- */
static void i2s_decode(struct srd_decoder_inst *di)
{
    i2s_s *s = (i2s_s *)c_decoder_get_private(di);
    int ret;

    int left_high = s->ws_polarity_left_high;
    int active_rising = s->clk_rising_edge;
    int right_shifted = s->bit_shift;
    int left_aligned = s->bit_align_left;
    int msb = s->msb_first;
    int wordlength = s->bit_depth;

    /* Wait for first WS edge — match Python:
     *   (sck, ws, sd) = self.wait({1: 'e'})
     *   self.ss_block = self.samplenum
     *   self.oldws = ws */
    ret = c_wait(di, CW_E(CH_WS), CW_END);
    if (ret != SRD_OK)
        return;
    s->ss_block = di_samplenum(di);
    s->oldws = c_pin(di, CH_WS);

    /* If right-shifted, wait for first SCK edge — match Python:
     *   if right_shifted:
     *       self.wait({0: 'r' if active_rising else 'f'}) */
    if (right_shifted) {
        if (active_rising)
            ret = c_wait(di, CW_R(CH_SCK), CW_END);
        else
            ret = c_wait(di, CW_F(CH_SCK), CW_END);
        if (ret != SRD_OK)
            return;
    }

    /* Main decode loop — match Python:
     *   while True:
     *       (sck, ws, sd) = self.wait({0: 'r' if active_rising else 'f'}) */
    while (1) {
        if (active_rising)
            ret = c_wait(di, CW_R(CH_SCK), CW_END);
        else
            ret = c_wait(di, CW_F(CH_SCK), CW_END);
        if (ret != SRD_OK)
            return;

        int ws = c_pin(di, CH_WS);
        int sd = c_pin(di, CH_SD);

        /* Shift bit into data register — match Python:
         *   if not right_shifted and ws != self.oldws:
         *       self.last = sd
         *   else:
         *       if msb: self.data = (self.data << 1) | sd
         *       else: self.data = self.data | (sd << self.bitcount)
         *       self.bitcount += 1 */
        if (!right_shifted && ws != s->oldws) {
            s->last = sd;
        } else {
            if (msb)
                s->data = (s->data << 1) | sd;
            else
                s->data = s->data | ((uint32_t)sd << s->bitcount);
            s->bitcount++;
        }

        /* Continue if WS hasn't flipped — match Python:
         *   if ws == self.oldws:
         *       continue */
        if (ws == s->oldws)
            continue;

        /* Write WAV header on first word — match Python:
         *   if not self.wrote_wav_header:
         *       self.put(0, 0, self.out_binary, [0, self.wav_header()])
         *       self.wrote_wav_header = True */
        if (!s->wrote_wav_header)
            i2s_wav_header(di, s);

        s->samplesreceived++;

        /* If right-shifted, wait for opposite SCK edge — match Python:
         *   if right_shifted:
         *       self.wait({0: 'f' if active_rising else 'r'}) */
        if (right_shifted) {
            if (active_rising)
                ret = c_wait(di, CW_F(CH_SCK), CW_END);
            else
                ret = c_wait(di, CW_R(CH_SCK), CW_END);
            if (ret != SRD_OK)
                return;
        }

        uint64_t samplenum = di_samplenum(di);

        /* Check word length — match Python:
         *   if self.wordlength > self.bitcount: */
        if (wordlength > s->bitcount) {
            char warn_str[128];
            snprintf(warn_str, sizeof(warn_str),
                "Received %d-bit word, expected %d-bit word",
                s->bitcount, wordlength);
            c_put(di, s->ss_block, samplenum, s->out_ann, ANN_WARN, warn_str);
        } else {
            /* Align data — match Python:
             *   if (left_algined and msb) or (not left_algined and not msb):
             *       self.data >>= self.bitcount - self.wordlength
             *   else:
             *       self.data &= int("1"*self.wordlength, 2) */
            uint32_t val = s->data;
            if ((left_aligned && msb) || (!left_aligned && !msb))
                val = val >> (s->bitcount - wordlength);
            else
                val = val & ((1u << wordlength) - 1);

            /* Determine channel — match Python:
             *   self.oldws = self.oldws if left_high else not self.oldws
             *   idx = 0 if self.oldws else 1
             *   c1 = 'Left channel' if self.oldws else 'Right channel'
             *   c2 = 'Left' if self.oldws else 'Right'
             *   c3 = 'L' if self.oldws else 'R' */
            int oldws_for_channel = left_high ? s->oldws : !s->oldws;
            int idx = oldws_for_channel ? 0 : 1;
            const char *c1 = oldws_for_channel ? "Left channel" : "Right channel";
            const char *c2 = oldws_for_channel ? "Left" : "Right";
            const char *c3 = oldws_for_channel ? "L" : "R";

            char v_str[16];
            snprintf(v_str, sizeof(v_str), "%08x", val);

            char ann1[64], ann2[64], ann3[64];
            snprintf(ann1, sizeof(ann1), "%s: %s", c1, v_str);
            snprintf(ann2, sizeof(ann2), "%s: %s", c2, v_str);
            snprintf(ann3, sizeof(ann3), "%s: %s", c3, v_str);

            /* Annotation — match Python:
             *   self.putb([idx, ['%s: %s' % (c1, v), '%s: %s' % (c2, v),
             *                '%s: %s' % (c3, v), c3]]) */
            c_put(di, s->ss_block, samplenum, s->out_ann, idx,
                  ann1, ann2, ann3, c3);

            /* Protocol output — match Python:
             *   self.putpb(['DATA', [c3, self.data]]) */
            c_proto(di, s->ss_block, samplenum, s->out_python,
                    "DATA", C_I8(c3[0]), C_U32(val), C_END);

            /* Binary output — match Python:
             *   self.putbin([0, self.wav_sample(self.data)]) */
            unsigned char bin_data[4];
            bin_data[0] = val & 0xFF;
            bin_data[1] = (val >> 8) & 0xFF;
            bin_data[2] = (val >> 16) & 0xFF;
            bin_data[3] = (val >> 24) & 0xFF;
            c_put_bin(di, s->ss_block, samplenum, s->out_binary, 0,
                      sizeof(bin_data), bin_data);
        }

        /* Reset decoder state — match Python:
         *   self.data = 0 if right_shifted else self.last
         *   self.bitcount = 0 if right_shifted else 1
         *   self.ss_block = self.samplenum
         *   self.oldws = ws */
        s->data = right_shifted ? 0 : s->last;
        s->bitcount = right_shifted ? 0 : 1;
        s->ss_block = samplenum;
        s->oldws = ws;
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder i2s_c_def = {
    .id = "i2s_c",
    .name = "I²S(C)",
    .longname = "Inter-IC Sound (C)",
    .desc = "I2S protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = i2s_channels,
    .num_channels = 3,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = i2s_options,
    .num_options = 6,
    .num_annotations = NUM_ANN,
    .ann_labels = i2s_ann_labels,
    .num_annotation_rows = 0,
    .annotation_rows = NULL,
    .inputs = i2s_inputs,
    .num_inputs = 1,
    .outputs = i2s_outputs,
    .num_outputs = 1,
    .binary = i2s_binary,
    .num_binary = 1,
    .tags = i2s_tags,
    .num_tags = 2,
    .state_size = sizeof(i2s_s),
    .reset = i2s_reset,
    .start = i2s_start,
    .decode = i2s_decode,
    .end = NULL,
    .metadata = i2s_metadata,
    .destroy = i2s_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    i2s_options[0].def = g_variant_new_string("left-high");
    i2s_options[1].def = g_variant_new_string("rising-edge");
    i2s_options[2].def = g_variant_new_string("none");
    i2s_options[3].def = g_variant_new_string("left-aligned");
    i2s_options[4].def = g_variant_new_string("msb-first");
    i2s_options[5].def = g_variant_new_int64(16);
    return &i2s_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}