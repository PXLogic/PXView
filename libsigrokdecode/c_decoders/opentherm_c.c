/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2024 DreamSourceLab <info@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum ot_ann {
    ANN_BIT = 0,
    ANN_STARTBIT,
    ANN_STOPBIT,
    ANN_PARITYBIT,
    ANN_MSGTYPE,
    ANN_SPARE,
    ANN_DATAID,
    ANN_DATAVALUE,
    ANN_M2S,
    ANN_S2M,
    ANN_FRAME,
    ANN_TIMING,
    ANN_WARNING,
    ANN_OTX,
    NUM_ANN,
};

enum ot_state {
    STATE_IDLE,
    STATE_SYNC,
    STATE_MID1,
    STATE_MID0,
    STATE_START1,
    STATE_START0,
};

static const struct {
    const char *dir;
    const char *name;
    const char *short_name;
} msg_type_table[8] = {
    { "M2S", "READ-DATA",       "RD"   },
    { "M2S", "WRITE-DATA",      "WD"   },
    { "M2S", "INVALID-DATA",    "INV"  },
    { "M2S", "RESERVED",        "RSV"  },
    { "S2M", "READ-ACK",        "RACK" },
    { "S2M", "WRITE-ACK",       "WACK" },
    { "S2M", "DATA-INVALID",    "INV"  },
    { "S2M", "UNKNOWN-DATAID",  "UNK"  },
};

typedef struct {
    /* Decoder state */
    int state;

    /* Edge tracking */
    uint64_t edges[64];
    int edge_count;

    /* Bit tracking */
    struct {
        uint64_t sample;
        int value;
    } bits[34];
    int bit_count;

    /* ss/es for each bit */
    struct {
        uint64_t ss;
        uint64_t es;
    } ss_es_bits[34];

    /* Previous edge info */
    uint64_t prev_samplenum;
    int prev_lvl;
    uint64_t c_samplenum;
    int c_lvl;

    /* Timing parameters */
    uint64_t halfbit;
    uint64_t s_range_min;
    uint64_t s_range_max;
    uint64_t l_range_min;
    uint64_t l_range_max;
    uint64_t silence;
    uint64_t glitchlen;

    uint64_t last_frame_edge;

    /* Options */
    int polarity;       /* 0=active-low, 1=active-high */
    int format;         /* 0=hex, 1=dec, 2=oct, 3=bin */
    int64_t bitlen;
    int64_t jitter_m;
    int64_t jitter_p;
    int64_t m2s_silence_min;
    int64_t m2s_silence_max;
    int64_t s2m_silence_min;
    int64_t m2m_act_max;
    int64_t ignore_glitches;

    uint64_t samplerate;
    int out_ann;
    int timing_set;
    int prev_samplenum_valid;
} ot_priv;

static struct srd_channel ot_channels[] = {
    { "ot", "OT", "OpenTherm line", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option ot_options[] = {
    { "polarity",          NULL, "Polarity",                                NULL, NULL },
    { "bitlen",            NULL, "Single bit period (us)",                  NULL, NULL },
    { "jitter_m",          NULL, "Edge jitter minus (us)",                  NULL, NULL },
    { "jitter_p",          NULL, "Edge jitter plus (us)",                   NULL, NULL },
    { "m2s_silence_min",   NULL, "Master to Slave min silence (us)",       NULL, NULL },
    { "m2s_silence_max",   NULL, "Master to Slave max silence (us)",       NULL, NULL },
    { "s2m_silence_min",   NULL, "Slave to Master min silence (us)",       NULL, NULL },
    { "m2m_act_max",       NULL, "Master req to req max period (us)",      NULL, NULL },
    { "ignore_glitches",   NULL, "Ignore glitches up to (us)",             NULL, NULL },
    { "format",            NULL, "Data format",                             NULL, NULL },
};

static const char *ot_ann_labels[][3] = {
    { "", "bit",        "Bit" },
    { "", "startbit",   "Startbit" },
    { "", "stopbit",    "Stopbit" },
    { "", "paritybit",  "Paritybit" },
    { "", "msgtype",    "MSG-TYPE" },
    { "", "spare",      "Spare" },
    { "", "dataid",     "DATA-ID" },
    { "", "datavalue",  "DATA-VALUE" },
    { "", "m2s",        "MasterToSlave" },
    { "", "s2m",        "SlaveToMaster" },
    { "", "frame",      "OpenThermFrame" },
    { "", "timing",     "Timing error" },
    { "", "warning",    "Warning" },
    { "", "otx",        "OpenThermExchange" },
};

static const int ot_row_bits_classes[] = { ANN_BIT, -1 };
static const int ot_row_fields_classes[] = { ANN_STARTBIT, ANN_STOPBIT, ANN_PARITYBIT, ANN_MSGTYPE, ANN_SPARE, ANN_DATAID, ANN_DATAVALUE, -1 };
static const int ot_row_direction_classes[] = { ANN_M2S, ANN_S2M, -1 };
static const int ot_row_frames_classes[] = { ANN_FRAME, -1 };
static const int ot_row_otxs_classes[] = { ANN_OTX, -1 };
static const int ot_row_warnings_classes[] = { ANN_WARNING, -1 };
static const int ot_row_timings_classes[] = { ANN_TIMING, -1 };

static const struct srd_c_ann_row ot_ann_rows[] = {
    { "bits",     "Bits",           ot_row_bits_classes,     1 },
    { "fields",   "Fields",         ot_row_fields_classes,   7 },
    { "direction", "Direction",     ot_row_direction_classes, 2 },
    { "frames",   "Frame",          ot_row_frames_classes,   1 },
    { "otxs",     "Description",    ot_row_otxs_classes,     1 },
    { "warnings", "Warnings",       ot_row_warnings_classes, 1 },
    { "timings",  "Timing errors",  ot_row_timings_classes,  1 },
};

static const char *ot_inputs[] = { "logic" };
static const char *ot_tags[] = { "OT" };

static int64_t s2t(ot_priv *s, uint64_t samples)
{
    if (s->samplerate == 0) return 0;
    return (int64_t)(samples * 1000000ULL / s->samplerate);
}

static uint64_t t2s(ot_priv *s, int64_t us)
{
    if (s->samplerate == 0) return 0;
    return (uint64_t)(us * s->samplerate / 1000000ULL);
}

static void setup_calc(ot_priv *s)
{
    if (s->samplerate == 0) return;

    s->halfbit = t2s(s, s->bitlen / 2);
    uint64_t jitter_m_s = t2s(s, s->jitter_m);
    uint64_t jitter_p_s = t2s(s, s->jitter_p);

    s->s_range_min = s->halfbit - jitter_m_s;
    s->s_range_max = s->halfbit + jitter_p_s;
    s->l_range_min = s->halfbit * 2 - jitter_m_s;
    s->l_range_max = s->halfbit * 2 + jitter_p_s;
    s->silence = t2s(s, s->m2s_silence_min);
    s->glitchlen = t2s(s, s->ignore_glitches);
    s->timing_set = 1;
}

static char edge_type(ot_priv *s, uint64_t prev_edge, uint64_t cur_edge)
{
    uint64_t diff = cur_edge - prev_edge;
    if (diff >= s->s_range_min && diff <= s->s_range_max)
        return 's';
    if (diff >= s->l_range_min && diff <= s->l_range_max)
        return 'l';
    return 'e';
}

static void reset_decoder_state(ot_priv *s)
{
    s->state = STATE_IDLE;
    s->edge_count = 0;
    s->bit_count = 0;
}

static void handle_timing_error(struct srd_decoder_inst *di, ot_priv *s,
    uint64_t tss, uint64_t tes)
{
    int64_t us = s2t(s, tes - tss);
    char text[64];
    snprintf(text, sizeof(text), "Timing error (%lld us)", (long long)us);
    c_put(di, tss, tes, s->out_ann, ANN_TIMING,
              text, "Timing", "T");
    s->last_frame_edge = tes;
}

static void handle_bits(struct srd_decoder_inst *di, ot_priv *s)
{
    if (s->bit_count < 1) return;

    /* Compute ss/es for each bit */
    for (int i = 0; i < s->bit_count; i++) {
        s->ss_es_bits[i].ss = s->bits[i].sample;
        if (i < s->bit_count - 1)
            s->ss_es_bits[i].es = s->bits[i + 1].sample;
        else
            s->ss_es_bits[i].es = s->bits[i].sample + s->halfbit * 2;

        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->bits[i].value);
        c_put(di, s->ss_es_bits[i].ss, s->ss_es_bits[i].es, s->out_ann, ANN_BIT, bit_str);
    }

    /* Start bit (bit 0) */
    char start_str[32];
    snprintf(start_str, sizeof(start_str), "Startbit: %d", s->bits[0].value);
    c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[0].es, s->out_ann, ANN_STARTBIT,
              start_str, "STRB", "S");

    if (s->bit_count < 2) {
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[s->bit_count - 1].es,
                  s->out_ann, ANN_WARNING, "Incomplete frame", "Incomplete", "!");
        return;
    }

    /* Parity bit (bit 1) */
    char par_str[32];
    snprintf(par_str, sizeof(par_str), "Paritybit: %d", s->bits[1].value);
    c_put(di, s->ss_es_bits[1].ss, s->ss_es_bits[1].es, s->out_ann, ANN_PARITYBIT,
              par_str, "PB", "P");

    /* Check parity */
    int parity = 0;
    if (s->bit_count == 34) {
        for (int i = 0; i < 32; i++)
            parity ^= s->bits[i + 1].value;
    }
    if (parity == 1 && s->bit_count == 34)
        c_put(di, s->ss_es_bits[1].ss, s->ss_es_bits[1].es,
                  s->out_ann, ANN_WARNING, "Parity error", "PE", "!");

    if (s->bit_count < 5) {
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[s->bit_count - 1].es,
                  s->out_ann, ANN_WARNING, "Incomplete frame", "Incomplete", "!");
        return;
    }

    /* MSG-TYPE (bits 2-4, MSB first) */
    int msg_type_val = 0;
    for (int i = 0; i < 3; i++)
        msg_type_val |= (s->bits[2 + i].value << (2 - i));

    if (msg_type_val < 0 || msg_type_val > 7) msg_type_val = 0;
    const char *mt_dir = msg_type_table[msg_type_val].dir;
    const char *mt_name = msg_type_table[msg_type_val].name;
    const char *mt_short = msg_type_table[msg_type_val].short_name;

    char mt_str[64];
    snprintf(mt_str, sizeof(mt_str), "MSG-TYPE: %s (%d)", mt_name, msg_type_val);
    c_put(di, s->ss_es_bits[2].ss, s->ss_es_bits[4].es, s->out_ann, ANN_MSGTYPE,
              mt_str, mt_short, mt_short);

    /* Direction annotation */
    if (strcmp(mt_dir, "M2S") == 0)
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[s->bit_count - 1].es,
                  s->out_ann, ANN_M2S, "MasterToSlave", mt_dir);
    else if (strcmp(mt_dir, "S2M") == 0)
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[s->bit_count - 1].es,
                  s->out_ann, ANN_S2M, "SlaveToMaster", mt_dir);

    if (s->bit_count < 9) {
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[s->bit_count - 1].es,
                  s->out_ann, ANN_WARNING, "Incomplete frame", "Incomplete", "!");
        return;
    }

    /* SPARE (bits 5-8) */
    int spare_val = 0;
    for (int i = 0; i < 4; i++)
        spare_val |= (s->bits[5 + i].value << (3 - i));
    char spare_str[32];
    snprintf(spare_str, sizeof(spare_str), "Spare: %d", spare_val);
    c_put(di, s->ss_es_bits[5].ss, s->ss_es_bits[8].es, s->out_ann, ANN_SPARE,
              spare_str, "SP", "SP");

    /* DATA-ID (bits 9-16, MSB first) */
    int data_id = 0;
    for (int i = 0; i < 8 && (9 + i) < s->bit_count; i++)
        data_id |= (s->bits[9 + i].value << (7 - i));

    char id_str[64];
    switch (s->format) {
    case 0: snprintf(id_str, sizeof(id_str), "DATA-ID: 0x%02X", data_id); break;
    case 2: snprintf(id_str, sizeof(id_str), "DATA-ID: 0%03o", data_id); break;
    case 3: {
        char btmp[9];
        for (int b = 0; b < 8; b++) btmp[b] = ((data_id >> (7 - b)) & 1) ? '1' : '0';
        btmp[8] = '\0';
        snprintf(id_str, sizeof(id_str), "DATA-ID: %s", btmp);
        break;
    }
    default: snprintf(id_str, sizeof(id_str), "DATA-ID: %d", data_id); break;
    }
    c_put(di, s->ss_es_bits[9].ss, s->ss_es_bits[16].es, s->out_ann, ANN_DATAID,
              id_str, "ID", "ID");

    if (s->bit_count < 33) {
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[s->bit_count - 1].es,
                  s->out_ann, ANN_WARNING, "Incomplete frame", "Incomplete", "!");
        return;
    }

    /* DATA-VALUE (bits 17-32, MSB first) */
    uint16_t data_value = 0;
    for (int i = 0; i < 16; i++)
        data_value |= (s->bits[17 + i].value << (15 - i));

    char val_str[64];
    switch (s->format) {
    case 0: snprintf(val_str, sizeof(val_str), "DATA-VALUE: 0x%04X", data_value); break;
    case 2: snprintf(val_str, sizeof(val_str), "DATA-VALUE: 0%06o", data_value); break;
    case 3: {
        char btmp[17];
        for (int b = 0; b < 16; b++) btmp[b] = ((data_value >> (15 - b)) & 1) ? '1' : '0';
        btmp[16] = '\0';
        snprintf(val_str, sizeof(val_str), "DATA-VALUE: %s", btmp);
        break;
    }
    default: snprintf(val_str, sizeof(val_str), "DATA-VALUE: %d", data_value); break;
    }
    c_put(di, s->ss_es_bits[17].ss, s->ss_es_bits[32].es, s->out_ann, ANN_DATAVALUE,
              val_str, "VAL", "V");

    /* Stop bit (bit 33) */
    if (s->bit_count == 34) {
        char stop_str[32];
        snprintf(stop_str, sizeof(stop_str), "Stopbit: %d", s->bits[33].value);
        c_put(di, s->ss_es_bits[33].ss, s->ss_es_bits[33].es, s->out_ann, ANN_STOPBIT,
                  stop_str, "STPB", "E");

        if (s->bits[33].value != 1)
            c_put(di, s->ss_es_bits[33].ss, s->ss_es_bits[33].es,
                      s->out_ann, ANN_WARNING, "Stop bit should be 1", "Stop err", "!");
    }

    /* Frame annotation */
    if (s->bit_count == 34) {
        char frame_str[128];
        switch (s->format) {
        case 0:
            snprintf(frame_str, sizeof(frame_str), "Frame %s %s(%d) %d/0x%02X %d/0x%04X",
                     mt_dir, mt_name, msg_type_val, data_id, data_id, data_value, data_value);
            break;
        case 2:
            snprintf(frame_str, sizeof(frame_str), "Frame %s %s(%d) 0%03o 0%06o",
                     mt_dir, mt_name, msg_type_val, data_id, data_value);
            break;
        default:
            snprintf(frame_str, sizeof(frame_str), "Frame %s %s(%d) %d %d",
                     mt_dir, mt_name, msg_type_val, data_id, data_value);
            break;
        }
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[33].es, s->out_ann, ANN_FRAME,
                  frame_str, mt_name, "F");

        /* OTX annotation */
        char otx_str[64];
        snprintf(otx_str, sizeof(otx_str), "%s %s", mt_dir, mt_name);
        c_put(di, s->ss_es_bits[0].ss, s->ss_es_bits[33].es, s->out_ann, ANN_OTX,
                  otx_str, mt_short, mt_short);
    }
}

static void ot_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(ot_priv)));
    ot_priv *s = (ot_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ot_priv));
    s->out_ann = -1;
    s->polarity = 0;
    s->format = 1;
    s->bitlen = 1000;
    s->jitter_m = 100;
    s->jitter_p = 150;
    s->m2s_silence_min = 20000;
    s->m2s_silence_max = 800000;
    s->s2m_silence_min = 100000;
    s->m2m_act_max = 1150000;
    s->ignore_glitches = 0;
    s->state = STATE_IDLE;
    s->prev_samplenum_valid = 0;
}

static void ot_start(struct srd_decoder_inst *di)
{
    ot_priv *s = (ot_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "opentherm");

    const char *pol_str = c_opt_str(di, "polarity", "active-low");
    s->polarity = (strcmp(pol_str, "active-low") == 0) ? 0 : 1;

    s->bitlen = c_opt_int(di, "bitlen", 1000);
    s->jitter_m = c_opt_int(di, "jitter_m", 100);
    s->jitter_p = c_opt_int(di, "jitter_p", 150);
    s->m2s_silence_min = c_opt_int(di, "m2s_silence_min", 20000);
    s->m2s_silence_max = c_opt_int(di, "m2s_silence_max", 800000);
    s->s2m_silence_min = c_opt_int(di, "s2m_silence_min", 100000);
    s->m2m_act_max = c_opt_int(di, "m2m_act_max", 1150000);
    s->ignore_glitches = c_opt_int(di, "ignore_glitches", 0);

    const char *fmt_str = c_opt_str(di, "format", "dec");
    if (strcmp(fmt_str, "hex") == 0)
        s->format = 0;
    else if (strcmp(fmt_str, "dec") == 0)
        s->format = 1;
    else if (strcmp(fmt_str, "oct") == 0)
        s->format = 2;
    else if (strcmp(fmt_str, "bin") == 0)
        s->format = 3;
    else
        s->format = 1;
}

static void ot_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    ot_priv *s = (ot_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->timing_set = 0;
    }
}

static void ot_decode(struct srd_decoder_inst *di)
{
    ot_priv *s = (ot_priv *)c_decoder_get_private(di);
    if (s->samplerate == 0)
        return;

    while (1) {
        if (!s->timing_set)
            setup_calc(s);

        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int lvl = c_pin(di, 0);

        if (!s->prev_samplenum_valid) {
            s->prev_samplenum = di_samplenum(di);
            s->prev_lvl = lvl;
            s->prev_samplenum_valid = 1;
            continue;
        }

        /* Glitch filtering */
        if (s->glitchlen > 0 && (di_samplenum(di) - s->prev_samplenum) <= s->glitchlen) {
            char glitch_str[64];
            snprintf(glitch_str, sizeof(glitch_str), "Glitch (%lld us)",
                     (long long)s2t(s, di_samplenum(di) - s->prev_samplenum));
            c_put(di, s->prev_samplenum, di_samplenum(di), s->out_ann, ANN_WARNING,
                      glitch_str, "Glitch", "G");
            s->prev_samplenum = di_samplenum(di);
            s->prev_lvl = lvl;
            continue;
        }

        s->c_samplenum = s->prev_samplenum;
        s->c_lvl = s->prev_lvl;
        s->prev_samplenum = di_samplenum(di);
        s->prev_lvl = lvl;

        if (s->edge_count < 64)
            s->edges[s->edge_count++] = s->c_samplenum;

        /* FSM processing */
        int bit = -1;
        uint64_t bitpos = 0;

        if (s->state == STATE_IDLE) {
            /* Check silence duration */
            if (s->last_frame_edge != 0 && (s->c_samplenum - s->last_frame_edge) < s->halfbit * 4) {
                c_put(di, s->last_frame_edge, s->c_samplenum, s->out_ann, ANN_WARNING,
                          "Sync error: silence too short", "Sync err", "S");
                s->last_frame_edge = s->c_samplenum;
                continue;
            }
            if ((s->polarity == 0 && s->c_lvl == 1) || (s->polarity == 1 && s->c_lvl == 0)) {
                s->state = STATE_SYNC;
            }
            continue;
        }

        /* Classify edge */
        char edge = 'e';
        if (s->edge_count >= 2)
            edge = edge_type(s, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1]);

        switch (s->state) {
        case STATE_SYNC:
            if (edge == 's') {
                s->state = STATE_MID1;
                bit = 1;
                bitpos = s->edges[s->edge_count - 2];
            } else {
                handle_bits(di, s);
                c_put(di, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1],
                          s->out_ann, ANN_WARNING,
                          "Sync error: start bit len error", "Sync err", "S");
                handle_timing_error(di, s, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1]);
                bit = -1;
                reset_decoder_state(s);
            }
            break;
        case STATE_MID1:
            if (edge == 's') {
                s->state = STATE_START1;
                bit = -1;
            } else if (edge == 'l') {
                s->state = STATE_MID0;
                bit = 0;
                bitpos = s->c_samplenum - s->halfbit;
            } else {
                handle_bits(di, s);
                handle_timing_error(di, s, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1]);
                bit = -1;
                reset_decoder_state(s);
            }
            break;
        case STATE_MID0:
            if (edge == 's') {
                s->state = STATE_START0;
                bit = -1;
            } else if (edge == 'l') {
                s->state = STATE_MID1;
                bit = 1;
                bitpos = s->c_samplenum - s->halfbit;
            } else {
                handle_bits(di, s);
                handle_timing_error(di, s, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1]);
                bit = -1;
                reset_decoder_state(s);
            }
            break;
        case STATE_START1:
            if (edge == 's') {
                s->state = STATE_MID1;
                bit = 1;
                bitpos = s->edges[s->edge_count - 2];
            } else {
                handle_bits(di, s);
                handle_timing_error(di, s, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1]);
                bit = -1;
                reset_decoder_state(s);
            }
            break;
        case STATE_START0:
            if (edge == 's') {
                s->state = STATE_MID0;
                bit = 0;
                bitpos = s->edges[s->edge_count - 2];
            } else {
                handle_bits(di, s);
                handle_timing_error(di, s, s->edges[s->edge_count - 2], s->edges[s->edge_count - 1]);
                bit = -1;
                reset_decoder_state(s);
            }
            break;
        default:
            reset_decoder_state(s);
            break;
        }

        if (bit >= 0 && s->bit_count < 34) {
            s->bits[s->bit_count].sample = bitpos;
            s->bits[s->bit_count].value = bit;
            s->bit_count++;
        }

        if (s->bit_count == 34) {
            handle_bits(di, s);
            reset_decoder_state(s);
            s->last_frame_edge = s->c_samplenum;
        }
    }
}

static void ot_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder opentherm_c_decoder = {
    .id = "opentherm_c",
    .name = "OpenTherm(C)",
    .longname = "OpenTherm (C)",
    .desc = "OpenTherm protocol. (C implementation)",
    .license = "gplv2+",
    .channels = ot_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ot_options,
    .num_options = 10,
    .num_annotations = NUM_ANN,
    .ann_labels = ot_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = ot_ann_rows,
    .inputs = ot_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .tags = ot_tags,
    .num_tags = 1,
    .binary = NULL,
    .num_binary = 0,
    .reset = ot_reset,
    .start = ot_start,
    .decode = ot_decode,
    .destroy = ot_destroy,
    .state_size = 0,
    .metadata = ot_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ot_options[0].def = g_variant_new_string("active-low");
    ot_options[1].def = g_variant_new_int64(1000);
    ot_options[2].def = g_variant_new_int64(100);
    ot_options[3].def = g_variant_new_int64(150);
    ot_options[4].def = g_variant_new_int64(20000);
    ot_options[5].def = g_variant_new_int64(800000);
    ot_options[6].def = g_variant_new_int64(100000);
    ot_options[7].def = g_variant_new_int64(1150000);
    ot_options[8].def = g_variant_new_int64(0);
    ot_options[9].def = g_variant_new_string("dec");

    /* Set enum value lists */
    static GSList *polarity_vals = NULL;
    polarity_vals = g_slist_append(polarity_vals, g_variant_new_string("active-low"));
    polarity_vals = g_slist_append(polarity_vals, g_variant_new_string("active-high"));
    ot_options[0].values = polarity_vals;

    static GSList *format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("hex"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("dec"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("oct"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("bin"));
    ot_options[9].values = format_vals;

    return &opentherm_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}