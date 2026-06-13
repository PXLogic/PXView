/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Kevin Redon <kingkevin@cuvoodoo.info>
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
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel indices — match Python: CS=0, SK=1, SI=2, SO=3 */
#define CH_CS 0
#define CH_SK 1
#define CH_SI 2
#define CH_SO 3

enum {
    ANN_START_BIT = 0,
    ANN_SI_BIT,
    ANN_SO_BIT,
    ANN_STATUS_READY,
    ANN_STATUS_BUSY,
    ANN_WARNING,
    NUM_ANN,
};

C_DECODER_STATE(mw, {
    int out_ann;
    int out_python;
});

/* Per-bit entry for Python output */
typedef struct {
    uint64_t ss;
    uint64_t es;
    int si;
    int so;
} mw_py_entry;

/* Per-change entry for packet collection */
typedef struct {
    uint64_t samplenum;
    uint64_t matched;
    int cs;
    int sk;
    int si;
    int so;
} mw_packet_entry;

static struct srd_channel mw_channels[] = {
    { "cs", "CS", "Chip select", 0, SRD_CHANNEL_COMMON, NULL },
    { "sk", "SK", "Clock", 1, SRD_CHANNEL_SCLK, NULL },
    { "si", "SI", "Slave in", 2, SRD_CHANNEL_SDATA, NULL },
    { "so", "SO", "Slave out", 3, SRD_CHANNEL_SDATA, NULL },
};

static const char* mw_ann_labels[][3] = {
    { "", "start-bit", "Start bit" },
    { "", "si-bit", "SI bit" },
    { "", "so-bit", "SO bit" },
    { "", "status-check-ready", "Status check ready" },
    { "", "status-check-busy", "Status check busy" },
    { "", "warning", "Warning" },
};

static const int mw_row_si_classes[] = { ANN_START_BIT, ANN_SI_BIT };
static const int mw_row_so_classes[] = { ANN_SO_BIT };
static const int mw_row_status_classes[] = { ANN_STATUS_READY, ANN_STATUS_BUSY };
static const int mw_row_warnings_classes[] = { ANN_WARNING };
static const struct srd_c_ann_row mw_ann_rows[] = {
    { "si-bits", "SI bits", mw_row_si_classes, 2 },
    { "so-bits", "SO bits", mw_row_so_classes, 1 },
    { "status", "Status", mw_row_status_classes, 2 },
    { "warnings", "Warnings", mw_row_warnings_classes, 1 },
};

static const char* mw_inputs[] = { "logic" };
static const char* mw_outputs[] = { "microwire" };
static const char* mw_tags[] = { "Embedded/industrial" };

static void mw_start(struct srd_decoder_inst *di)
{
    mw_s *s = (mw_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "microwire");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "microwire");
}

static void mw_decode(struct srd_decoder_inst *di)
{
    mw_s *s = (mw_s *)c_decoder_get_private(di);

    while (1) {
        /* Wait for slave to be selected on rising CS — Python: self.wait({0: 'r'}) */
        int ret = c_wait(di, CW_R(CH_CS), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);
        int sk = c_pin(di, CH_SK);

        if (sk) {
            c_put(di, samplenum, samplenum, s->out_ann, ANN_WARNING,
                "Clock should be low on start", "Clock high on start", "Clock high", "SK high");
        }

        /* Collect packet entries while CS is high.
         * Python saves state before each wait, so we save the CS rising edge
         * as packet[0] matching the Python decoder behavior. */
        GArray *packet = g_array_new(FALSE, FALSE, sizeof(mw_packet_entry));

        {
            mw_packet_entry cs_entry;
            cs_entry.samplenum = samplenum;
            cs_entry.matched = 0;
            cs_entry.cs = 1;
            cs_entry.sk = sk;
            cs_entry.si = c_pin(di, CH_SI);
            cs_entry.so = c_pin(di, CH_SO);
            g_array_append_val(packet, cs_entry);
        }

        int cs = 1;
        while (cs) {
            /* Python: self.wait([{0: 'l'}, {1: edge}, {3: 'e'}])
             * where edge = 'r' if sk==0 else 'f' */
            int cur_sk = c_pin(di, CH_SK);
            if (cur_sk == 0)
                ret = c_wait(di, CW_L(CH_CS), CW_OR, CW_R(CH_SK), CW_OR, CW_E(CH_SO), CW_END);
            else
                ret = c_wait(di, CW_L(CH_CS), CW_OR, CW_F(CH_SK), CW_OR, CW_E(CH_SO), CW_END);
            if (ret != SRD_OK) {
                g_array_free(packet, TRUE);
                return;
            }

            samplenum = di_samplenum(di);
            uint64_t matched = di_matched(di);
            cs = c_pin(di, CH_CS);
            sk = c_pin(di, CH_SK);
            int si = c_pin(di, CH_SI);
            int so = c_pin(di, CH_SO);

            mw_packet_entry entry;
            entry.samplenum = samplenum;
            entry.matched = matched;
            entry.cs = cs;
            entry.sk = sk;
            entry.si = si;
            entry.so = so;
            g_array_append_val(packet, entry);

            if (cs == 0)
                break;
        }

        /* Figure out if this is a status check.
         * Python: find first clock rising edge, check if SI is high (start bit). */
        int status_check = 1;
        for (guint i = 0; i < packet->len; i++) {
            mw_packet_entry *e = &g_array_index(packet, mw_packet_entry, i);
            if (e->matched & (1ULL << 1)) {
                if (e->sk) {
                    if (e->si)
                        status_check = 0;
                    break;
                }
            }
        }

        if (status_check) {
            /* Status check: SO low = busy, SO high = ready */
            uint64_t start_sn = g_array_index(packet, mw_packet_entry, 0).samplenum;
            int bit_so = g_array_index(packet, mw_packet_entry, 0).so;

            for (guint i = 0; i < packet->len; i++) {
                mw_packet_entry *e = &g_array_index(packet, mw_packet_entry, i);
                if (e->matched & (1ULL << 2)) {
                    if (bit_so == 0 && e->so) {
                        c_put(di, start_sn, e->samplenum, s->out_ann, ANN_STATUS_BUSY, "Busy", "B");
                    }
                    start_sn = e->samplenum;
                    bit_so = e->so;
                }
            }

            mw_packet_entry *last = &g_array_index(packet, mw_packet_entry, packet->len - 1);
            if (bit_so == 0) {
                c_put(di, start_sn, last->samplenum, s->out_ann, ANN_STATUS_BUSY, "Busy", "B");
            } else {
                c_put(di, start_sn, last->samplenum, s->out_ann, ANN_STATUS_READY, "Ready", "R");
            }
        } else {
            /* Bit communication */
            uint64_t bit_start = 0;
            int bit_si = 0;
            int bit_so = 0;
            int start_bit = 1;
            GArray *pydata = g_array_new(FALSE, FALSE, sizeof(mw_py_entry));

            for (guint i = 0; i < packet->len; i++) {
                mw_packet_entry *e = &g_array_index(packet, mw_packet_entry, i);

                if (e->matched & (1ULL << 1)) {
                    /* Clock edge */
                    if (e->sk) {
                        /* Rising clock edge */
                        if (bit_start > 0) {
                            if (start_bit) {
                                if (bit_si == 0) {
                                    c_put(di, bit_start, e->samplenum, s->out_ann, ANN_WARNING,
                                        "Start bit not high", "Start bit low");
                                } else {
                                    c_put(di, bit_start, e->samplenum, s->out_ann, ANN_START_BIT,
                                        "Start bit", "S");
                                }
                                start_bit = 0;
                            } else {
                                char si_long[16], so_long[16];
                                char si_mid[8], so_mid[8];
                                char si_short[4], so_short[4];
                                snprintf(si_long, sizeof(si_long), "SI bit: %d", bit_si);
                                snprintf(so_long, sizeof(so_long), "SO bit: %d", bit_so);
                                snprintf(si_mid, sizeof(si_mid), "SI: %d", bit_si);
                                snprintf(so_mid, sizeof(so_mid), "SO: %d", bit_so);
                                snprintf(si_short, sizeof(si_short), "%d", bit_si);
                                snprintf(so_short, sizeof(so_short), "%d", bit_so);
                                c_put(di, bit_start, e->samplenum, s->out_ann, ANN_SI_BIT,
                                    si_long, si_mid, si_short);
                                c_put(di, bit_start, e->samplenum, s->out_ann, ANN_SO_BIT,
                                    so_long, so_mid, so_short);
                                mw_py_entry pye = { bit_start, e->samplenum, bit_si, bit_so };
                                g_array_append_val(pydata, pye);
                            }
                        }
                        bit_start = e->samplenum;
                        bit_si = e->si;
                    } else {
                        /* Falling clock edge */
                        bit_so = e->so;
                    }
                } else if ((e->matched & (1ULL << 0)) && e->cs == 0 && e->sk == 0) {
                    /* End of packet */
                    char si_long[16], so_long[16];
                    char si_mid[8], so_mid[8];
                    char si_short[4], so_short[4];
                    snprintf(si_long, sizeof(si_long), "SI bit: %d", bit_si);
                    snprintf(so_long, sizeof(so_long), "SO bit: %d", bit_so);
                    snprintf(si_mid, sizeof(si_mid), "SI: %d", bit_si);
                    snprintf(so_mid, sizeof(so_mid), "SO: %d", bit_so);
                    snprintf(si_short, sizeof(si_short), "%d", bit_si);
                    snprintf(so_short, sizeof(so_short), "%d", bit_so);
                    c_put(di, bit_start, e->samplenum, s->out_ann, ANN_SI_BIT,
                        si_long, si_mid, si_short);
                    c_put(di, bit_start, e->samplenum, s->out_ann, ANN_SO_BIT,
                        so_long, so_mid, so_short);
                    mw_py_entry pye = { bit_start, e->samplenum, bit_si, bit_so };
                    g_array_append_val(pydata, pye);
                }
            }

            /* Protocol output */
            if (s->out_python >= 0 && pydata->len > 0) {
                mw_packet_entry *first = &g_array_index(packet, mw_packet_entry, 0);
                mw_packet_entry *last = &g_array_index(packet, mw_packet_entry, packet->len - 1);
                int bit_count = (int)pydata->len;
                int buf_size = bit_count * 18;
                unsigned char *py_buf = (unsigned char *)g_malloc(buf_size);
                for (int i = 0; i < bit_count; i++) {
                    mw_py_entry *pe = &g_array_index(pydata, mw_py_entry, i);
                    uint64_t ss = pe->ss;
                    uint64_t es = pe->es;
                    memcpy(py_buf + i * 18, &ss, 8);
                    memcpy(py_buf + i * 18 + 8, &es, 8);
                    py_buf[i * 18 + 16] = (unsigned char)pe->si;
                    py_buf[i * 18 + 17] = (unsigned char)pe->so;
                }
                c_proto(di, first->samplenum, last->samplenum,
                    s->out_python, "microwire",
                    C_BYTES(py_buf, buf_size), C_END);
                g_free(py_buf);
            }
            g_array_free(pydata, TRUE);
        }

        g_array_free(packet, TRUE);
    }
}

static struct srd_c_decoder microwire_c_def = {
    .id = "microwire_c",
    .name = "Microwire(C)",
    .longname = "Microwire (C)",
    .desc = "3-wire, half-duplex, synchronous serial bus (C implementation)",
    .license = "gplv2+",
    .channels = mw_channels,
    .num_channels = 4,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mw_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = mw_ann_rows,
    .inputs = mw_inputs,
    .num_inputs = 1,
    .outputs = mw_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = mw_tags,
    .num_tags = 1,
    .state_size = sizeof(mw_s),
    .reset = mw_reset,
    .start = mw_start,
    .decode = mw_decode,
    .destroy = mw_destroy,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &microwire_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}