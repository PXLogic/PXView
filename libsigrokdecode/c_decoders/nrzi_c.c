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

/* Channel indices — match Python channels tuple */
#define CH_DATA 0

/* Annotation class indices — match Python annotations tuple */
enum nrzi_ann {
    ANN_PREAMBLE = 0,
    ANN_BIT,
    NUM_ANN,
};

enum nrzi_internal_state { STATE_SYNC, STATE_DECODE };

/* Decoder state struct — C_DECODER_STATE auto-generates nrzi_s typedef,
 * nrzi_reset (calloc), and nrzi_destroy (free). */
C_DECODER_STATE(nrzi, {
    enum nrzi_internal_state state;

    uint64_t sync_cycles[64];
    int sync_count;
    int preamble_len;
    uint64_t symbol_len;
    uint64_t samplerate;
    uint64_t ss_block;
    uint64_t es_block;
    uint64_t preamble_start;

    /* Output IDs */
    int out_ann;
    int out_python;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel nrzi_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL},
};

/* Options — match Python options tuple */
static struct srd_decoder_option nrzi_options[] = {
    {"preamble_len", NULL, "Preamble Length", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *nrzi_ann_labels[][3] = {
    {"", "preamble", "Preamble"},
    {"", "bit", "Decoded bits"},
};

static const int nrzi_row_bits_classes[] = {0, 1, -1};
static const struct srd_c_ann_row nrzi_ann_rows[] = {
    {"bits", "Bits", nrzi_row_bits_classes, 2},
};

static const char *nrzi_inputs[] = {"logic", NULL};
static const char *nrzi_outputs[] = {"nrzi", NULL};
static const char *nrzi_tags[] = {"Encoding", NULL};

/* ---- start callback — match Python start() ---- */
static void nrzi_start(struct srd_decoder_inst *di)
{
    nrzi_s *s = (nrzi_s *)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "nrzi");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "nrzi");

    const char *plen_str = c_opt_str(di, "preamble_len", "16");
    s->preamble_len = plen_str ? atoi(plen_str) : 16;
    s->samplerate = c_samplerate(di);
}

/* ---- metadata callback — match Python metadata() ---- */
static void nrzi_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    nrzi_s *s = (nrzi_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* ---- decode callback — match Python decode() ---- */
static void nrzi_decode(struct srd_decoder_inst *di)
{
    nrzi_s *s = (nrzi_s *)c_decoder_get_private(di);
    int ret;

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0)
        return;

    /* Wait for first rising edge — match Python:
     *   self.wait({0: 'r'})
     *   preamble_start = self.samplenum */
    ret = c_wait(di, CW_R(CH_DATA), CW_END);
    if (ret != SRD_OK)
        return;
    s->preamble_start = di_samplenum(di);

    while (1) {
        if (s->state == STATE_SYNC) {
            /* Previous edge */
            uint64_t start = di_samplenum(di);

            /* Next rising edge — match Python:
             *   self.wait({0: 'r'})
             *   end = self.samplenum */
            ret = c_wait(di, CW_R(CH_DATA), CW_END);
            if (ret != SRD_OK)
                return;
            uint64_t end = di_samplenum(di);

            /* Add cycle length to list */
            if (s->sync_count < 64) {
                s->sync_cycles[s->sync_count] = end - start;
                s->sync_count++;
            }

            /* Calculate clock rate — match Python:
             *   if len(self.sync_cycles) == self.preamble_len: */
            if (s->sync_count == s->preamble_len) {
                uint64_t sum = 0;
                for (int i = 0; i < s->sync_count; i++)
                    sum += s->sync_cycles[i];
                double avg_cycle = (double)sum / (double)s->sync_count;
                s->symbol_len = (uint64_t)(avg_cycle / 2.0 + 0.5);

                double clock_rate = (double)s->samplerate / ((double)s->symbol_len * 2.0);

                /* Format frequency string — match Python's str(float) formatting.
                 * Python's '{}'.format(float) uses repr() which gives enough
                 * digits to uniquely identify the float. Use %g with enough
                 * precision, and ensure integer values get ".0" suffix. */
                char freq_str[64];
                if (clock_rate >= 1e6) {
                    double mhz = clock_rate / 1e6;
                    if (mhz == (double)(long long)mhz && mhz < 1e15)
                        snprintf(freq_str, sizeof(freq_str), "%.1f MHz", mhz);
                    else
                        snprintf(freq_str, sizeof(freq_str), "%g MHz", mhz);
                } else if (clock_rate >= 1e3) {
                    double khz = clock_rate / 1e3;
                    if (khz == (double)(long long)khz && khz < 1e15)
                        snprintf(freq_str, sizeof(freq_str), "%.1f kHz", khz);
                    else
                        snprintf(freq_str, sizeof(freq_str), "%g kHz", khz);
                } else {
                    if (clock_rate == (double)(long long)clock_rate && clock_rate < 1e15)
                        snprintf(freq_str, sizeof(freq_str), "%.1f Hz", clock_rate);
                    else
                        snprintf(freq_str, sizeof(freq_str), "%g Hz", clock_rate);
                }

                char preamble_str[128];
                snprintf(preamble_str, sizeof(preamble_str), "Preamble (%s)", freq_str);

                /* Preamble annotation — match Python:
                 *   self.ss_block = preamble_start
                 *   self.es_block = self.samplenum
                 *   self.putx([0, ['Preamble ({})'.format(frequency)]]) */
                c_put(di, s->preamble_start, di_samplenum(di),
                      s->out_ann, ANN_PREAMBLE, preamble_str);

                /* Skip to start of next symbol after preamble — match Python:
                 *   self.wait({'skip': int(self.symbol_len / 2)}) */
                ret = c_wait(di, CW_SKIP(s->symbol_len / 2), CW_END);
                if (ret != SRD_OK)
                    return;

                s->state = STATE_DECODE;
            }

        } else if (s->state == STATE_DECODE) {
            /* Start of bit — match Python:
             *   start_sample = self.samplenum */
            uint64_t start_sample = di_samplenum(di);

            /* Wait for edge or one symbol len — match Python:
             *   self.wait([{0: 'e'}, {'skip': self.symbol_len}]) */
            ret = c_wait(di, CW_E(CH_DATA), CW_OR, CW_SKIP(s->symbol_len), CW_END);
            if (ret != SRD_OK)
                return;

            uint64_t matched = di_matched(di);

            /* Check if transition was detected — match Python:
             *   if self.matched == (True, False): */
            if (matched & (1ULL << 0)) {
                /* Edge matched — adjust symbol length so transition is at mid-point */
                uint64_t edge_samp = di_samplenum(di) - start_sample;
                int64_t offset = (int64_t)(s->symbol_len / 2) - (int64_t)edge_samp;
                int64_t remaining = (int64_t)s->symbol_len - (int64_t)edge_samp - offset;

                /* Skip forward to end of symbol — match Python:
                 *   self.wait({'skip': remaining}) */
                if (remaining > 0) {
                    ret = c_wait(di, CW_SKIP((uint64_t)remaining), CW_END);
                    if (ret != SRD_OK)
                        return;
                }
            }

            /* Add bit annotation — match Python:
             *   self.ss_block = start_sample
             *   self.es_block = self.samplenum
             *   self.putx([1, ['{:n}'.format(self.matched & (0b1 << 0))]]) */
            int bit_val = (matched & (1ULL << 0)) ? 1 : 0;
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", bit_val);
            c_put(di, start_sample, di_samplenum(di), s->out_ann, ANN_BIT, bit_str);

            /* Push bit to stacked decoders — match Python:
             *   self.putp(self.matched & (0b1 << 0)) */
            c_proto(di, start_sample, di_samplenum(di), s->out_python,
                    "BIT", C_U8(bit_val), C_END);
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder nrzi_c_def = {
    .id = "nrzi_c",
    .name = "NRZ-I(C)",
    .longname = "Non-return-to-zero Inverted (C)",
    .desc = "Bits encoded as presence or absence of a transition. (C implementation)",
    .license = "gplv2+",
    .channels = nrzi_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = nrzi_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = nrzi_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = nrzi_ann_rows,
    .inputs = nrzi_inputs,
    .num_inputs = 1,
    .outputs = nrzi_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = nrzi_tags,
    .num_tags = 1,
    .state_size = sizeof(nrzi_s),
    .reset = nrzi_reset,
    .start = nrzi_start,
    .decode = nrzi_decode,
    .end = NULL,
    .metadata = nrzi_metadata,
    .destroy = nrzi_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    nrzi_options[0].def = g_variant_new_string("16");
    GSList *vals = NULL;
    vals = g_slist_append(vals, g_variant_new_string("4"));
    vals = g_slist_append(vals, g_variant_new_string("8"));
    vals = g_slist_append(vals, g_variant_new_string("16"));
    vals = g_slist_append(vals, g_variant_new_string("32"));
    vals = g_slist_append(vals, g_variant_new_string("64"));
    nrzi_options[0].values = vals;
    return &nrzi_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}