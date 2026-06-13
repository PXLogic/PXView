/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2018 Steve R <steversig@virginmedia.com>
 * Copyright (C) 2024 DreamSourceLab <info@dreamsourcelab.com>
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
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_DATA 0

enum ook_ann {
    ANN_FRAME = 0,
    ANN_INFO,
    ANN_1111,
    ANN_1010,
    ANN_DIFFMAN,
    ANN_NRZ,
    NUM_ANN,
};

enum ook_state {
    STATE_IDLE,
    STATE_WAITING_FOR_PREAMBLE,
    STATE_DECODING,
    STATE_DECODE_TIMEOUT,
};

enum decode_type {
    DECODE_NRZ = 0,
    DECODE_MANCHESTER,
    DECODE_DIFF_MANCHESTER,
};

enum preamble_type {
    PREAMBLE_AUTO = 0,
    PREAMBLE_1010 = 1,
    PREAMBLE_1111 = 2,
};

typedef struct {
    uint64_t ss;
    uint64_t es;
    char state;
} ook_bit_t;

typedef struct ook_s {
    uint64_t ss;
    uint64_t es;
    int state;
    uint64_t ss_1111, ss_1010;
    uint64_t samplenumber_last;
    uint64_t sample_first;
    uint64_t sample_high;
    uint64_t sample_low;
    int edge_count;
    uint64_t word_first;
    int word_count;
    int insync;

    /* Manchester state */
    int lstate_1111;
    int lstate_1010;
    int half_time;
    int half_time_1010;
    int man_errors;
    int man_errors_1010;

    /* Diff Manchester state */
    char diff_man_trans;
    int diff_man_len;

    /* Preamble buffer */
    struct {
        uint64_t start;
        uint64_t samples;
        char state;
    } preamble[10];
    int preamble_count;

    /* Decoded stream */
    ook_bit_t decoded[1024];
    int decoded_count;
    ook_bit_t decoded_1010[1024];
    int decoded_1010_count;

    /* Pulse lengths for binary output */
    uint64_t pulse_lengths[1024];
    int pulse_count;

    /* Options */
    int invert;
    int decodeas;
    int preamble_val;
    int preamble_len;
    int diffmanvar;

    uint64_t samplerate;
    int out_ann;
    int out_python;
    int out_binary;
} ook_s;

/* Manually expand C_DECODER_STATE(ook, ...) because the struct body
   contains commas that confuse the C preprocessor. */
static void ook_reset(struct srd_decoder_inst *di) {
    ook_s *s = (ook_s *)calloc(1, sizeof(ook_s));
    c_decoder_set_private(di, s);
}
static void ook_destroy(struct srd_decoder_inst *di) {
    void *p = c_decoder_get_private(di);
    free(p);
    c_decoder_set_private(di, NULL);
}

static struct srd_channel ook_channels[] = {
    { "data", "DATA", "Data line", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option ook_options[] = {
    { "invert",    NULL, "Invert data",              NULL, NULL },
    { "decodeas",  NULL, "Decode type",              NULL, NULL },
    { "preamble",  NULL, "Preamble",                 NULL, NULL },
    { "preamlen",  NULL, "Filter length",            NULL, NULL },
    { "diffmanvar", NULL, "Transition at start",     NULL, NULL },
};

static const char *ook_ann_labels[][3] = {
    { "", "frame",   "Frame" },
    { "", "info",    "Info" },
    { "", "1111",    "1111" },
    { "", "1010",    "1010" },
    { "", "diffman", "Diff man" },
    { "", "nrz",     "NRZ" },
};

static const int ook_row_frames_classes[] = { ANN_FRAME, -1 };
static const int ook_row_info_classes[] = { ANN_INFO, -1 };
static const int ook_row_1111_classes[] = { ANN_1111, -1 };
static const int ook_row_1010_classes[] = { ANN_1010, -1 };
static const int ook_row_diffman_classes[] = { ANN_DIFFMAN, -1 };
static const int ook_row_nrz_classes[] = { ANN_NRZ, -1 };

static const struct srd_c_ann_row ook_ann_rows[] = {
    { "frames",    "Framing",   ook_row_frames_classes,  1 },
    { "info-vals", "Info",      ook_row_info_classes,    1 },
    { "man1111",   "Man 1111",  ook_row_1111_classes,    1 },
    { "man1010",   "Man 1010",  ook_row_1010_classes,    1 },
    { "diffmans",  "Diff man",  ook_row_diffman_classes, 1 },
    { "nrz-vals",  "NRZ",       ook_row_nrz_classes,     1 },
};

static const struct srd_decoder_binary ook_binary[] = {
    { 0, "pulse-lengths", "Pulse lengths" },
};

static const char *ook_inputs[] = { "logic" };
static const char *ook_outputs[] = { "ook" };
static const char *ook_tags[] = { "Encoding" };

static void decode_manchester_sim(struct srd_decoder_inst *di, ook_s *s,
    uint64_t start, uint64_t samples, char state, uint64_t dsamples,
    int *half_time, int *lstate, uint64_t *ss, int pream,
    ook_bit_t *decoded, int *decoded_count, int *errors)
{
    (void)di;
    ook_bit_t bit;
    bit.ss = 0;
    bit.es = 0;
    bit.state = 0;
    *errors = 0;

    if (s->edge_count == 0)
        (*half_time)++;

    if (samples > (uint64_t)(0.75 * (double)dsamples) && samples <= (uint64_t)(1.5 * (double)dsamples)) {
        /* Long pulse */
        *half_time += 2;
        uint64_t es;
        if (*half_time % 2 == 0) {
            es = start;
        } else {
            es = start + samples / 2;
        }
        if (*ss == start) {
            *lstate = 'E';
            es = start + samples;
        }
        if (!(s->edge_count == 0 && pream == 1010)) {
            bit.ss = *ss;
            bit.es = es;
            bit.state = (char)*lstate;
        }
        *lstate = state;
        *ss = es;
    } else if (samples > (uint64_t)(0.25 * (double)dsamples) && samples <= (uint64_t)(0.75 * (double)dsamples)) {
        /* Short pulse */
        *half_time += 1;
        if (*half_time % 2 == 0) {
            uint64_t es = start + samples;
            bit.ss = *ss;
            bit.es = es;
            bit.state = (char)*lstate;
            *lstate = state;
            *ss = es;
        } else {
            *ss = start;
            *lstate = state;
        }
    } else {
        /* Error */
        *errors = 1;
        if (s->state != STATE_DECODE_TIMEOUT) {
            *lstate = 'E';
            bit.ss = *ss;
            bit.es = *ss + samples;
            bit.state = 'E';
        } else {
            bit.ss = *ss;
            bit.es = *ss + s->sample_first;
            bit.state = (char)*lstate;
        }
        *ss = bit.es;
    }

    if (bit.state != 0 && *decoded_count < 1024) {
        decoded[*decoded_count] = bit;
        (*decoded_count)++;
    }
}

static void output_decoded_manchester(struct srd_decoder_inst *di, ook_s *s,
    ook_bit_t *decoded, int decoded_count, int ann_class)
{
    for (int i = 0; i < decoded_count; i++) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%c", decoded[i].state);
        c_put(di, decoded[i].ss, decoded[i].es, s->out_ann, ann_class, bit_str);
    }
}

static void output_frame_manchester(struct srd_decoder_inst *di, ook_s *s)
{
    ook_bit_t *best = s->decoded;
    int best_count = s->decoded_count;
    int best_ann = ANN_1111;

    if (s->preamble_val == PREAMBLE_1010) {
        best = s->decoded_1010;
        best_count = s->decoded_1010_count;
        best_ann = ANN_1010;
    } else if (s->preamble_val == PREAMBLE_1111) {
        best = s->decoded;
        best_count = s->decoded_count;
        best_ann = ANN_1111;
    } else {
        /* Auto: choose fewer errors */
        if (s->man_errors_1010 < s->man_errors) {
            best = s->decoded_1010;
            best_count = s->decoded_1010_count;
            best_ann = ANN_1010;
        }
    }

    output_decoded_manchester(di, s, best, best_count, best_ann);

    /* Frame annotation */
    if (best_count > 0) {
        char frame_str[256];
        int pos = 0;
        for (int i = 0; i < best_count && pos < (int)sizeof(frame_str) - 2; i++)
            pos += snprintf(frame_str + pos, sizeof(frame_str) - pos, "%c", best[i].state);
        c_put(di, best[0].ss, best[best_count - 1].es, s->out_ann, ANN_FRAME, frame_str);
    }

    /* Binary output: pulse lengths */
    if (s->pulse_count > 0) {
        uint8_t bin_data[1024 * 8];
        int bin_pos = 0;
        for (int i = 0; i < s->pulse_count && bin_pos + 8 <= (int)sizeof(bin_data); i++) {
            uint64_t pl = s->pulse_lengths[i];
            for (int b = 0; b < 8; b++)
                bin_data[bin_pos++] = (uint8_t)(pl >> (8 * b));
        }
        c_put_bin(di, s->ss, s->es, s->out_binary, 0, bin_pos, bin_data);
    }
}

static void decode_manchester(struct srd_decoder_inst *di, ook_s *s,
    uint64_t start, uint64_t samples, char state)
{
    uint64_t dsamples = s->sample_high;

    /* Decode 1111 preamble path */
    decode_manchester_sim(di, s, start, samples, state, dsamples,
        &s->half_time, &s->lstate_1111, &s->ss_1111, 1111,
        s->decoded, &s->decoded_count, &s->man_errors);

    /* Decode 1010 preamble path */
    decode_manchester_sim(di, s, start, samples, state, dsamples,
        &s->half_time_1010, &s->lstate_1010, &s->ss_1010, 1010,
        s->decoded_1010, &s->decoded_1010_count, &s->man_errors_1010);
}

static void decode_diff_manchester(struct srd_decoder_inst *di, ook_s *s,
    uint64_t start, uint64_t samples, char state)
{
    uint64_t dsamples = s->sample_high;
    int half_time = 0;

    if (samples > (uint64_t)(0.75 * (double)dsamples) && samples <= (uint64_t)(1.5 * (double)dsamples))
        half_time = 2;
    else if (samples > (uint64_t)(0.25 * (double)dsamples) && samples <= (uint64_t)(0.75 * (double)dsamples))
        half_time = 1;
    else
        return;

    for (int h = 0; h < half_time; h++) {
        char transition = (state != s->diff_man_trans) ? '1' : '0';
        s->diff_man_trans = state;

        if (s->decoded_count < 1024) {
            s->decoded[s->decoded_count].ss = start;
            s->decoded[s->decoded_count].es = start + dsamples;
            s->decoded[s->decoded_count].state = transition;
            s->decoded_count++;
        }

        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%c", transition);
        c_put(di, start, start + dsamples, s->out_ann, ANN_DIFFMAN, bit_str);
    }
}

static void decode_nrz(struct srd_decoder_inst *di, ook_s *s,
    uint64_t start, uint64_t samples, char state)
{
    
    int num_bits = 0;
    if (s->sample_high > 0)
        num_bits = (int)((samples + s->sample_high / 2) / s->sample_high);
    if (num_bits < 1) num_bits = 1;
    if (num_bits > 64) num_bits = 64;

    for (int i = 0; i < num_bits; i++) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%c", state);
        uint64_t bit_ss = start + i * s->sample_high;
        uint64_t bit_es = bit_ss + s->sample_high;
        c_put(di, bit_ss, bit_es, s->out_ann, ANN_NRZ, bit_str);

        if (s->decoded_count < 1024) {
            s->decoded[s->decoded_count].ss = bit_ss;
            s->decoded[s->decoded_count].es = bit_es;
            s->decoded[s->decoded_count].state = state;
            s->decoded_count++;
        }
    }
}

static void decode_timeout(struct srd_decoder_inst *di, ook_s *s)
{
    if (s->decodeas == DECODE_MANCHESTER) {
        output_frame_manchester(di, s);
    } else if (s->decodeas == DECODE_NRZ) {
        if (s->decoded_count > 0) {
            char frame_str[256];
            int pos = 0;
            for (int i = 0; i < s->decoded_count && pos < (int)sizeof(frame_str) - 2; i++)
                pos += snprintf(frame_str + pos, sizeof(frame_str) - pos, "%c", s->decoded[i].state);
            c_put(di, s->decoded[0].ss, s->decoded[s->decoded_count - 1].es,
                      s->out_ann, ANN_FRAME, frame_str);
        }
    } else if (s->decodeas == DECODE_DIFF_MANCHESTER) {
        if (s->decoded_count > 0) {
            char frame_str[256];
            int pos = 0;
            for (int i = 0; i < s->decoded_count && pos < (int)sizeof(frame_str) - 2; i++)
                pos += snprintf(frame_str + pos, sizeof(frame_str) - pos, "%c", s->decoded[i].state);
            c_put(di, s->decoded[0].ss, s->decoded[s->decoded_count - 1].es,
                      s->out_ann, ANN_FRAME, frame_str);
        }
    }

    /* Reset for next frame */
    s->decoded_count = 0;
    s->decoded_1010_count = 0;
    s->pulse_count = 0;
    s->man_errors = 0;
    s->man_errors_1010 = 0;
    s->half_time = 0;
    s->half_time_1010 = 0;
    s->insync = 0;
    s->state = STATE_IDLE;
}

static void lock_onto_preamble(struct srd_decoder_inst *di, ook_s *s,
    uint64_t samples, char state)
{
    if (s->preamble_count < 10) {
        s->preamble[s->preamble_count].start = s->samplenumber_last;
        s->preamble[s->preamble_count].samples = samples;
        s->preamble[s->preamble_count].state = state;
        s->preamble_count++;
    }

    if (s->preamble_count < 3) return;

    /* Check for noise: if ratio of longest to shortest > 5:1, reset */
    uint64_t min_s = UINT64_MAX, max_s = 0;
    for (int i = 0; i < s->preamble_count; i++) {
        if (s->preamble[i].samples < min_s) min_s = s->preamble[i].samples;
        if (s->preamble[i].samples > max_s) max_s = s->preamble[i].samples;
    }
    if (min_s > 0 && max_s / min_s > 5) {
        s->preamble_count = 0;
        return;
    }

    /* Need enough preamble pulses */
    if (s->preamble_count < s->preamble_len + 2) return;

    /* Calculate sample_high and sample_low from preamble */
    uint64_t sum_high = 0, sum_low = 0;
    int count_high = 0, count_low = 0;
    for (int i = 0; i < s->preamble_count; i++) {
        if (s->preamble[i].state == '1') {
            sum_high += s->preamble[i].samples;
            count_high++;
        } else {
            sum_low += s->preamble[i].samples;
            count_low++;
        }
    }

    if (count_high > 0 && count_low > 0) {
        s->sample_high = sum_high / count_high;
        s->sample_low = sum_low / count_low;
    } else if (count_high > 0) {
        s->sample_high = sum_high / count_high;
        s->sample_low = s->sample_high;
    } else if (count_low > 0) {
        s->sample_low = sum_low / count_low;
        s->sample_high = s->sample_low;
    }

    s->sample_first = s->sample_high > s->sample_low ? s->sample_high : s->sample_low;
    s->insync = 1;
    s->state = STATE_DECODING;

    /* Initialize Manchester state */
    s->ss_1111 = s->samplenumber_last;
    s->ss_1010 = s->samplenumber_last;
    s->lstate_1111 = state;
    s->lstate_1010 = state;
    s->diff_man_trans = state;

    c_put(di, s->preamble[0].start, s->samplenumber_last,
              s->out_ann, ANN_INFO, "Preamble detected");
}

static void ook_start(struct srd_decoder_inst *di)
{
    ook_s *s = (ook_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ook");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "ook");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "ook");

    const char *invert_str = c_opt_str(di, "invert", "no");
    s->invert = (strcmp(invert_str, "yes") == 0) ? 1 : 0;

    const char *decodeas_str = c_opt_str(di, "decodeas", "Manchester");
    if (strcmp(decodeas_str, "NRZ") == 0)
        s->decodeas = DECODE_NRZ;
    else if (strcmp(decodeas_str, "Diff Manchester") == 0)
        s->decodeas = DECODE_DIFF_MANCHESTER;
    else
        s->decodeas = DECODE_MANCHESTER;

    const char *preamble_str = c_opt_str(di, "preamble", "auto");
    if (strcmp(preamble_str, "1010") == 0)
        s->preamble_val = PREAMBLE_1010;
    else if (strcmp(preamble_str, "1111") == 0)
        s->preamble_val = PREAMBLE_1111;
    else
        s->preamble_val = PREAMBLE_AUTO;

    const char *preamlen_str = c_opt_str(di, "preamlen", "7");
    s->preamble_len = atoi(preamlen_str);
    if (s->preamble_len < 0) s->preamble_len = 0;
    if (s->preamble_len > 10) s->preamble_len = 10;

    const char *diffmanvar_str = c_opt_str(di, "diffmanvar", "1");
    s->diffmanvar = atoi(diffmanvar_str);

    /* Non-zero defaults that calloc didn't set */
    s->preamble_len = (s->preamble_len > 0) ? s->preamble_len : 7;
    s->diffmanvar = s->diffmanvar ? s->diffmanvar : 1;
}

static void ook_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    ook_s *s = (ook_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void ook_decode(struct srd_decoder_inst *di)
{
    ook_s *s = (ook_s *)c_decoder_get_private(di);

    if (s->samplerate == 0)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0)
        return;

    while (1) {
        int ret;
        if (s->edge_count == 0) {
            ret = c_wait(di, CW_E(CH_DATA), CW_END);
            s->state = STATE_DECODING;
        } else {
            ret = c_wait(di, CW_E(CH_DATA), CW_OR, CW_SKIP(5 * s->sample_first), CW_END);
            if (ret != SRD_OK)
                return;

            /* Check for timeout */
            uint64_t matched = di_matched(di);
            if ((matched & (1ULL << 1)) && !(matched & (1ULL << 0))) {
                s->state = STATE_DECODE_TIMEOUT;
            }
        }
        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);

        if (!s->samplenumber_last) {
            s->samplenumber_last = samplenum;
            s->word_first = samplenum;
            continue;
        }

        uint64_t samples = samplenum - s->samplenumber_last;
        if (!s->sample_first)
            s->sample_first = samples;

        /* Determine pin state */
        int pinstate = c_pin(di, CH_DATA);
        if (s->state == STATE_DECODE_TIMEOUT)
            pinstate = !pinstate;
        if (s->invert)
            pinstate = !pinstate;
        char state = pinstate ? '1' : '0';

        /* Store pulse length */
        if (s->pulse_count < 1024)
            s->pulse_lengths[s->pulse_count++] = samples;

        /* No preamble filtering when preamlen=0 */
        if (s->preamble_len == 0) {
            s->sample_high = s->sample_first;
            s->sample_low = s->sample_first;
            s->insync = 0;
        }

        if (!s->insync) {
            lock_onto_preamble(di, s, samples, state);
        } else {
            switch (s->decodeas) {
            case DECODE_NRZ:
                decode_nrz(di, s, s->samplenumber_last, samples, state);
                break;
            case DECODE_MANCHESTER:
                decode_manchester(di, s, s->samplenumber_last, samples, state);
                break;
            case DECODE_DIFF_MANCHESTER:
                decode_diff_manchester(di, s, s->samplenumber_last, samples, state);
                break;
            }
        }

        /* Handle timeout */
        if (s->state == STATE_DECODE_TIMEOUT) {
            decode_timeout(di, s);
        }

        s->samplenumber_last = samplenum;
        s->edge_count++;
    }
}

static struct srd_c_decoder ook_c_def = {
    .id = "ook_c",
    .name = "OOK(C)",
    .longname = "On-off keying (C)",
    .desc = "On-off keying protocol. (C implementation)",
    .license = "gplv2+",
    .channels = ook_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ook_options,
    .num_options = 5,
    .num_annotations = NUM_ANN,
    .ann_labels = ook_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = ook_ann_rows,
    .inputs = ook_inputs,
    .num_inputs = 1,
    .outputs = ook_outputs,
    .num_outputs = 1,
    .tags = ook_tags,
    .num_tags = 1,
    .binary = ook_binary,
    .num_binary = 1,
    .state_size = sizeof(ook_s),
    .reset = ook_reset,
    .start = ook_start,
    .decode = ook_decode,
    .destroy = ook_destroy,
    .metadata = ook_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ook_options[0].def = g_variant_new_string("no");
    ook_options[1].def = g_variant_new_string("Manchester");
    ook_options[2].def = g_variant_new_string("auto");
    ook_options[3].def = g_variant_new_string("7");
    ook_options[4].def = g_variant_new_string("1");

    /* Set enum value lists */
    GSList *invert_vals = NULL;
    GSList *decodeas_vals = NULL;
    GSList *preamble_vals = NULL;
    GSList *preamlen_vals = NULL;
    GSList *diffmanvar_vals = NULL;

    invert_vals = g_slist_append(invert_vals, g_variant_new_string("no"));
    invert_vals = g_slist_append(invert_vals, g_variant_new_string("yes"));
    ook_options[0].values = invert_vals;

    decodeas_vals = g_slist_append(decodeas_vals, g_variant_new_string("NRZ"));
    decodeas_vals = g_slist_append(decodeas_vals, g_variant_new_string("Manchester"));
    decodeas_vals = g_slist_append(decodeas_vals, g_variant_new_string("Diff Manchester"));
    ook_options[1].values = decodeas_vals;

    preamble_vals = g_slist_append(preamble_vals, g_variant_new_string("auto"));
    preamble_vals = g_slist_append(preamble_vals, g_variant_new_string("1010"));
    preamble_vals = g_slist_append(preamble_vals, g_variant_new_string("1111"));
    ook_options[2].values = preamble_vals;

    static const char *preamlen_strs[] = {"0","3","4","5","6","7","8","9","10"};
    for (int i = 0; i < 9; i++)
        preamlen_vals = g_slist_append(preamlen_vals, g_variant_new_string(preamlen_strs[i]));
    ook_options[3].values = preamlen_vals;

    diffmanvar_vals = g_slist_append(diffmanvar_vals, g_variant_new_string("1"));
    diffmanvar_vals = g_slist_append(diffmanvar_vals, g_variant_new_string("0"));
    ook_options[4].values = diffmanvar_vals;

    return &ook_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}