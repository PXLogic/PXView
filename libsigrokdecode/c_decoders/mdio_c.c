/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 Elias Oenal <sigrok@eliasoenal.com>
 * Copyright (C) 2019 DreamSourceLab <support@dreamsourcelab.com>
 * Copyright (C) 2025 C port (v4 API)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel indices — match Python: MDC=0, MDIO=1 */
#define CH_MDC  0
#define CH_MDIO 1

enum {
    ANN_BIT_VAL = 0,
    ANN_BIT_NUM,
    ANN_FRAME,
    ANN_FRAME_IDLE,
    ANN_FRAME_ERROR,
    ANN_DECODE,
    NUM_ANN,
};

enum mdio_state {
    STATE_PRE = 0,
    STATE_ST,
    STATE_OP,
    STATE_PRTAD,
    STATE_DEVAD,
    STATE_TA,
    STATE_DATA,
};

C_DECODER_STATE(mdio, {
    int state;
    int bitcount;
    int opcode;
    int clause45;
    int clause45_addr;
    int portad;
    int portad_bits;
    int devad;
    int devad_bits;
    int data;
    int data_bits;
    int ta_invalid;       /* 0=valid, 1=bit1, 2=bit2, 3=both */
    int ta_first_bit;
    char op_invalid[32];
    int is_read;
    int preamble_len;
    uint64_t ss_frame;
    uint64_t ss_frame_field;
    int out_ann;
    int out_python;
    int show_debug_bits;
    int read_edge_falling;
    int illegal_bus;
    uint64_t ss_illegal;
    uint64_t mdiobits_head_mdio;
    uint64_t mdiobits_head_ss;
    uint64_t mdiobits_head_es;
    uint64_t cycle_lengths[48];
    int num_cycle_lengths;
    uint64_t ss_preamble_first;
    uint64_t sample_hist[33];
    int sample_hist_idx;
    int sample_hist_count;
    uint64_t prev_samplenum;
});

static struct srd_channel mdio_channels[] = {
    { "mdc", "MDC", "Clock", 0, SRD_CHANNEL_SCLK, NULL },
    { "mdio", "MDIO", "Data", 1, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option mdio_options[] = {
    { "show_debug_bits", NULL, "Show debug bits", NULL, NULL },
    { "read_edge", NULL, "read edge", NULL, NULL },
};

static const char* mdio_ann_labels[][3] = {
    { "", "bit-val", "Bit value" },
    { "", "bit-num", "Bit number" },
    { "", "frame", "Frame" },
    { "", "frame-idle", "Bus idle state" },
    { "", "frame-error", "Frame error" },
    { "", "decode", "Decode" },
};

static const int mdio_row_bitval_classes[] = { ANN_BIT_VAL };
static const int mdio_row_bitnum_classes[] = { ANN_BIT_NUM };
static const int mdio_row_frame_classes[] = { ANN_FRAME, ANN_FRAME_IDLE };
static const int mdio_row_frame_error_classes[] = { ANN_FRAME_ERROR };
static const int mdio_row_decode_classes[] = { ANN_DECODE };
static const struct srd_c_ann_row mdio_ann_rows[] = {
    { "bit-val", "Bit value", mdio_row_bitval_classes, 1 },
    { "bit-num", "Bit number", mdio_row_bitnum_classes, 1 },
    { "frame", "Frame", mdio_row_frame_classes, 2 },
    { "frame-error", "Frame error", mdio_row_frame_error_classes, 1 },
    { "decode", "Decode", mdio_row_decode_classes, 1 },
};

static const char* mdio_inputs[] = { "logic" };
static const char* mdio_outputs[] = { "mdio" };
static const char* mdio_tags[] = { "Networking" };

static uint64_t mdio_get_sample_hist(mdio_s *s, int back)
{
    if (back < 0 || back >= s->sample_hist_count || back >= 33)
        return 0;
    int idx = (s->sample_hist_idx + 33 - 1 - back) % 33;
    return s->sample_hist[idx];
}

static void mdio_reset_state(mdio_s *s)
{
    s->bitcount = -1;
    s->opcode = -1;
    s->clause45 = 0;
    s->ss_frame = (uint64_t)-1;
    s->ss_frame_field = (uint64_t)-1;
    s->preamble_len = 0;
    s->ta_invalid = 0;
    s->ta_first_bit = 1;
    s->op_invalid[0] = '\0';
    s->portad = -1;
    s->portad_bits = 5;
    s->devad = -1;
    s->devad_bits = 5;
    s->data = -1;
    s->data_bits = 16;
    s->state = STATE_PRE;
    s->is_read = 1;
    s->num_cycle_lengths = 0;
    s->sample_hist_idx = 0;
    s->sample_hist_count = 0;
    s->ss_preamble_first = 0;
}

static void mdio_start(struct srd_decoder_inst *di)
{
    mdio_s *s = (mdio_s *)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mdio");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "mdio");

    const char *debug_str = c_opt_str(di, "show_debug_bits", "no");
    s->show_debug_bits = (debug_str && strcmp(debug_str, "yes") == 0) ? 1 : 0;

    const char *edge_str = c_opt_str(di, "read_edge", "falling");
    s->read_edge_falling = (edge_str && strcmp(edge_str, "falling") == 0) ? 1 : 0;

    /* Non-zero defaults that calloc didn't set */
    s->clause45_addr = -1;
    s->ss_frame = (uint64_t)-1;
    s->ss_frame_field = (uint64_t)-1;
    mdio_reset_state(s);
}

static void mdio_putbit(struct srd_decoder_inst *di, mdio_s *s,
    int mdio_val, uint64_t ss, uint64_t es)
{
    char val_str[4];
    snprintf(val_str, sizeof(val_str), "%d", mdio_val);
    c_put(di, ss, es, s->out_ann, ANN_BIT_VAL, val_str);

    if (s->show_debug_bits) {
        char num_str[16];
        char num_short[4];
        snprintf(num_str, sizeof(num_str), "%d", s->bitcount - 1);
        snprintf(num_short, sizeof(num_short), "%d", (s->bitcount - 1) % 10);
        c_put(di, ss, es, s->out_ann, ANN_BIT_NUM, num_str, num_short);
    }
}

static uint64_t mdio_quartile_cycle_length(mdio_s *s)
{
    if (s->num_cycle_lengths < 1)
        return 1;
    int count = s->num_cycle_lengths;
    uint64_t sorted[48];
    memcpy(sorted, s->cycle_lengths, sizeof(uint64_t) * count);
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (sorted[j] < sorted[i]) {
                uint64_t tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    int idx = count / 4;
    if (idx < 0) idx = 0;
    if (idx >= count) idx = count - 1;
    return sorted[idx] > 0 ? sorted[idx] : 1;
}

static void mdio_putdata(struct srd_decoder_inst *di, mdio_s *s)
{
    char data_str[32];
    snprintf(data_str, sizeof(data_str), "DATA: %04X", s->data);
    c_put(di, s->ss_frame_field, s->mdiobits_head_es, s->out_ann, ANN_FRAME,
        data_str, "DATA", "D");

    if (s->clause45 && s->opcode == 0) {
        s->clause45_addr = s->data;
    }

    if (s->opcode > 0 || !s->clause45) {
        char decoded_min[128] = { 0 };
        int pos = 0;

        if (s->clause45 && s->clause45_addr != -1) {
            pos += snprintf(decoded_min + pos, sizeof(decoded_min) - pos,
                "ADDR: %04X ", s->clause45_addr);
        } else if (s->clause45) {
            pos += snprintf(decoded_min + pos, sizeof(decoded_min) - pos,
                "ADDR: UKWN ");
        }

        int is_read = 0;
        if ((s->clause45 && s->opcode > 1) || (!s->clause45 && s->opcode)) {
            pos += snprintf(decoded_min + pos, sizeof(decoded_min) - pos,
                "READ:  %04X", s->data);
            is_read = 1;
        } else {
            pos += snprintf(decoded_min + pos, sizeof(decoded_min) - pos,
                "WRITE: %04X", s->data);
            is_read = 0;
        }

        char decoded_ext[128] = { 0 };
        int epos = 0;
        epos += snprintf(decoded_ext + epos, sizeof(decoded_ext) - epos,
            " %s: %02d",
            s->clause45 ? "PRTAD" : "PHYAD", s->portad);
        epos += snprintf(decoded_ext + epos, sizeof(decoded_ext) - epos,
            " %s: %02d",
            s->clause45 ? "DEVAD" : "REGAD", s->devad);
        if (s->ta_invalid || s->op_invalid[0]) {
            epos += snprintf(decoded_ext + epos, sizeof(decoded_ext) - epos, " ERROR");
        }

        char full[256];
        snprintf(full, sizeof(full), "%s%s", decoded_min, decoded_ext);
        c_put(di, s->ss_frame, s->mdiobits_head_es, s->out_ann, ANN_DECODE,
            full, decoded_min);

        /* Protocol output */
        c_proto(di, s->ss_frame, s->mdiobits_head_es, s->out_python, "DATA",
                C_U8(s->clause45), C_I32(s->clause45_addr),
                C_U8(is_read), C_U8(s->portad), C_U8(s->devad),
                C_U16(s->data), C_END);
    }

    if (s->clause45 && s->opcode == 2 && s->clause45_addr != -1) {
        s->clause45_addr += 1;
    }
}

static void mdio_state_PRE(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    if (s->illegal_bus) {
        if (mdio_val == 0) {
            return;
        } else {
            s->illegal_bus = 0;
            c_put(di, s->ss_illegal, samplenum, s->out_ann, ANN_FRAME_ERROR,
                "ILLEGAL BUS STATE", "ILL");
            s->ss_frame = samplenum;
        }
    }

    if (s->ss_frame == (uint64_t)-1) {
        s->ss_frame = samplenum;
    }

    if (mdio_val == 1) {
        if (s->preamble_len == 0)
            s->ss_preamble_first = samplenum;
        s->preamble_len += 1;
    }

    if (s->preamble_len > 16) {
        if (s->preamble_len >= 10000 + 32) {
            uint64_t ss_32 = mdio_get_sample_hist(s, 32);
            char idle_str[32];
            snprintf(idle_str, sizeof(idle_str), "IDLE #%d", s->preamble_len - 32);
            c_put(di, s->ss_frame, ss_32, s->out_ann, ANN_FRAME_IDLE,
                idle_str, "IDLE", "I");
            s->ss_frame = ss_32;
            s->preamble_len = 32;
        }
        if (mdio_val == 0) {
            if (s->preamble_len < 32) {
                s->ss_frame = s->ss_preamble_first;
                c_put(di, s->ss_frame, samplenum, s->out_ann, ANN_FRAME_ERROR,
                    "SHORT PREAMBLE", "SHRT PRE");
            } else if (s->preamble_len > 32) {
                uint64_t ss_32 = mdio_get_sample_hist(s, 32);
                s->ss_frame = ss_32;
                char idle_str[32];
                snprintf(idle_str, sizeof(idle_str), "IDLE #%d", s->preamble_len - 32);
                c_put(di, s->ss_preamble_first, ss_32, s->out_ann, ANN_FRAME_IDLE,
                    idle_str, "IDLE", "I");
                s->preamble_len = 32;
            } else {
                uint64_t ss_32 = mdio_get_sample_hist(s, 32);
                s->ss_frame = ss_32;
            }
            char pre_str[32];
            snprintf(pre_str, sizeof(pre_str), "PRE #%d", s->preamble_len);
            c_put(di, s->ss_frame, samplenum, s->out_ann, ANN_FRAME,
                pre_str, "PRE", "P");
            s->ss_frame_field = samplenum;
            s->state = STATE_ST;
        }
    } else if (mdio_val == 0) {
        s->ss_illegal = s->ss_frame;
        s->illegal_bus = 1;
    }
}

static void mdio_state_ST(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    (void)di;
    (void)samplenum;
    if (mdio_val == 0) {
        s->clause45 = 1;
    }
    s->state = STATE_OP;
}

static void mdio_state_OP(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    if (s->opcode == -1) {
        if (s->clause45) {
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, "ST (Clause 45)", "ST 45", "ST", "S");
        } else {
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, "ST (Clause 22)", "ST 22", "ST", "S");
        }
        s->ss_frame_field = samplenum;

        if (mdio_val) {
            s->opcode = 2;
        } else {
            s->opcode = 0;
        }
    } else {
        if (s->clause45) {
            s->opcode += mdio_val;
            s->state = STATE_PRTAD;
        } else {
            if (mdio_val == s->opcode) {
                strncpy(s->op_invalid, "invalid for Clause 22", sizeof(s->op_invalid) - 1);
                s->op_invalid[sizeof(s->op_invalid) - 1] = '\0';
            }
            s->state = STATE_PRTAD;
        }
    }
}

static void mdio_state_PRTAD(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    if (s->portad == -1) {
        s->portad = 0;
        const char *op_long = "";
        const char *op_short = "";
        if (s->clause45) {
            if (s->opcode == 0) {
                op_long = "OP: ADDR"; op_short = "OP: A";
            } else if (s->opcode == 1) {
                op_long = "OP: WRITE"; op_short = "OP: W"; s->is_read = 0;
            } else if (s->opcode == 2) {
                op_long = "OP: READINC"; op_short = "OP: RI"; s->is_read = 1;
            } else if (s->opcode == 3) {
                op_long = "OP: READ"; op_short = "OP: R"; s->is_read = 1;
            }
        } else {
            if (s->opcode) {
                op_long = "OP: READ"; op_short = "OP: R"; s->is_read = 1;
            } else {
                op_long = "OP: WRITE"; op_short = "OP: W"; s->is_read = 0;
            }
        }
        c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, op_long, op_short, "OP", "O");
        if (s->op_invalid[0]) {
            char op_err[64];
            snprintf(op_err, sizeof(op_err), "OP %s", s->op_invalid);
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME_ERROR, op_err, "OP", "O");
        }
        s->ss_frame_field = samplenum;
    }

    s->portad_bits--;
    s->portad |= mdio_val << s->portad_bits;
    if (!s->portad_bits) {
        s->state = STATE_DEVAD;
    }
}

static void mdio_state_DEVAD(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    if (s->devad == -1) {
        s->devad = 0;
        char prtad_str[32];
        if (s->clause45) {
            snprintf(prtad_str, sizeof(prtad_str), "PRTAD: %02d", s->portad);
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, prtad_str, "PRT", "P");
        } else {
            snprintf(prtad_str, sizeof(prtad_str), "PHYAD: %02d", s->portad);
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, prtad_str, "PHY", "P");
        }
        s->ss_frame_field = samplenum;
    }
    s->devad_bits--;
    s->devad |= mdio_val << s->devad_bits;
    if (!s->devad_bits) {
        s->state = STATE_TA;
    }
}

static void mdio_state_TA(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    if (s->ta_first_bit) {
        s->ta_first_bit = 0;
        char regad_str[32];
        if (s->clause45) {
            snprintf(regad_str, sizeof(regad_str), "DEVAD: %02d", s->devad);
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, regad_str, "DEV", "D");
        } else {
            snprintf(regad_str, sizeof(regad_str), "REGAD: %02d", s->devad);
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, regad_str, "REG", "R");
        }
        s->ss_frame_field = samplenum;
        if (mdio_val != 1 && ((s->clause45 && s->opcode < 2) || (!s->clause45 && s->opcode == 0))) {
            s->ta_invalid = 1;
        }
    } else {
        if (mdio_val != 0) {
            if (s->ta_invalid)
                s->ta_invalid = 3;
            else
                s->ta_invalid = 2;
        }
        s->state = STATE_DATA;
    }
}

static void mdio_state_DATA(struct srd_decoder_inst *di, mdio_s *s, int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    if (s->data == -1) {
        s->data = 0;
        c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME, "TA", "T");
        if (s->ta_invalid) {
            const char *ta_str = "";
            if (s->ta_invalid == 3) {
                ta_str = "TA invalid (bit1 and bit2)";
            } else if (s->ta_invalid == 2) {
                ta_str = "TA invalid (bit2)";
            } else {
                ta_str = "TA invalid (bit1)";
            }
            c_put(di, s->ss_frame_field, s->prev_samplenum, s->out_ann, ANN_FRAME_ERROR, ta_str, "TA", "T");
        }
        s->ss_frame_field = samplenum;
    }
    s->data_bits--;
    s->data |= mdio_val << s->data_bits;
    if (!s->data_bits) {
        s->mdiobits_head_es = s->mdiobits_head_ss + mdio_quartile_cycle_length(s);
        s->bitcount++;
        mdio_putbit(di, s, s->mdiobits_head_mdio, s->mdiobits_head_ss, s->mdiobits_head_es);
        mdio_putdata(di, s);
        mdio_reset_state(s);
    }
}

static void mdio_handle_bit(struct srd_decoder_inst *di, mdio_s *s,
    int mdio_val, uint64_t samplenum)
{
    (void)samplenum;
    s->sample_hist[s->sample_hist_idx] = samplenum;
    s->sample_hist_idx = (s->sample_hist_idx + 1) % 33;
    if (s->sample_hist_count < 33)
        s->sample_hist_count++;

    if (s->bitcount > 0 && s->num_cycle_lengths < 48) {
        s->cycle_lengths[s->num_cycle_lengths++] = samplenum - s->mdiobits_head_ss;
    }

    if (s->bitcount >= 0) {
        mdio_putbit(di, s, s->mdiobits_head_mdio, s->mdiobits_head_ss, samplenum);
    }

    s->mdiobits_head_mdio = mdio_val;
    s->mdiobits_head_ss = samplenum;
    s->bitcount++;

    s->prev_samplenum = samplenum;

    switch (s->state) {
    case STATE_PRE:
        mdio_state_PRE(di, s, mdio_val, samplenum);
        break;
    case STATE_ST:
        mdio_state_ST(di, s, mdio_val, samplenum);
        break;
    case STATE_OP:
        mdio_state_OP(di, s, mdio_val, samplenum);
        break;
    case STATE_PRTAD:
        mdio_state_PRTAD(di, s, mdio_val, samplenum);
        break;
    case STATE_DEVAD:
        mdio_state_DEVAD(di, s, mdio_val, samplenum);
        break;
    case STATE_TA:
        mdio_state_TA(di, s, mdio_val, samplenum);
        break;
    case STATE_DATA:
        mdio_state_DATA(di, s, mdio_val, samplenum);
        break;
    }
}

static void mdio_decode(struct srd_decoder_inst *di)
{
    mdio_s *s = (mdio_s *)c_decoder_get_private(di);
    int use_falling = 0;

    while (1) {
        int ret;
        if (use_falling) {
            ret = c_wait(di, CW_F(CH_MDC), CW_END);
        } else {
            ret = c_wait(di, CW_R(CH_MDC), CW_END);
        }
        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);
        int mdio_val = c_pin(di, CH_MDIO);
        mdio_handle_bit(di, s, mdio_val, samplenum);

        if (s->state == STATE_DATA && s->is_read && s->read_edge_falling) {
            use_falling = 1;
        } else {
            use_falling = 0;
        }
    }
}

static struct srd_c_decoder mdio_c_def = {
    .id = "mdio_c",
    .name = "MDIO(C)",
    .longname = "Management Data Input/Output (C)",
    .desc = "MII management bus between MAC and PHY (C implementation)",
    .license = "bsd",
    .channels = mdio_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = mdio_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = mdio_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = mdio_ann_rows,
    .inputs = mdio_inputs,
    .num_inputs = 1,
    .outputs = mdio_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = mdio_tags,
    .num_tags = 1,
    .state_size = sizeof(mdio_s),
    .reset = mdio_reset,
    .start = mdio_start,
    .decode = mdio_decode,
    .destroy = mdio_destroy,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    mdio_options[0].def = g_variant_new_string("no");
    GSList* debug_vals = NULL;
    debug_vals = g_slist_append(debug_vals, g_variant_new_string("yes"));
    debug_vals = g_slist_append(debug_vals, g_variant_new_string("no"));
    mdio_options[0].values = debug_vals;

    mdio_options[1].def = g_variant_new_string("falling");
    GSList* edge_vals = NULL;
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("rising"));
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("falling"));
    mdio_options[1].values = edge_vals;

    return &mdio_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}