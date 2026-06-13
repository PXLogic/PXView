/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012-2015 Uwe Hermann <uwe@hermann-uwe.de>
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

/* Channel indices — match Python: TDI=0, TDO=1, TCK=2, TMS=3, TRST=4, SRST=5, RTCK=6 */
#define CH_TDI  0
#define CH_TDO  1
#define CH_TCK  2
#define CH_TMS  3
#define CH_TRST 4
#define CH_SRST 5
#define CH_RTCK 6

enum jtag_state {
    TEST_LOGIC_RESET = 0,
    RUN_TEST_IDLE = 1,
    SELECT_DR_SCAN = 2,
    CAPTURE_DR = 3,
    UPDATE_DR = 4,
    PAUSE_DR = 5,
    SHIFT_DR = 6,
    EXIT1_DR = 7,
    EXIT2_DR = 8,
    SELECT_IR_SCAN = 9,
    CAPTURE_IR = 10,
    UPDATE_IR = 11,
    PAUSE_IR = 12,
    SHIFT_IR = 13,
    EXIT1_IR = 14,
    EXIT2_IR = 15,
};

enum jtag_ann {
    ANN_TEST_LOGIC_RESET = 0,
    ANN_RUN_TEST_IDLE,
    ANN_SELECT_DR_SCAN,
    ANN_CAPTURE_DR,
    ANN_UPDATE_DR,
    ANN_PAUSE_DR,
    ANN_SHIFT_DR,
    ANN_EXIT1_DR,
    ANN_EXIT2_DR,
    ANN_SELECT_IR_SCAN,
    ANN_CAPTURE_IR,
    ANN_UPDATE_IR,
    ANN_PAUSE_IR,
    ANN_SHIFT_IR,
    ANN_EXIT1_IR,
    ANN_EXIT2_IR,
    ANN_BIT_TDI,
    ANN_BIT_TDO,
    ANN_BITSTRING_TDI,
    ANN_BITSTRING_TDO,
    NUM_ANN,
};

/* Next-state table: next_state[current_state][tms] */
static const int next_state[16][2] = {
    { 1, 0 },  /* TEST_LOGIC_RESET: tms=0->RUN_TEST_IDLE, tms=1->TEST_LOGIC_RESET */
    { 1, 2 },  /* RUN_TEST_IDLE: tms=0->RUN_TEST_IDLE, tms=1->SELECT_DR_SCAN */
    { 3, 9 },  /* SELECT_DR_SCAN: tms=0->CAPTURE_DR, tms=1->SELECT_IR_SCAN */
    { 6, 7 },  /* CAPTURE_DR: tms=0->SHIFT_DR, tms=1->EXIT1_DR */
    { 1, 2 },  /* UPDATE_DR: tms=0->RUN_TEST_IDLE, tms=1->SELECT_DR_SCAN */
    { 5, 8 },  /* PAUSE_DR: tms=0->PAUSE_DR, tms=1->EXIT2_DR */
    { 6, 7 },  /* SHIFT_DR: tms=0->SHIFT_DR, tms=1->EXIT1_DR */
    { 5, 4 },  /* EXIT1_DR: tms=0->PAUSE_DR, tms=1->UPDATE_DR */
    { 6, 4 },  /* EXIT2_DR: tms=0->SHIFT_DR, tms=1->UPDATE_DR */
    { 10, 0 }, /* SELECT_IR_SCAN: tms=0->CAPTURE_IR, tms=1->TEST_LOGIC_RESET */
    { 13, 14 },/* CAPTURE_IR: tms=0->SHIFT_IR, tms=1->EXIT1_IR */
    { 1, 2 },  /* UPDATE_IR: tms=0->RUN_TEST_IDLE, tms=1->SELECT_DR_SCAN */
    { 12, 15 },/* PAUSE_IR: tms=0->PAUSE_IR, tms=1->EXIT2_IR */
    { 13, 14 },/* SHIFT_IR: tms=0->SHIFT_IR, tms=1->EXIT1_IR */
    { 12, 11 },/* EXIT1_IR: tms=0->PAUSE_IR, tms=1->UPDATE_IR */
    { 13, 11 },/* EXIT2_IR: tms=0->SHIFT_IR, tms=1->UPDATE_IR */
};

static const char *jtag_state_names[] = {
    "TEST-LOGIC-RESET",
    "RUN-TEST/IDLE",
    "SELECT-DR-SCAN",
    "CAPTURE-DR",
    "UPDATE-DR",
    "PAUSE-DR",
    "SHIFT-DR",
    "EXIT1-DR",
    "EXIT2-DR",
    "SELECT-IR-SCAN",
    "CAPTURE-IR",
    "UPDATE-IR",
    "PAUSE-IR",
    "SHIFT-IR",
    "EXIT1-IR",
    "EXIT2-IR",
};

/* Decoder state struct — C_DECODER_STATE auto-generates jtag_s typedef,
 * jtag_reset (calloc), and jtag_destroy (free). We supplement non-zero
 * defaults in jtag_start(). */
C_DECODER_STATE(jtag, {
    int state;
    int oldstate;
    int bits_tdi[256];
    int bits_tdo[256];
    uint64_t bits_ss[256];
    int bits_cnt;
    uint64_t ss_bitstring;
    uint64_t ss_item;
    uint64_t es_item;
    gboolean first;
    gboolean first_shift_bit;
    gboolean data_ready;
    uint64_t ss_state;  /* state annotation start sample */
    int out_ann;
    int out_python;
    int last_bit_tdi;
    int last_bit_tdo;
    uint64_t last_bit_ss;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel jtag_channels[] = {
    { "tdi",  "TDI",  "Test data input",    0, SRD_CHANNEL_SDATA, "dec_jtag_chan_tdi" },
    { "tdo",  "TDO",  "Test data output",   1, SRD_CHANNEL_SDATA, "dec_jtag_chan_tdo" },
    { "tck",  "TCK",  "Test clock",         2, SRD_CHANNEL_SCLK,  "dec_jtag_chan_tck" },
    { "tms",  "TMS",  "Test mode select",   3, SRD_CHANNEL_COMMON, "dec_jtag_chan_tms" },
};

static struct srd_channel jtag_optional_channels[] = {
    { "trst", "TRST#", "Test reset",        4, SRD_CHANNEL_COMMON, "dec_jtag_opt_chan_trst" },
    { "srst", "SRST#", "System reset",       5, SRD_CHANNEL_COMMON, "dec_jtag_opt_chan_srst" },
    { "rtck", "RTCK",  "Return clock signal", 6, SRD_CHANNEL_SCLK,  "dec_jtag_opt_chan_rtck" },
};

/* Annotation labels — match Python annotations tuple */
static const char *jtag_ann_labels[][3] = {
    { "", "test-logic-reset", "TEST-LOGIC-RESET" },
    { "", "run-test/idle",    "RUN-TEST/IDLE" },
    { "", "select-dr-scan",  "SELECT-DR-SCAN" },
    { "", "capture-dr",      "CAPTURE-DR" },
    { "", "update-dr",       "UPDATE-DR" },
    { "", "pause-dr",        "PAUSE-DR" },
    { "", "shift-dr",        "SHIFT-DR" },
    { "", "exit1-dr",        "EXIT1-DR" },
    { "", "exit2-dr",        "EXIT2-DR" },
    { "", "select-ir-scan",  "SELECT-IR-SCAN" },
    { "", "capture-ir",      "CAPTURE-IR" },
    { "", "update-ir",       "UPDATE-IR" },
    { "", "pause-ir",        "PAUSE-IR" },
    { "", "shift-ir",        "SHIFT-IR" },
    { "", "exit1-ir",        "EXIT1-IR" },
    { "", "exit2-ir",        "EXIT2-IR" },
    { "", "bit-tdi",         "Bit (TDI)" },
    { "", "bit-tdo",         "Bit (TDO)" },
    { "", "bitstring-tdi",   "Bitstring (TDI)" },
    { "", "bitstring-tdo",   "Bitstring (TDO)" },
};

/* Annotation row class lists */
static const int jtag_row_bits_tdi_classes[] = { ANN_BIT_TDI, -1 };
static const int jtag_row_bits_tdo_classes[] = { ANN_BIT_TDO, -1 };
static const int jtag_row_bitstrings_tdi_classes[] = { ANN_BITSTRING_TDI, -1 };
static const int jtag_row_bitstrings_tdo_classes[] = { ANN_BITSTRING_TDO, -1 };
static const int jtag_row_states_classes[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -1
};

static const struct srd_c_ann_row jtag_ann_rows[] = {
    { "bits-tdi",         "Bits (TDI)",         jtag_row_bits_tdi_classes, 1 },
    { "bits-tdo",         "Bits (TDO)",         jtag_row_bits_tdo_classes, 1 },
    { "bitstrings-tdi",   "Bitstring (TDI)",    jtag_row_bitstrings_tdi_classes, 1 },
    { "bitstrings-tdo",   "Bitstring (TDO)",    jtag_row_bitstrings_tdo_classes, 1 },
    { "states",           "States",             jtag_row_states_classes, 16 },
};

static const char *jtag_inputs[] = { "logic", NULL };
static const char *jtag_outputs[] = { "jtag", NULL };
static const char *jtag_tags[] = { "Debug/trace", NULL };

/* ---- start callback — match Python: start() ---- */
static void jtag_start(struct srd_decoder_inst *di)
{
    jtag_s *s = (jtag_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "jtag");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "jtag");

    /* Non-zero defaults that calloc didn't set */
    s->state = RUN_TEST_IDLE;
    s->oldstate = RUN_TEST_IDLE;
    s->first = TRUE;
}

/* ---- decode callback — main state machine ---- */
static void jtag_decode(struct srd_decoder_inst *di)
{
    jtag_s *s = (jtag_s *)c_decoder_get_private(di);

    while (1) {
        /* Python: (tdi, tdo, tck, tms, trst, srst, rtck) = self.wait({2: 'r'}) */
        int ret = c_wait(di, CW_R(CH_TCK), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);

        int tms = c_pin(di, CH_TMS);
        int oldstate = s->state;
        int newstate = next_state[oldstate][tms];
        gboolean shifting_to_exit1 = (oldstate == SHIFT_DR && newstate == EXIT1_DR) ||
                                     (oldstate == SHIFT_IR && newstate == EXIT1_IR);

        /* Track first shift bit — Python: first_bit logic */
        if ((newstate == SHIFT_DR && oldstate != SHIFT_DR) ||
            (newstate == SHIFT_IR && oldstate != SHIFT_IR)) {
            s->first_shift_bit = TRUE;
        }

        /* Collect TDI/TDO bits while in SHIFT-DR or SHIFT-IR */
        if (oldstate == SHIFT_DR || oldstate == SHIFT_IR) {
            if (s->first_shift_bit) {
                s->first_shift_bit = FALSE;
                /* Initialize bitstring start on the first shift bit */
                s->ss_bitstring = samplenum;
                s->ss_item = samplenum;
            } else {
                int tdi_val = c_pin(di, CH_TDI);
                int tdo_val = c_pin(di, CH_TDO);

                if (s->bits_cnt < 256) {
                    s->bits_tdi[s->bits_cnt] = tdi_val;
                    s->bits_tdo[s->bits_cnt] = tdo_val;
                    s->bits_ss[s->bits_cnt] = samplenum;
                }

                /* Defer last bit annotation when transitioning to EXIT1 */
                if (!shifting_to_exit1) {
                    char tdi_str[4];
                    char tdo_str[4];
                    snprintf(tdi_str, sizeof(tdi_str), "%d", tdi_val);
                    snprintf(tdo_str, sizeof(tdo_str), "%d", tdo_val);
                    c_put(di, s->ss_item, samplenum, s->out_ann, ANN_BIT_TDI, tdi_str);
                    c_put(di, s->ss_item, samplenum, s->out_ann, ANN_BIT_TDO, tdo_str);
                } else {
                    s->last_bit_tdi = tdi_val;
                    s->last_bit_tdo = tdo_val;
                    s->last_bit_ss = samplenum;
                }

                s->es_item = samplenum;
                s->ss_item = samplenum;
                s->bits_cnt++;
                s->data_ready = TRUE;
            }
        }

        /* EXIT1/EXIT2 -> next transition: output deferred bitstring, protocol, and last bit */
        if ((oldstate == EXIT1_DR || oldstate == EXIT2_DR ||
             oldstate == EXIT1_IR || oldstate == EXIT2_IR) && newstate != oldstate) {
            if (s->data_ready && s->bits_cnt > 0) {
                int is_ir = (oldstate == EXIT1_IR || oldstate == EXIT2_IR);
                int cnt = s->bits_cnt > 256 ? 256 : s->bits_cnt;
                int i;
                uint64_t tdi_val = 0, tdo_val = 0;
                for (i = 0; i < cnt; i++) {
                    tdi_val |= ((uint64_t)s->bits_tdi[i] << i);
                    tdo_val |= ((uint64_t)s->bits_tdo[i] << i);
                }
                const char *dr_ir = is_ir ? "IR" : "DR";

                /* Bitstring annotations */
                char tdi_str[128];
                char tdo_str[128];
                snprintf(tdi_str, sizeof(tdi_str), "%s TDI:  (0x%llx), %d bits",
                    dr_ir, (unsigned long long)tdi_val, cnt);
                snprintf(tdo_str, sizeof(tdo_str), "%s TDO:  (0x%llx), %d bits",
                         dr_ir, (unsigned long long)tdo_val, cnt);
                c_put(di, s->ss_bitstring, samplenum, s->out_ann, ANN_BITSTRING_TDI, tdi_str);
                c_put(di, s->ss_bitstring, samplenum, s->out_ann, ANN_BITSTRING_TDO, tdo_str);

                /* Protocol output with bitstring and per-bit ss/es data.
                 * Binary data format:
                 *   [0..cnt]       = bitstring as null-terminated string (reversed: last-bit-first)
                 *   [cnt+1..]      = per-bit ss/es pairs, each 16 bytes (8B ss LE + 8B es LE) */
                {
                    char bitstring_tdi[257];
                    char bitstring_tdo[257];
                    for (i = 0; i < cnt; i++) {
                        bitstring_tdi[i] = '0' + s->bits_tdi[cnt - 1 - i];
                        bitstring_tdo[i] = '0' + s->bits_tdo[cnt - 1 - i];
                    }
                    bitstring_tdi[cnt] = '\0';
                    bitstring_tdo[cnt] = '\0';

                    int bitstring_len = cnt + 1;
                    int per_bit_size = 16;
                    int data_size = bitstring_len + cnt * per_bit_size;
                    unsigned char *proto_data = (unsigned char *)g_malloc(data_size);

                    /* TDI protocol output */
                    memcpy(proto_data, bitstring_tdi, bitstring_len);
                    int pos = bitstring_len;
                    for (i = 0; i < cnt; i++) {
                        int bit_idx = cnt - 1 - i;
                        uint64_t ss = s->bits_ss[bit_idx];
                        uint64_t es;
                        if (bit_idx == cnt - 1)
                            es = samplenum;
                        else
                            es = s->bits_ss[bit_idx + 1];
                        memcpy(proto_data + pos, &ss, 8);
                        memcpy(proto_data + pos + 8, &es, 8);
                        pos += per_bit_size;
                    }
                    c_proto(di, s->ss_bitstring, samplenum, s->out_python,
                            is_ir ? "IR TDI" : "DR TDI",
                            C_BYTES(proto_data, data_size), C_END);

                    /* TDO protocol output */
                    memcpy(proto_data, bitstring_tdo, bitstring_len);
                    pos = bitstring_len;
                    for (i = 0; i < cnt; i++) {
                        int bit_idx = cnt - 1 - i;
                        uint64_t ss = s->bits_ss[bit_idx];
                        uint64_t es;
                        if (bit_idx == cnt - 1)
                            es = samplenum;
                        else
                            es = s->bits_ss[bit_idx + 1];
                        memcpy(proto_data + pos, &ss, 8);
                        memcpy(proto_data + pos + 8, &es, 8);
                        pos += per_bit_size;
                    }
                    c_proto(di, s->ss_bitstring, samplenum, s->out_python,
                            is_ir ? "IR TDO" : "DR TDO",
                            C_BYTES(proto_data, data_size), C_END);

                    g_free(proto_data);
                }

                /* Output deferred last bit annotation */
                {
                    char tdi_str[4];
                    char tdo_str[4];
                    snprintf(tdi_str, sizeof(tdi_str), "%d", s->last_bit_tdi);
                    snprintf(tdo_str, sizeof(tdo_str), "%d", s->last_bit_tdo);
                    c_put(di, s->last_bit_ss, samplenum, s->out_ann, ANN_BIT_TDI, tdi_str);
                    c_put(di, s->last_bit_ss, samplenum, s->out_ann, ANN_BIT_TDO, tdo_str);
                }

                s->bits_cnt = 0;
                s->first = TRUE;
                s->data_ready = FALSE;
            }
        }

        /* Emit state annotation on every rising TCK edge (except the first),
         * matching Python decoder behavior. Python emits the OLD state
         * annotation from the previous edge to the current edge. */
        if (s->first) {
            s->first = FALSE;
        } else {
            c_put(di, s->ss_state, samplenum, s->out_ann, oldstate,
                  jtag_ann_labels[oldstate][2]);
            c_proto(di, s->ss_state, samplenum, s->out_python,
                    "NEW STATE", C_STR(jtag_state_names[newstate]), C_END);
        }
        s->ss_state = samplenum;

        s->oldstate = oldstate;
        s->state = newstate;
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder jtag_c_def = {
    .id = "jtag_c",
    .name = "JTAG(C)",
    .longname = "Joint Test Action Group (C)",
    .desc = "JTAG protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = jtag_channels,
    .num_channels = 4,
    .optional_channels = jtag_optional_channels,
    .num_optional_channels = 3,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = jtag_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = jtag_ann_rows,
    .inputs = jtag_inputs,
    .num_inputs = 1,
    .outputs = jtag_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = jtag_tags,
    .num_tags = 1,
    .state_size = sizeof(jtag_s),
    .reset = jtag_reset,
    .start = jtag_start,
    .decode = jtag_decode,
    .end = NULL,
    .metadata = NULL,
    .destroy = jtag_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &jtag_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}