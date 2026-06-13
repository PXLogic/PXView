/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 Daniel Schulte <trilader@schroedingers-bit.net>
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

/* Channel indices — match Python: CLK=0, DATA=1 */
#define CLK  0
#define DATA 1

/* Annotation class indices — match Python Ann class */
enum ps2_ann {
    ANN_BIT = 0,
    ANN_HSTART,
    ANN_DSTART,
    ANN_STOP,
    ANN_PARITY_OK,
    ANN_PARITY_ERR,
    ANN_DATA_BIT,
    ANN_WORD,
    ANN_ACK,
    ANN_ATK_DATA_POINT,
    NUM_ANN,
};

/* State machine — match Python self.state */
enum ps2_state {
    STATE_IDLE,
    STATE_HtoD_DATA,
    STATE_HtoD_ACK_WAIT,
    STATE_HtoD_ACK,
    STATE_DtoH_DATA,
    STATE_DtoH_NEXT,
};

/* Decoder state struct — C_DECODER_STATE auto-generates ps2_s typedef,
 * ps2_reset (calloc), and ps2_destroy (free). */
C_DECODER_STATE(ps2, {
    enum ps2_state state;
    int bitcount;
    int bits[12];
    uint64_t bit_ss[12];
    uint8_t byte_val;
    int htd;
    int htd_clock;
    int dth_clock;
    int htd_next;  /* HtoDss flag: after DtoH with data-fall, next word is HtoD */
    int out_ann;
    int out_python;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel ps2_channels[] = {
    {"clk",  "CLK",  "Clock line", 0, SRD_CHANNEL_SCLK,  "dec_ps2_chan_clk"},
    {"data", "DATA", "Data line",  1, SRD_CHANNEL_SDATA, "dec_ps2_chan_data"},
};

/* Options — match Python options tuple */
static struct srd_decoder_option ps2_options[] = {
    {"HtoD_Clock", "dec_ps2_opt_HtoD_Clock", "HtoD_Clock", NULL, NULL},
    {"DtoH_Clock", "dec_ps2_opt_DtoH_Clock", "DtoH_Clock", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *ps2_ann_labels[][3] = {
    {"207", "bit",       "Bit"},
    {"109", "HSTART",    "HSTART"},
    {"50",  "DSTART",    "DSTART"},
    {"1000","stop-bit",  "Stop bit"},
    {"7",   "parity-ok", "Parity OK bit"},
    {"1000","parity-err","Parity error bit"},
    {"40",  "data-bit",  "Data bit"},
    {"65",  "word",      "Word"},
    {"75",  "ACK",       "ACK"},
    {"90",  "atk-data-point", "ATK Data point"},
};

/* Annotation row class lists */
static const int ps2_row_bits_classes[] = {ANN_BIT, -1};
static const int ps2_row_fields_classes[] = {ANN_HSTART, ANN_DSTART, ANN_STOP, ANN_PARITY_OK, ANN_PARITY_ERR, ANN_DATA_BIT, ANN_WORD, ANN_ACK, -1};
static const int ps2_row_atk_classes[] = {ANN_ATK_DATA_POINT, -1};

static const struct srd_c_ann_row ps2_ann_rows[] = {
    {"bits",      "Bits",      ps2_row_bits_classes,   1},
    {"fields",    "Fields",    ps2_row_fields_classes,  8},
    {"atk-signs", "ATK signs", ps2_row_atk_classes,    1},
};

static const char *ps2_inputs[] = {"logic", NULL};
static const char *ps2_outputs[] = {"ps2", NULL};
static const char *ps2_tags[] = {"PC", NULL};

/* Helper: handle complete byte — match Python handle_bits() when bitcount==11 */
static void ps2_handle_byte(struct srd_decoder_inst *di, ps2_s *s)
{
    int i;

    /* Calculate bitwidth for annotation boundaries */
    uint64_t bitwidth = s->bit_ss[2] - s->bit_ss[1];
    uint64_t half_bitwidth = bitwidth / 2;

    /* Per-bit annotations — match Python:
     *   for i in range(11): self.putb(i, Ann.BIT) */
    for (i = 0; i < 11; i++) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->bits[i]);
        uint64_t es;
        if (i < 10)
            es = s->bit_ss[i + 1];
        else
            es = s->bit_ss[i] + half_bitwidth;
        c_put(di, s->bit_ss[i], es, s->out_ann, ANN_BIT, bit_str);
    }

    /* Start bit annotation — match Python:
     *   if self.state == 'HtoD': putx(0, [Ann.HSTART, ...])
     *   if self.state == 'DtoH': putx(0, [Ann.DSTART, ...]) */
    if (s->htd) {
        c_put(di, s->bit_ss[0], s->bit_ss[1], s->out_ann, ANN_HSTART,
              "Host Start", "HStart", "HS");
    } else {
        c_put(di, s->bit_ss[0], s->bit_ss[1], s->out_ann, ANN_DSTART,
              "Device Start", "Device", "DS");
    }

    /* Extract data word — match Python:
     *   word = 0; for i in range(8): word |= (self.bits[i + 1].val << i) */
    s->byte_val = 0;
    for (i = 0; i < 8; i++)
        s->byte_val |= (s->bits[i + 1] << i);

    /* Word annotation — match Python:
     *   self.put(bits[1].ss, bits[8].es, ..., [Ann.WORD, ...]) */
    {
        char word_long[16], word_mid[16], word_short[16];
        snprintf(word_long, sizeof(word_long), "Data: %02x", s->byte_val);
        snprintf(word_mid, sizeof(word_mid), "D: %02x", s->byte_val);
        snprintf(word_short, sizeof(word_short), "%02x", s->byte_val);
        c_put(di, s->bit_ss[1], s->bit_ss[9], s->out_ann, ANN_WORD,
              word_long, word_mid, word_short);
    }

    /* Parity check — match Python:
     *   parity_ok = (bin(word).count('1') + self.bits[9].val) % 2 == 1 */
    {
        int ones = 0;
        for (i = 0; i < 8; i++) {
            if (s->byte_val & (1 << i))
                ones++;
        }
        ones += s->bits[9];
        int parity_ok = (ones % 2 == 1);

        if (parity_ok) {
            c_put(di, s->bit_ss[9], s->bit_ss[10], s->out_ann, ANN_PARITY_OK,
                  "Parity OK", "Par OK", "P");
        } else {
            c_put(di, s->bit_ss[9], s->bit_ss[10], s->out_ann, ANN_PARITY_ERR,
                  "Parity error", "Par err", "PE");
        }
    }

    /* Stop bit annotation — match Python:
     *   self.putx(10, [Ann.STOP, ...]) */
    c_put(di, s->bit_ss[10], s->bit_ss[10] + half_bitwidth, s->out_ann, ANN_STOP,
          "Stop bit", "Stop", "St", "T");

    /* Protocol output for stacked decoders — match Python:
     *   pkt = Ps2Packet(val=word, host=host, pok=parity_ok, ack=False)
     *   self.put(bits[0].ss, bits[10].es, self.out_python, pkt) */
    {
        int ones = 0;
        for (i = 0; i < 8; i++) {
            if (s->byte_val & (1 << i))
                ones++;
        }
        ones += s->bits[9];
        int parity_ok = (ones % 2 == 1);

        c_proto(di, s->bit_ss[0], s->bit_ss[10] + half_bitwidth, s->out_python,
                "BYTE",
                C_U8(s->byte_val),
                C_U8(s->htd ? 1 : 0),
                C_U8(parity_ok ? 1 : 0),
                C_U8(s->htd ? 1 : 0), C_END);
    }
}

/* start callback — match Python start() */
static void ps2_start(struct srd_decoder_inst *di)
{
    ps2_s *s = (ps2_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "ps2");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "ps2");

    const char *htod_str = c_opt_str(di, "HtoD_Clock", "rise");
    const char *dtoh_str = c_opt_str(di, "DtoH_Clock", "fall");

    s->htd_clock = (strcmp(htod_str, "rise") == 0) ? 0 : 1;
    s->dth_clock = (strcmp(dtoh_str, "fall") == 0) ? 1 : 0;
}

/* decode callback — main state machine, match Python decode() */
static void ps2_decode(struct srd_decoder_inst *di)
{
    ps2_s *s = (ps2_s *)c_decoder_get_private(di);
    int ret;

    while (1) {
        switch (s->state) {

        case STATE_IDLE: {
            if (s->htd_next) {
                /* After DtoH with data-fall, next word is HtoD.
                 * Python: (clock_pin, data_pin) = self.wait({0: 'r', 1: 'l'}) */
                ret = c_wait(di, CW_R(CLK), CW_L(DATA), CW_END);
                if (ret != SRD_OK)
                    return;

                s->htd = 1;
                s->htd_next = 0;
                s->state = STATE_HtoD_DATA;
                s->bitcount = 0;
                memset(s->bits, 0, sizeof(s->bits));
                memset(s->bit_ss, 0, sizeof(s->bit_ss));
                {
                    int data_val = c_pin(di, DATA);
                    s->bits[0] = data_val;
                    s->bit_ss[0] = di_samplenum(di);
                    s->bitcount = 1;
                }
            } else {
                /* Match Python's 4 conditions:
                 * {0: 'f', 1: 'r'} - condition 0: skip (fall CLK + rise DATA)
                 * {0: 'f', 1: 'f'} - condition 1: HtoD (fall CLK + fall DATA)
                 * {0: 'f', 1: 'h'} - condition 2: HtoD (fall CLK + high DATA)
                 * {0: 'f', 1: 'l'} - condition 3: DtoH (fall CLK + low DATA)
                 */
                ret = c_wait(di, CW_F(CLK), CW_R(DATA), CW_OR,
                             CW_F(CLK), CW_F(DATA), CW_OR,
                             CW_F(CLK), CW_H(DATA), CW_OR,
                             CW_F(CLK), CW_L(DATA), CW_END);
                if (ret != SRD_OK)
                    return;

                uint64_t matched = di_matched(di);

                if (matched & (1ULL << 0)) {
                    /* fall CLK + rise DATA -> skip */
                } else if (matched & (1ULL << 1)) {
                    /* fall CLK + fall DATA -> HtoD */
                    s->htd = 1;
                    s->state = STATE_HtoD_DATA;
                    s->bitcount = 0;
                    memset(s->bits, 0, sizeof(s->bits));
                    memset(s->bit_ss, 0, sizeof(s->bit_ss));
                    {
                        int data_val = c_pin(di, DATA);
                        s->bits[0] = data_val;
                        s->bit_ss[0] = di_samplenum(di);
                        s->bitcount = 1;
                    }
                } else if (matched & (1ULL << 2)) {
                    /* fall CLK + high DATA -> HtoD */
                    s->htd = 1;
                    s->state = STATE_HtoD_DATA;
                    s->bitcount = 0;
                    memset(s->bits, 0, sizeof(s->bits));
                    memset(s->bit_ss, 0, sizeof(s->bit_ss));
                    {
                        int data_val = c_pin(di, DATA);
                        s->bits[0] = data_val;
                        s->bit_ss[0] = di_samplenum(di);
                        s->bitcount = 1;
                    }
                } else if (matched & (1ULL << 3)) {
                    /* fall CLK + low DATA -> DtoH */
                    s->htd = 0;
                    s->state = STATE_DtoH_DATA;
                    s->bitcount = 0;
                    memset(s->bits, 0, sizeof(s->bits));
                    memset(s->bit_ss, 0, sizeof(s->bit_ss));
                    {
                        int data_val = c_pin(di, DATA);
                        s->bits[0] = data_val;
                        s->bit_ss[0] = di_samplenum(di);
                        s->bitcount = 1;
                    }
                }
            }
            break;
        }

        case STATE_HtoD_DATA: {
            /* Python: if self.HtoD: self.wait({0: 'r'}) else: self.wait({0: 'f'}) */
            if (s->htd_clock == 0)
                ret = c_wait(di, CW_R(CLK), CW_END);
            else
                ret = c_wait(di, CW_F(CLK), CW_END);
            if (ret != SRD_OK)
                return;

            {
                int data_val = c_pin(di, DATA);
                if (s->bitcount < 12) {
                    s->bits[s->bitcount] = data_val;
                    s->bit_ss[s->bitcount] = di_samplenum(di);
                }
                s->bitcount++;

                if (s->bitcount == 10)
                    s->state = STATE_HtoD_ACK_WAIT;
            }
            break;
        }

        case STATE_HtoD_ACK_WAIT: {
            /* Python: (clock_pin, data_pin) = self.wait({0: 'r'}) */
            ret = c_wait(di, CW_R(CLK), CW_END);
            if (ret != SRD_OK)
                return;

            {
                int data_val = c_pin(di, DATA);
                if (s->bitcount < 12) {
                    s->bits[s->bitcount] = data_val;
                    s->bit_ss[s->bitcount] = di_samplenum(di);
                }
                s->bitcount++;

                if (s->bitcount == 11) {
                    ps2_handle_byte(di, s);
                    s->state = STATE_HtoD_ACK;
                }
            }
            break;
        }

        case STATE_HtoD_ACK: {
            /* Python: (clock_pin, data_pin) = self.wait({0: 'f'}) */
            uint64_t ack_ss = di_samplenum(di);
            ret = c_wait(di, CW_F(CLK), CW_END);
            if (ret != SRD_OK)
                return;

            {
                int data_val = c_pin(di, DATA);
                if (s->bitcount < 12) {
                    s->bits[s->bitcount] = data_val;
                    s->bit_ss[s->bitcount] = di_samplenum(di);
                }
                s->bitcount++;
            }

            /* Python: (clock_pin, data_pin) = self.wait({0: 'r'}) */
            {
                ret = c_wait(di, CW_R(CLK), CW_END);
                if (ret != SRD_OK)
                    return;

                c_put(di, ack_ss, di_samplenum(di), s->out_ann, ANN_ACK,
                      "ACK", "ACK", "A");
            }

            s->state = STATE_IDLE;
            s->htd = 0;
            break;
        }

        case STATE_DtoH_DATA: {
            /* Python: if self.DtoH: self.wait({0: 'f'}) else: self.wait({0: 'r'}) */
            if (s->dth_clock == 1)
                ret = c_wait(di, CW_F(CLK), CW_END);
            else
                ret = c_wait(di, CW_R(CLK), CW_END);
            if (ret != SRD_OK)
                return;

            {
                int data_val = c_pin(di, DATA);
                if (s->bitcount < 12) {
                    s->bits[s->bitcount] = data_val;
                    s->bit_ss[s->bitcount] = di_samplenum(di);
                }
                s->bitcount++;

                if (s->bitcount == 11) {
                    ps2_handle_byte(di, s);
                    s->state = STATE_DtoH_NEXT;
                }
            }
            break;
        }

        case STATE_DtoH_NEXT: {
            /* Python: (clock_pin, data_pin) = self.wait([{1: 'f'}, {0: 'r'}])
             * Condition 0: data falling -> next is HtoD (HtoDss)
             * Condition 1: clock rising -> next is normal detection */
            ret = c_wait(di, CW_F(DATA), CW_OR, CW_R(CLK), CW_END);
            if (ret != SRD_OK)
                return;

            uint64_t matched = di_matched(di);

            if (matched & (1ULL << 0)) {
                /* Data fell after DtoH word — next word will be HtoD (HtoDss) */
                s->htd_next = 1;
                s->htd = 1;
                s->state = STATE_IDLE;
            } else if (matched & (1ULL << 1)) {
                /* CLK rose after DtoH word — next word normal detection */
                s->htd_next = 0;
                s->htd = 0;
                s->state = STATE_IDLE;
            }
            break;
        }
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder ps2_c_def = {
    .id = "ps2_c",
    .name = "PS/2(C)",
    .longname = "PS/2 keyboard/mouse (C)",
    .desc = "PS/2 keyboard/mouse protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = ps2_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ps2_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = ps2_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ps2_ann_rows,
    .inputs = ps2_inputs,
    .num_inputs = 1,
    .outputs = ps2_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ps2_tags,
    .num_tags = 1,
    .state_size = sizeof(ps2_s),
    .reset = ps2_reset,
    .start = ps2_start,
    .decode = ps2_decode,
    .end = NULL,
    .metadata = NULL,
    .destroy = ps2_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ps2_options[0].def = g_variant_new_string("rise");
    GSList *htod_vals = NULL;
    htod_vals = g_slist_append(htod_vals, g_variant_new_string("rise"));
    htod_vals = g_slist_append(htod_vals, g_variant_new_string("fall"));
    ps2_options[0].values = htod_vals;

    ps2_options[1].def = g_variant_new_string("fall");
    GSList *dtoh_vals = NULL;
    dtoh_vals = g_slist_append(dtoh_vals, g_variant_new_string("fall"));
    dtoh_vals = g_slist_append(dtoh_vals, g_variant_new_string("rise"));
    ps2_options[1].values = dtoh_vals;

    return &ps2_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}