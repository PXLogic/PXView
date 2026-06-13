/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2014 Angus Gratton <gus@projectgus.com>
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

/* Channel indices — match Python: SWCLK=0, SWDIO=1 */
#define CH_SWCLK 0
#define CH_SWDIO 1

#define RISING  1
#define FALLING 0

#define ADDR_DP_SELECT  0x8
#define ADDR_DP_CTRLSTAT 0x4

enum swd_state {
    UNKNOWN = 0,
    REQ = 1,
    ACK = 2,
    DATA = 3,
    DPARITY = 4,
};

enum swd_ann {
    ANN_RESET = 0,
    ANN_ENABLE,
    ANN_READ,
    ANN_WRITE,
    ANN_ACK,
    ANN_DATA,
    ANN_PARITY,
    NUM_ANN,
};

/* Decoder state struct — C_DECODER_STATE auto-generates swd_s typedef,
 * swd_reset (calloc), and swd_destroy (free). We supplement non-zero
 * defaults in swd_start(). */
C_DECODER_STATE(swd, {
    int state;
    int sample_edge;
    int turnaround;
    int ack;
    uint64_t ss_req;
    char bits[128];
    int bits_len;
    uint64_t samplenums[128];
    int linereset_count;
    uint32_t data;
    int addr;
    int rw;       /* 0=W, 1=R */
    int apdp;     /* 0=DP, 1=AP */
    int ctrlsel;
    int orundetect;
    int dparity;
    int out_ann;
    int out_python;
    uint64_t ss_linereset;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel swd_channels[] = {
    { "swclk", "SWCLK", "Master clock",       0, SRD_CHANNEL_SCLK,  "dec_swd_chan_swclk" },
    { "swdio", "SWDIO", "Data input/output",   1, SRD_CHANNEL_SDATA, "dec_swd_chan_swdio" },
};

/* Options — match Python options tuple */
static struct srd_decoder_option swd_options[] = {
    { "strict_start", "dec_swd_opt_strict_start", "Wait for a line reset before starting to decode", NULL, NULL },
};

/* Annotation labels — match Python annotations tuple */
static const char *swd_ann_labels[][3] = {
    { "", "reset",  "RESET" },
    { "", "enable", "ENABLE" },
    { "", "read",   "READ" },
    { "", "write",  "WRITE" },
    { "", "ack",    "ACK" },
    { "", "data",   "DATA" },
    { "", "parity", "PARITY" },
};

/* Annotation row class lists */
static const int swd_row_all_classes[] = {
    ANN_RESET, ANN_ENABLE, ANN_READ, ANN_WRITE, ANN_ACK, ANN_DATA, ANN_PARITY, -1
};

static const struct srd_c_ann_row swd_ann_rows[] = {
    { "swd", "SWD", swd_row_all_classes, 7 },
};

static const char *swd_inputs[] = { "logic", NULL };
static const char *swd_outputs[] = { "swd", NULL };
static const char *swd_tags[] = { "Debug/trace", NULL };

/* ---- Helper: get human-readable address description ---- */
static const char *get_address_description(swd_s *s)
{
    static char buf[32];
    if (s->apdp == 0) { /* DP */
        if (s->rw == 1) { /* Read */
            switch (s->addr) {
            case 0x0: return "IDCODE";
            case 0x4: return s->ctrlsel == 0 ? "R CTRL/STAT" : "R DLCR";
            case 0x8: return "RESEND";
            case 0xC: return "RDBUFF";
            }
        } else { /* Write */
            switch (s->addr) {
            case 0x0: return "W ABORT";
            case 0x4: return s->ctrlsel == 0 ? "W CTRL/STAT" : "W DLCR";
            case 0x8: return "W SELECT";
            case 0xC: return "W RESERVED";
            }
        }
    } else { /* AP */
        if (s->rw == 1) {
            snprintf(buf, sizeof(buf), "R AP%x", s->addr);
        } else {
            snprintf(buf, sizeof(buf), "W AP%x", s->addr);
        }
        return buf;
    }
    snprintf(buf, sizeof(buf), "? %c%c%x", s->rw ? 'R' : 'W', s->apdp ? 'A' : 'D', s->addr);
    return buf;
}

/* ---- Helper: get ACK string ---- */
static const char *get_ack_string(int ack)
{
    switch (ack) {
    case 0: return "OK";
    case 1: return "FAULT";
    case 2: return "WAIT";
    case 3: return "NOREPLY";
    case 4: return "ERROR";
    default: return "UNKNOWN";
    }
}

/* ---- Helper: reset SWD state for line reset ---- */
static void swd_reset_state(swd_s *s)
{
    s->bits_len = 0;
    s->linereset_count = 0;
    s->turnaround = 0;
    s->sample_edge = RISING;
    s->state = REQ;
}

/* ---- Helper: advance to next SWD state ---- */
static void swd_next_state(swd_s *s)
{
    s->bits_len = 0;
    s->linereset_count = 0;
    switch (s->state) {
    case UNKNOWN:
        s->state = REQ;
        s->sample_edge = RISING;
        s->turnaround = 0;
        break;
    case REQ:
        s->state = ACK;
        s->sample_edge = FALLING;
        s->turnaround = 1;
        break;
    case ACK:
        s->state = DATA;
        if (s->rw == 0) { /* Write */
            s->sample_edge = RISING;
            s->turnaround = 2;
        } else { /* Read */
            s->sample_edge = FALLING;
            s->turnaround = 0;
        }
        break;
    case DATA:
        s->state = DPARITY;
        break;
    case DPARITY:
        s->state = REQ;
        s->sample_edge = RISING;
        s->turnaround = (s->rw == 1) ? 1 : 0;
        break;
    }
}

/* ---- Helper: update DP state on completed write ---- */
static void handle_completed_write(swd_s *s)
{
    if (s->apdp != 0)
        return;
    if (s->addr == ADDR_DP_SELECT)
        s->ctrlsel = s->data & 1;
    else if (s->addr == ADDR_DP_CTRLSTAT && s->ctrlsel == 0)
        s->orundetect = s->data & 1;
}

/* ---- start callback — match Python: start() ---- */
static void swd_start(struct srd_decoder_inst *di)
{
    swd_s *s = (swd_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "swd");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "swd");

    /* Non-zero defaults that calloc didn't set */
    s->state = UNKNOWN;
    s->sample_edge = RISING;

    /* Python: if self.options['strict_start'] == 'no': self.state = 'REQ' */
    const char *strict = c_opt_str(di, "strict_start", "no");
    if (strcmp(strict, "no") == 0)
        s->state = REQ;
}

/* ---- decode callback — main state machine ---- */
static void swd_decode(struct srd_decoder_inst *di)
{
    swd_s *s = (swd_s *)c_decoder_get_private(di);

    while (1) {
        /* Python: (clk, dio) = self.wait({0: 'e'}) */
        int ret = c_wait(di, CW_E(CH_SWCLK), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);
        int clk = c_pin(di, CH_SWCLK);
        int dio = c_pin(di, CH_SWDIO);

        /* Count rising edges with DIO held high for line reset detection */
        if (clk == RISING) {
            if (dio == 1) {
                if (s->linereset_count == 0)
                    s->ss_linereset = samplenum;
                s->linereset_count++;
            } else {
                if (s->linereset_count >= 50) {
                    c_put(di, s->ss_linereset, samplenum, s->out_ann, ANN_RESET, "LINERESET");
                    c_proto(di, s->ss_linereset, samplenum, s->out_python, "LINE_RESET", C_END);
                    swd_reset_state(s);
                }
                s->linereset_count = 0;
            }
        }

        /* Only care about the configured sample edge */
        if (clk != s->sample_edge)
            continue;

        /* Skip turnaround bits */
        if (s->turnaround > 0) {
            s->turnaround--;
            continue;
        }

        /* Accumulate bits */
        if (s->bits_len < 128) {
            s->bits[s->bits_len] = dio ? '1' : '0';
            s->samplenums[s->bits_len] = samplenum;
            s->bits_len++;
        }

        switch (s->state) {
        case UNKNOWN:
            /* Python: handle_unknown_edge — ignore until line reset */
            break;

        case REQ: {
            /* Check for JTAG->SWD enable sequence */
            if (s->bits_len >= 16) {
                static const char jtag_swd_pat[] = "0111100111100111";
                int match = 1;
                int i;
                for (i = 0; i < 16; i++) {
                    if (s->bits[s->bits_len - 16 + i] != jtag_swd_pat[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    uint64_t ss = s->samplenums[s->bits_len - 16];
                    s->ss_req = ss;
                    c_put(di, ss, samplenum, s->out_ann, ANN_ENABLE, "JTAG->SWD");
                    swd_reset_state(s);
                    break;
                }
            }

            /* Check for valid SWD request packet */
            if (s->bits_len >= 8) {
                int start_bit = s->bits[s->bits_len - 8] - '0';
                int apdp_bit  = s->bits[s->bits_len - 7] - '0';
                int rw_bit    = s->bits[s->bits_len - 6] - '0';
                int addr_bit0 = s->bits[s->bits_len - 5] - '0';
                int addr_bit1 = s->bits[s->bits_len - 4] - '0';
                int stop_bit  = s->bits[s->bits_len - 2] - '0';
                int park_bit  = s->bits[s->bits_len - 1] - '0';

                if (start_bit == 1 && stop_bit == 0 && park_bit == 1) {
                    /* Verify parity */
                    int calc_parity = (rw_bit + apdp_bit + addr_bit0 + addr_bit1) % 2;
                    int parity_bit = s->bits[s->bits_len - 3] - '0';
                    (void)calc_parity;
                    (void)parity_bit;

                    s->rw = rw_bit;
                    s->apdp = apdp_bit;
                    s->addr = (addr_bit1 * 2 + addr_bit0) << 2;

                    uint64_t ss = s->samplenums[s->bits_len - 8];
                    s->ss_req = ss;
                    const char *desc = get_address_description(s);
                    int ann = (s->rw == 1) ? ANN_READ : ANN_WRITE;
                    c_put(di, ss, samplenum, s->out_ann, ann, desc);
                    swd_next_state(s);
                }
            }
            break;
        }

        case ACK: {
            if (s->bits_len < 3)
                break;

            uint64_t ss = s->samplenums[s->bits_len - 3];

            if (s->bits[s->bits_len - 3] == '1' &&
                s->bits[s->bits_len - 2] == '0' &&
                s->bits[s->bits_len - 1] == '0') {
                c_put(di, ss, samplenum, s->out_ann, ANN_ACK, "OK");
                s->ack = 0;
                swd_next_state(s);
            } else if (s->bits[s->bits_len - 3] == '0' &&
                       s->bits[s->bits_len - 2] == '0' &&
                       s->bits[s->bits_len - 1] == '1') {
                c_put(di, ss, samplenum, s->out_ann, ANN_ACK, "FAULT");
                s->ack = 1;
                if (s->orundetect == 1)
                    swd_next_state(s);
                else
                    swd_reset_state(s);
                s->turnaround = 1;
            } else if (s->bits[s->bits_len - 3] == '0' &&
                       s->bits[s->bits_len - 2] == '1' &&
                       s->bits[s->bits_len - 1] == '0') {
                c_put(di, ss, samplenum, s->out_ann, ANN_ACK, "WAIT");
                s->ack = 2;
                if (s->orundetect == 1)
                    swd_next_state(s);
                else
                    swd_reset_state(s);
                s->turnaround = 1;
            } else if (s->bits[s->bits_len - 3] == '1' &&
                       s->bits[s->bits_len - 2] == '1' &&
                       s->bits[s->bits_len - 1] == '1') {
                c_put(di, ss, samplenum, s->out_ann, ANN_ACK, "NOREPLY");
                s->ack = 3;
                swd_reset_state(s);
            } else {
                c_put(di, ss, samplenum, s->out_ann, ANN_ACK, "ERROR");
                s->ack = 4;
                swd_reset_state(s);
            }
            break;
        }

        case DATA: {
            if (s->bits_len < 32)
                break;

            s->data = 0;
            s->dparity = 0;
            int i;
            for (i = 0; i < 32; i++) {
                if (s->bits[s->bits_len - 32 + i] == '1') {
                    s->data |= (1u << i);
                    s->dparity++;
                }
            }
            s->dparity %= 2;

            uint64_t ss = s->samplenums[s->bits_len - 32];
            char data_str[16];
            snprintf(data_str, sizeof(data_str), "0x%08x", s->data);
            c_put(di, ss, samplenum, s->out_ann, ANN_DATA, data_str);
            swd_next_state(s);
            break;
        }

        case DPARITY: {
            int parity_received = s->bits[s->bits_len - 1] - '0';
            uint64_t ss = s->samplenums[s->bits_len - 1];

            if (s->dparity != parity_received) {
                char ptext[16];
                snprintf(ptext, sizeof(ptext), "%d%d", s->dparity, parity_received);
                c_put(di, ss, samplenum, s->out_ann, ANN_PARITY, ptext);
            } else {
                /* Emit protocol output with address, data, ack */
                uint32_t addr32 = (uint32_t)s->addr;
                const char *ack_str = get_ack_string(s->ack);
                int ack_len = (int)strlen(ack_str) + 1;
                unsigned char py_data[8 + 32];
                memcpy(py_data, &addr32, 4);
                memcpy(py_data + 4, &s->data, 4);
                memcpy(py_data + 8, ack_str, ack_len);
                if (s->rw == 1) {
                    c_proto(di, s->ss_req, samplenum, s->out_python,
                            s->apdp ? "AP_READ" : "DP_READ",
                            C_BYTES(py_data, 8 + ack_len), C_END);
                } else {
                    c_proto(di, s->ss_req, samplenum, s->out_python,
                            s->apdp ? "AP_WRITE" : "DP_WRITE",
                            C_BYTES(py_data, 8 + ack_len), C_END);
                    handle_completed_write(s);
                }
            }
            swd_next_state(s);
            break;
        }
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder swd_c_def = {
    .id = "swd_c",
    .name = "SWD(C)",
    .longname = "Serial Wire Debug (C)",
    .desc = "SWD protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = swd_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = swd_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = swd_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = swd_ann_rows,
    .inputs = swd_inputs,
    .num_inputs = 1,
    .outputs = swd_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = swd_tags,
    .num_tags = 1,
    .state_size = sizeof(swd_s),
    .reset = swd_reset,
    .start = swd_start,
    .decode = swd_decode,
    .end = NULL,
    .metadata = NULL,
    .destroy = swd_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* Set option defaults — match Python options tuple */
    swd_options[0].def = g_variant_new_string("no");

    /* Set option value lists */
    GSList *strict_vals = NULL;
    strict_vals = g_slist_append(strict_vals, g_variant_new_string("yes"));
    strict_vals = g_slist_append(strict_vals, g_variant_new_string("no"));
    swd_options[0].values = strict_vals;

    return &swd_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}