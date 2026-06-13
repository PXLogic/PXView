/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2021
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

/* Annotation class indices — match Python annotations tuple */
enum {
    ANN_DATA_SYMBOL = 0,
    ANN_CTRL_SYMBOL,
    ANN_BIT,
    ANN_BYTE,
    NUM_ANN,
};

/* 4B5B Data Symbols — match Python symbols.py sym_data */
static const int data_sym[32] = {
    -1, -1, -1, -1,
    -1, -1, -1, -1,
    -1, 0x1, 0x4, 0x5,
    -1, -1, 0x6, 0x7,
    -1, -1, 0x8, 0x9,
    0x2, 0x3, 0xA, 0xB,
    -1, -1, 0xC, 0xD,
    0xE, 0xF, 0x0, -1,
};

/* 4B5B Control Symbols — match Python symbols.py sym_ctrl */
static const char *ctrl_long[32] = {
    "QUIET", NULL, NULL, NULL,
    "HALT", NULL, "L", "RESET",
    NULL, NULL, NULL, NULL,
    NULL, "TERMINATE", NULL, NULL,
    NULL, "K", NULL, NULL,
    NULL, NULL, NULL, NULL,
    "J", "SET", NULL, NULL,
    NULL, NULL, NULL, "IDLE",
};

static const char *ctrl_short[32] = {
    "Q", NULL, NULL, NULL,
    "H", NULL, "L", "R",
    NULL, NULL, NULL, NULL,
    NULL, "T", NULL, NULL,
    NULL, "K", NULL, NULL,
    NULL, NULL, NULL, NULL,
    "J", "S", NULL, NULL,
    NULL, NULL, NULL, "I",
};

/* Decoder state struct — C_DECODER_STATE auto-generates fourb5b_s typedef,
 * fourb5b_reset (calloc), and fourb5b_destroy (free). */
C_DECODER_STATE(fourb5b, {
    uint64_t samplerate;
    int jk_seen_j;
    int jk_seen_k;
    uint64_t sym_start;
    uint64_t data_start;
    int symbol;
    int bit_count;
    int last_nibble;
    int has_last_nibble;
    int bit_offset;
    int out_ann;
    int out_python;
});

/* Options — match Python options tuple */
static struct srd_decoder_option fourb5b_options[] = {
    {"bit_offset", NULL, "Bit offset", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *ann_labels[][3] = {
    {"", "symbol_data", "Data symbol"},
    {"", "symbol_ctrl", "Control symbol"},
    {"", "bit", "Decoded bit"},
    {"", "byte", "Decoded byte"},
};

static const int row_symbol_classes[] = {0, 1, -1};
static const int row_bits_classes[] = {2, -1};
static const int row_bytes_classes[] = {3, -1};
static const struct srd_c_ann_row ann_rows[] = {
    {"symbol", "Symbols", row_symbol_classes, 2},
    {"bits", "Bits", row_bits_classes, 1},
    {"bytes", "Bytes", row_bytes_classes, 1},
};

static const char *inputs[] = {"nrzi", NULL};
static const char *outputs[] = {"4b5b", NULL};
static const char *tags[] = {"Encoding", NULL};

/* ---- metadata callback — match Python metadata() ---- */
static void fourb5b_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    fourb5b_s *s = (fourb5b_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* ---- start callback — match Python start() ---- */
static void fourb5b_start(struct srd_decoder_inst *di)
{
    fourb5b_s *s = (fourb5b_s *)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "4b5b");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "4b5b");
    s->samplerate = c_samplerate(di);
    s->last_nibble = -1;

    const char *offset_str = c_opt_str(di, "bit_offset", "0");
    s->bit_offset = offset_str ? atoi(offset_str) : 0;
}

/* ---- recv_proto callback — match Python decode(startsample, endsample, data) ---- */
static void fourb5b_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample,
                               uint64_t end_sample, const char *cmd,
                               const c_field *fields, int n_fields)
{
    fourb5b_s *s = (fourb5b_s *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Only handle "BIT" commands from NRZI decoder */
    if (strncmp(cmd, "BIT", 3) != 0)
        return;

    int bit_val = (fields && n_fields > 0) ? (fields[0].u8 & 1) : 0;

    /* Offset symbol starting point — match Python:
     *   if self.bit_offset > 0:
     *       self.bit_offset -= 1
     *       return */
    if (s->bit_offset > 0) {
        s->bit_offset--;
        return;
    }

    /* Set symbol and data byte start samples — match Python:
     *   if self.bits == 0:
     *       self.symbol_start = startsample
     *       if self.last_nibble == None:
     *           self.data_start = startsample */
    if (s->bit_count == 0) {
        s->sym_start = start_sample;
        if (!s->has_last_nibble)
            s->data_start = start_sample;
    }

    /* Shift bit into symbol — match Python:
     *   self.symbol = (self.symbol << 1) | data
     *   self.bits += 1 */
    s->symbol = (s->symbol << 1) | bit_val;
    s->bit_count++;

    /* If all bits for symbol received — match Python:
     *   if self.bits == 5: */
    if (s->bit_count == 5) {
        /* Control symbol — match Python:
         *   if self.symbol in sym_ctrl: */
        if (ctrl_long[s->symbol]) {
            /* Check for start sequence symbols (J, K) — match Python:
             *   self.jk[0] = self.symbol == 0b11000 or self.jk[0]   # J
             *   self.jk[1] = self.symbol == 0b10001 or self.jk[1]   # K */
            s->jk_seen_j = (s->symbol == 24) || s->jk_seen_j;   /* J = 0b11000 = 24 */
            s->jk_seen_k = (s->symbol == 17) || s->jk_seen_k;   /* K = 0b10001 = 17 */

            /* Add control symbol annotation — match Python:
             *   self.putx([1, sym_ctrl[self.symbol]]) */
            c_put(di, s->sym_start, end_sample, s->out_ann, ANN_CTRL_SYMBOL,
                  ctrl_long[s->symbol], ctrl_short[s->symbol]);

            /* Push control symbol to stacked decoders — match Python:
             *   self.putp((sym_ctrl[self.symbol][1], True)) */
            c_proto(di, s->sym_start, end_sample, s->out_python,
                    ctrl_short[s->symbol], C_U8(s->symbol), C_U8(1), C_END);

        }
        /* Data symbol (only if decoder has seen JK start sequence) — match Python:
         *   elif self.symbol in sym_data and self.jk == [True, True]: */
        else if (data_sym[s->symbol] >= 0 && s->jk_seen_j && s->jk_seen_k) {
            /* Add data symbol annotations — match Python:
             *   self.putx([0, ['{:05b}'.format(self.symbol)]])
             *   self.putx([2, ['{:04b}'.format(sym_data[self.symbol])]]) */
            char sym_str[6];
            for (int i = 4; i >= 0; i--)
                sym_str[4 - i] = ((s->symbol >> i) & 1) + '0';
            sym_str[5] = '\0';
            c_put(di, s->sym_start, end_sample, s->out_ann, ANN_DATA_SYMBOL, sym_str);

            int nibble = data_sym[s->symbol];
            char bit_str[5];
            for (int i = 3; i >= 0; i--)
                bit_str[3 - i] = ((nibble >> i) & 1) + '0';
            bit_str[4] = '\0';
            c_put(di, s->sym_start, end_sample, s->out_ann, ANN_BIT, bit_str);

            /* Second nibble of data byte — match Python:
             *   if self.last_nibble != None: */
            if (s->has_last_nibble) {
                int data_byte = (nibble << 4) | s->last_nibble;

                /* Add data byte annotation — match Python:
                 *   self.ss_block = self.data_start
                 *   self.es_block = endsample
                 *   self.putx([3, ['0x{:02X}'.format(data_byte)]]) */
                char byte_str[8];
                snprintf(byte_str, sizeof(byte_str), "0x%02X", data_byte);
                c_put(di, s->data_start, end_sample, s->out_ann, ANN_BYTE, byte_str);

                /* Push byte to stacked decoders — match Python:
                 *   self.putp((data_byte, False)) */
                c_proto(di, s->data_start, end_sample, s->out_python,
                        "DATA", C_U8(data_byte), C_U8(0), C_END);

                s->data_start = end_sample;
                s->has_last_nibble = 0;
            }
            /* First nibble of data byte — match Python:
             *   else:
             *       self.last_nibble = sym_data[self.symbol] */
            else {
                s->last_nibble = nibble;
                s->has_last_nibble = 1;
            }
        }

        /* Reset symbol value — match Python:
         *   self.symbol_start = endsample
         *   self.symbol = 0
         *   self.bits = 0 */
        s->sym_start = end_sample;
        s->symbol = 0;
        s->bit_count = 0;
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder fourb5b_c_def = {
    .id = "4b5b_c",
    .name = "4B5B(C)",
    .longname = "4B5B Line Code (C)",
    .desc = "Maps 4 data bits to 5 bit symbols for transmission. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = fourb5b_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ann_rows,
    .inputs = inputs,
    .num_inputs = 1,
    .outputs = outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = tags,
    .num_tags = 1,
    .state_size = sizeof(fourb5b_s),
    .reset = fourb5b_reset,
    .start = fourb5b_start,
    .decode = NULL,
    .metadata = fourb5b_metadata,
    .destroy = fourb5b_destroy,
    .decode_upper = fourb5b_recv_proto,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals[] = {
        g_variant_new_string("0"),
        g_variant_new_string("1"),
        g_variant_new_string("2"),
        g_variant_new_string("3"),
        g_variant_new_string("4"),
    };
    GSList *val_list = NULL;
    for (int i = 0; i < 5; i++)
        val_list = g_slist_append(val_list, vals[i]);
    fourb5b_options[0].def = g_variant_new_string("0");
    fourb5b_options[0].values = val_list;
    return &fourb5b_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}