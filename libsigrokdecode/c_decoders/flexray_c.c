/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2019 Stephan Thiele <stephan.thiele@mailbox.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    STATE_IDLE,
    STATE_GET_BITS,
};

enum {
    ANN_DATA = 0,
    ANN_TSS,
    ANN_FSS,
    ANN_RESERVED_BIT,
    ANN_PPI,
    ANN_NULL_FRAME,
    ANN_SYNC_FRAME,
    ANN_STARTUP_FRAME,
    ANN_ID,
    ANN_LENGTH,
    ANN_HEADER_CRC,
    ANN_CYCLE,
    ANN_DATA_BYTE,
    ANN_FRAME_CRC,
    ANN_FES,
    ANN_BSS,
    ANN_WARNING,
    ANN_BIT,
    ANN_CID,
    ANN_DTS,
    ANN_CAS,
    NUM_ANN,
};

/* FlexRay constants */
#define cChannelIdleDelimiter 11
#define cCrcInitA  0xFEDCBA
#define cCrcInitB  0xABCDEF
#define cCrcPolynomial 0x5D6DCB
#define cCrcSize   24
#define cdBSS       2
#define cdCAS      30
#define cdFES       2
#define cdFSS       1
#define cHCrcInit   0x01A
#define cHCrcPolynomial 0x385
#define cHCrcSize   11

#define CH_CHANNEL 0

typedef struct {
    int state;
    uint64_t samplerate;
    double bit_width;
    double sample_point;
    int sample_point_percent;

    uint8_t rawbits[4096];
    int num_rawbits;
    uint8_t bits[4096];
    int num_bits;
    int curbit;

    uint64_t tss_start, tss_end;
    uint64_t ss_block;
    uint64_t ss_bit0, ss_bit1, ss_bit2;
    uint64_t ss_databytebits[2048];
    int num_databytebits;

    int last_databit;
    int last_xmit_bit;
    int end_of_frame;
    int dynamic_frame;

    uint32_t frame_id;
    uint32_t payload_length;
    uint32_t header_crc;
    uint32_t frame_crc;
    uint32_t cycle;

    uint64_t dom_edge_snum;
    int dom_edge_bcount;

    int channel_type; /* 0=A, 1=B */
    int bitrate;

    int out_ann;
} flexray_state;

static uint32_t bitpack_msb(uint8_t *bits, int count)
{
    uint32_t val = 0;
    for (int i = 0; i < count && i < 32; i++)
        val = (val << 1) | (bits[i] & 1);
    return val;
}

static uint32_t flexray_crc(uint32_t data, int data_len_bits,
                            uint32_t polynom, int crc_len_bits,
                            uint32_t iv, uint32_t xor_val)
{
    uint32_t reg = iv ^ xor_val;
    for (int i = data_len_bits - 1; i >= 0; i--) {
        int bit = ((reg >> (crc_len_bits - 1)) & 1) ^ ((data >> i) & 1);
        reg <<= 1;
        if (bit)
            reg ^= polynom;
    }
    uint32_t mask = ((uint32_t)1 << crc_len_bits) - 1;
    return (reg & mask) ^ xor_val;
}

static void flexray_putg(flexray_state *s, struct srd_decoder_inst *di,
                         uint64_t ss, uint64_t es, int ann_class, const char **txts)
{
    int left = (int)s->sample_point;
    int right = (int)(s->bit_width - s->sample_point);
    uint64_t new_ss = (ss > (uint64_t)left) ? (ss - left) : 0;
    uint64_t new_es = es + right;

    struct srd_c_annotation ann;
    ann.ann_class = ann_class;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, new_ss, new_es, s->out_ann, &ann);
}

static void flexray_putx(flexray_state *s, struct srd_decoder_inst *di,
                         uint64_t samplenum, int ann_class, const char **txts)
{
    flexray_putg(s, di, samplenum, samplenum, ann_class, txts);
}

static void flexray_putb(flexray_state *s, struct srd_decoder_inst *di,
                         uint64_t ss, uint64_t es, int ann_class, const char **txts)
{
    flexray_putg(s, di, ss, es, ann_class, txts);
}

static void reset_variables(flexray_state *s)
{
    s->state = STATE_IDLE;
    s->num_rawbits = 0;
    s->num_bits = 0;
    s->curbit = 0;
    s->last_databit = 999;
    s->last_xmit_bit = 999;
    s->end_of_frame = 0;
    s->dynamic_frame = 0;
    s->frame_id = 0;
    s->payload_length = 0;
    s->header_crc = 0;
    s->frame_crc = 0;
    s->cycle = 0;
    s->ss_block = 0;
    s->ss_bit0 = 0;
    s->ss_bit1 = 0;
    s->ss_bit2 = 0;
    s->num_databytebits = 0;
}

static int is_bss_sequence(flexray_state *s)
{
    if (s->end_of_frame)
        return 0;
    if ((s->num_rawbits - 2) % 10 == 0)
        return 1;
    if ((s->num_rawbits - 3) % 10 == 0)
        return 1;
    return 0;
}

static void dom_edge_seen(flexray_state *s, uint64_t samplenum)
{
    (void)samplenum;
    s->dom_edge_snum = samplenum;
    s->dom_edge_bcount = s->curbit;
}

static uint64_t get_sample_point(flexray_state *s, int bitnum)
{
    double offset = (double)(bitnum - s->dom_edge_bcount) * s->bit_width;
    return (uint64_t)((double)s->dom_edge_snum + offset + s->sample_point);
}

static void handle_bit(flexray_state *s, struct srd_decoder_inst *di,
                       uint8_t fr_rx, uint64_t samplenum)
{
    (void)samplenum;
    if (s->num_rawbits < 4096) s->rawbits[s->num_rawbits++] = fr_rx;
    if (s->num_bits < 4096) s->bits[s->num_bits++] = fr_rx;

    int bitnum = s->num_bits - 1;

    if (is_bss_sequence(s)) {
        s->num_bits--;
        if (bitnum > 1) {
            char bss_str[4];
            snprintf(bss_str, sizeof(bss_str), "%d", fr_rx);
            const char *bss_txts[] = {bss_str, NULL};
            flexray_putx(s, di, samplenum, ANN_BSS, bss_txts);
        } else {
            if (s->num_rawbits == 2)
                s->ss_bit1 = samplenum;
            else if (s->num_rawbits == 3)
                s->ss_bit2 = samplenum;
        }
        s->curbit++;
        return;
    } else {
        if (bitnum > 1) {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", fr_rx);
            const char *bit_txts[] = {bit_str, NULL};
            flexray_putx(s, di, samplenum, ANN_BIT, bit_txts);
        }
    }

    if (bitnum == 0) {
        s->ss_bit0 = samplenum;
    } else if (bitnum == 1) {
        if (s->num_rawbits >= 3 && s->rawbits[0] == 1 && s->rawbits[1] == 1 && s->rawbits[2] == 0) {
            /* Normal frame start */
            const char *tss_txts[] = {"Transmission start sequence", "TSS", NULL};
            flexray_putg(s, di, s->tss_start, s->tss_end, ANN_TSS, tss_txts);

            char f0_str[4];
            snprintf(f0_str, sizeof(f0_str), "%d", s->rawbits[0]);
            const char *f0_txts[] = {f0_str, NULL};
            flexray_putg(s, di, s->ss_bit0, s->ss_bit0, ANN_BIT, f0_txts);

            const char *fss_txts[] = {"FSS", "Frame start sequence", NULL};
            flexray_putg(s, di, s->ss_bit0, s->ss_bit0, ANN_FSS, fss_txts);

            char f1_str[4];
            snprintf(f1_str, sizeof(f1_str), "%d", s->rawbits[1]);
            const char *f1_txts[] = {f1_str, NULL};
            flexray_putg(s, di, s->ss_bit1, s->ss_bit1, ANN_BSS, f1_txts);

            char f2_str[4];
            snprintf(f2_str, sizeof(f2_str), "%d", s->rawbits[2]);
            const char *f2_txts[] = {f2_str, NULL};
            flexray_putg(s, di, s->ss_bit2, s->ss_bit2, ANN_BSS, f2_txts);

            char rb_str[32], rb_short[16], rb_tiny[8];
            snprintf(rb_str, sizeof(rb_str), "Reserved bit: %d", fr_rx);
            snprintf(rb_short, sizeof(rb_short), "RB: %d", fr_rx);
            snprintf(rb_tiny, sizeof(rb_tiny), "RB");
            const char *rb_txts[] = {rb_str, rb_short, rb_tiny, NULL};
            flexray_putx(s, di, samplenum, ANN_RESERVED_BIT, rb_txts);
        } else if (s->num_rawbits >= 3 && s->rawbits[0] == 1 && s->rawbits[1] == 1 && s->rawbits[2] == 1) {
            /* CAS */
            const char *cas_txts[] = {"Collision avoidance symbol", "CAS", NULL};
            flexray_putg(s, di, s->tss_start, s->tss_end, ANN_CAS, cas_txts);
            reset_variables(s);
            return;
        }
    } else if (bitnum == 2) {
        char ppi_str[48], ppi_short[16];
        snprintf(ppi_str, sizeof(ppi_str), "Payload preamble indicator: %d", fr_rx);
        snprintf(ppi_short, sizeof(ppi_short), "PPI: %d", fr_rx);
        const char *ppi_txts[] = {ppi_str, ppi_short, NULL};
        flexray_putx(s, di, samplenum, ANN_PPI, ppi_txts);
    } else if (bitnum == 3) {
        const char *data_type = fr_rx ? "data frame" : "null frame";
        char nf_str[48], nf_short[16], nf_tiny[8];
        snprintf(nf_str, sizeof(nf_str), "Null frame indicator: %s", data_type);
        snprintf(nf_short, sizeof(nf_short), "NF: %d", fr_rx);
        snprintf(nf_tiny, sizeof(nf_tiny), "NF");
        const char *nf_txts[] = {nf_str, nf_short, nf_tiny, NULL};
        flexray_putx(s, di, samplenum, ANN_NULL_FRAME, nf_txts);
    } else if (bitnum == 4) {
        char sync_str[48], sync_short[16], sync_tiny[8];
        snprintf(sync_str, sizeof(sync_str), "Sync frame indicator: %d", fr_rx);
        snprintf(sync_short, sizeof(sync_short), "Sync: %d", fr_rx);
        snprintf(sync_tiny, sizeof(sync_tiny), "Sync");
        const char *sync_txts[] = {sync_str, sync_short, sync_tiny, NULL};
        flexray_putx(s, di, samplenum, ANN_SYNC_FRAME, sync_txts);
    } else if (bitnum == 5) {
        char startup_str[48], startup_short[16], startup_tiny[8];
        snprintf(startup_str, sizeof(startup_str), "Startup frame indicator: %d", fr_rx);
        snprintf(startup_short, sizeof(startup_short), "Startup: %d", fr_rx);
        snprintf(startup_tiny, sizeof(startup_tiny), "Startup");
        const char *startup_txts[] = {startup_str, startup_short, startup_tiny, NULL};
        flexray_putx(s, di, samplenum, ANN_STARTUP_FRAME, startup_txts);
    } else if (bitnum == 6) {
        s->ss_block = samplenum;
    } else if (bitnum == 16) {
        s->frame_id = bitpack_msb(&s->bits[6], 10);
        char id_str[32], id_short[16], id_tiny[16];
        snprintf(id_str, sizeof(id_str), "Frame ID: %d", s->frame_id);
        snprintf(id_short, sizeof(id_short), "ID: %d", s->frame_id);
        snprintf(id_tiny, sizeof(id_tiny), "%d", s->frame_id);
        const char *id_txts[] = {id_str, id_short, id_tiny, NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_ID, id_txts);
    } else if (bitnum == 17) {
        s->ss_block = samplenum;
    } else if (bitnum == 23) {
        s->payload_length = bitpack_msb(&s->bits[17], 7);
        char len_str[32], len_short[16], len_tiny[16];
        snprintf(len_str, sizeof(len_str), "Payload length: %d", s->payload_length);
        snprintf(len_short, sizeof(len_short), "Length: %d", s->payload_length);
        snprintf(len_tiny, sizeof(len_tiny), "%d", s->payload_length);
        const char *len_txts[] = {len_str, len_short, len_tiny, NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_LENGTH, len_txts);
    } else if (bitnum == 24) {
        s->ss_block = samplenum;
    } else if (bitnum == 34) {
        /* Header CRC */
        uint32_t header_to_check = bitpack_msb(&s->bits[4], 20);
        uint32_t expected_crc = flexray_crc(header_to_check, 20,
            cHCrcPolynomial, cHCrcSize, cHCrcInit, 0);
        s->header_crc = bitpack_msb(&s->bits[24], 11);
        const char *crc_ann = (s->header_crc == expected_crc) ? "OK" : "bad";
        char hcrc_str[48], hcrc_short[32], hcrc_tiny[16];
        snprintf(hcrc_str, sizeof(hcrc_str), "Header CRC: 0x%X (%s)", s->header_crc, crc_ann);
        snprintf(hcrc_short, sizeof(hcrc_short), "0x%X (%s)", s->header_crc, crc_ann);
        snprintf(hcrc_tiny, sizeof(hcrc_tiny), "0x%X", s->header_crc);
        const char *hcrc_txts[] = {hcrc_str, hcrc_short, hcrc_tiny, NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_HEADER_CRC, hcrc_txts);
    } else if (bitnum == 35) {
        s->ss_block = samplenum;
    } else if (bitnum == 40) {
        s->cycle = bitpack_msb(&s->bits[35], 6);
        char cyc_str[32], cyc_short[16], cyc_tiny[16];
        snprintf(cyc_str, sizeof(cyc_str), "Cycle: %d", s->cycle);
        snprintf(cyc_short, sizeof(cyc_short), "Cyc: %d", s->cycle);
        snprintf(cyc_tiny, sizeof(cyc_tiny), "%d", s->cycle);
        const char *cyc_txts[] = {cyc_str, cyc_short, cyc_tiny, NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_CYCLE, cyc_txts);
        s->last_databit = 41 + 2 * s->payload_length * 8;
    } else if (bitnum >= 41 && bitnum < s->last_databit) {
        if (s->num_databytebits < 2048)
            s->ss_databytebits[s->num_databytebits++] = samplenum;
    } else if (bitnum == s->last_databit) {
        if (s->num_databytebits < 2048)
            s->ss_databytebits[s->num_databytebits++] = samplenum;
        int num_data_bytes = 2 * s->payload_length;
        for (int i = 0; i < num_data_bytes; i++) {
            int x = 40 + (8 * i) + 1;
            uint8_t b = 0;
            if (x + 8 <= s->num_bits)
                b = (uint8_t)bitpack_msb(&s->bits[x], 8);
            if (i * 8 < s->num_databytebits && (i + 1) * 8 - 1 < s->num_databytebits) {
                char db_str[48], db_short[32], db_tiny[16];
                snprintf(db_str, sizeof(db_str), "Data byte %d: 0x%02x", i, b);
                snprintf(db_short, sizeof(db_short), "DB%d: 0x%02x", i, b);
                snprintf(db_tiny, sizeof(db_tiny), "%02X", b);
                const char *db_txts[] = {db_str, db_short, db_tiny, NULL};
                flexray_putg(s, di, s->ss_databytebits[i * 8],
                             s->ss_databytebits[(i + 1) * 8 - 1], ANN_DATA_BYTE, db_txts);
            }
        }
        s->num_databytebits = 0;
        s->ss_block = samplenum;
    } else if (bitnum == s->last_databit + 23) {
        /* Frame CRC */
        int frame_bits_count = s->num_bits - 1 - 24;
        uint32_t frame_to_check = 0;
        if (frame_bits_count > 0 && frame_bits_count <= 32)
            frame_to_check = bitpack_msb(&s->bits[1], frame_bits_count);
        uint32_t iv = (s->channel_type == 0) ? cCrcInitA : cCrcInitB;
        uint32_t expected_crc = flexray_crc(frame_to_check, frame_bits_count,
            cCrcPolynomial, cCrcSize, iv, 0);
        s->frame_crc = bitpack_msb(&s->bits[s->last_databit], 24);
        const char *crc_ann = (s->frame_crc == expected_crc) ? "OK" : "bad";
        char fcrc_str[48], fcrc_short[32], fcrc_tiny[16];
        snprintf(fcrc_str, sizeof(fcrc_str), "Frame CRC: 0x%X (%s)", s->frame_crc, crc_ann);
        snprintf(fcrc_short, sizeof(fcrc_short), "0x%X (%s)", s->frame_crc, crc_ann);
        snprintf(fcrc_tiny, sizeof(fcrc_tiny), "0x%X", s->frame_crc);
        const char *fcrc_txts[] = {fcrc_str, fcrc_short, fcrc_tiny, NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_FRAME_CRC, fcrc_txts);
        s->end_of_frame = 1;
    } else if (bitnum == s->last_databit + 24) {
        s->ss_block = samplenum;
    } else if (bitnum == s->last_databit + 25) {
        const char *fes_txts[] = {"Frame end sequence", "FES", NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_FES, fes_txts);
    } else if (bitnum == s->last_databit + 26) {
        if (!fr_rx)
            s->dynamic_frame = 1;
        else
            s->last_xmit_bit = bitnum;
        s->ss_block = samplenum;
    } else if (bitnum == s->last_xmit_bit) {
        s->ss_block = samplenum;
    } else if (bitnum == s->last_xmit_bit + cChannelIdleDelimiter - 1) {
        const char *cid_txts[] = {"Channel idle delimiter", "CID", NULL};
        flexray_putb(s, di, s->ss_block, samplenum, ANN_CID, cid_txts);
        reset_variables(s);
        return;
    } else if (bitnum > s->last_databit + 27) {
        if (s->dynamic_frame) {
            if (fr_rx) {
                if (s->last_xmit_bit == 999) {
                    const char *dts_txts[] = {"Dynamic trailing sequence", "DTS", NULL};
                    flexray_putb(s, di, s->ss_block, samplenum, ANN_DTS, dts_txts);
                    s->last_xmit_bit = bitnum + 1;
                    s->ss_block = samplenum;
                }
            }
        }
    }

    s->curbit++;
}

static struct srd_channel flexray_channels[] = {
    {"channel", "Channel", "FlexRay bus channel", 0, SRD_CHANNEL_SDATA, "dec_flexray_chan_channel"},
};

static struct srd_decoder_option flexray_options[] = {
    {"channel_type", "dec_flexray_opt_channel_type", "Channel type", NULL, NULL},
    {"bitrate", "dec_flexray_opt_bitrate", "Bitrate (bit/s)", NULL, NULL},
};

static const char *flexray_ann_labels[][3] = {
    {"", "data", "FlexRay payload data"},
    {"", "tss", "Transmission start sequence"},
    {"", "fss", "Frame start sequence"},
    {"", "reserved-bit", "Reserved bit"},
    {"", "ppi", "Payload preamble indicator"},
    {"", "null-frame", "Nullframe indicator"},
    {"", "sync-frame", "Full identifier"},
    {"", "startup-frame", "Startup frame indicator"},
    {"", "id", "Frame ID"},
    {"", "length", "Data length"},
    {"", "header-crc", "Header CRC"},
    {"", "cycle", "Cycle code"},
    {"", "data-byte", "Data byte"},
    {"", "frame-crc", "Frame CRC"},
    {"", "fes", "Frame end sequence"},
    {"", "bss", "Byte start sequence"},
    {"", "warning", "Warning"},
    {"", "bit", "Bit"},
    {"", "cid", "Channel idle delimiter"},
    {"", "dts", "Dynamic trailing sequence"},
    {"", "cas", "Collision avoidance symbol"},
};

static const int flexray_row_bits_classes[] = {ANN_BSS, ANN_BIT};
static const int flexray_row_fields_classes[] = {
    ANN_DATA, ANN_TSS, ANN_FSS, ANN_RESERVED_BIT, ANN_PPI, ANN_NULL_FRAME,
    ANN_SYNC_FRAME, ANN_STARTUP_FRAME, ANN_ID, ANN_LENGTH, ANN_HEADER_CRC,
    ANN_CYCLE, ANN_DATA_BYTE, ANN_FRAME_CRC, ANN_FES, ANN_CID, ANN_DTS, ANN_CAS
};
static const int flexray_row_warnings_classes[] = {ANN_WARNING};
static const struct srd_c_ann_row flexray_ann_rows[] = {
    {"bits", "Bits", flexray_row_bits_classes, 2},
    {"fields", "Fields", flexray_row_fields_classes, 18},
    {"warnings", "Warnings", flexray_row_warnings_classes, 1},
};

static const char *flexray_inputs[] = {"logic"};
static const char *flexray_outputs[] = {};
static const char *flexray_tags[] = {"Automotive"};

static void flexray_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(flexray_state)));
    flexray_state *s = (flexray_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(flexray_state));
    s->sample_point_percent = 50;
    s->last_databit = 999;
    s->last_xmit_bit = 999;
    s->out_ann = 0;
}

static void flexray_start(struct srd_decoder_inst *di)
{
    flexray_state *s = (flexray_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "flexray");

    const char *ch_type = c_opt_str(di, "channel_type", "A");
    s->channel_type = (strcmp(ch_type, "B") == 0) ? 1 : 0;

    s->bitrate = (int)c_opt_int(di, "bitrate", 10000000);
    s->samplerate = c_samplerate(di);

    if (s->samplerate > 0 && s->bitrate > 0) {
        s->bit_width = (double)s->samplerate / (double)s->bitrate;
        s->sample_point = (s->bit_width / 100.0) * s->sample_point_percent;
    }
}

static void flexray_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    flexray_state *s = (flexray_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->bitrate > 0) {
            s->bit_width = (double)s->samplerate / (double)s->bitrate;
            s->sample_point = (s->bit_width / 100.0) * s->sample_point_percent;
        }
    }
}

static void flexray_decode(struct srd_decoder_inst *di)
{
    flexray_state *s = (flexray_state *)c_decoder_get_private(di);
    /* Fallback samplerate */
    if (s->samplerate == 0)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0 || s->bitrate == 0)
        return;

    if (s->bit_width == 0) {
        s->bit_width = (double)s->samplerate / (double)s->bitrate;
        s->sample_point = (s->bit_width / 100.0) * s->sample_point_percent;
    }

    while (1) {
        if (s->state == STATE_IDLE) {
            int ret = c_wait(di, CW_L(CH_CHANNEL), CW_END);
            if (ret != SRD_OK)
                return;

            s->tss_start = di_samplenum(di);

            ret = c_wait(di, CW_H(CH_CHANNEL), CW_END);
            if (ret != SRD_OK)
                return;

            s->tss_end = di_samplenum(di);
            dom_edge_seen(s, di_samplenum(di));
            s->state = STATE_GET_BITS;

        } else if (s->state == STATE_GET_BITS) {
            uint64_t pos = get_sample_point(s, s->curbit);
            uint64_t skip_count = 0;
            if (pos > di_samplenum(di))
                skip_count = pos - di_samplenum(di);

            int ret = c_wait(di, CW_SKIP(skip_count), CW_OR, CW_F(CH_CHANNEL), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1 << 1)) {
                dom_edge_seen(s, di_samplenum(di));
            }

            if (di_matched(di) & (1 << 0)) {
                uint8_t fr_rx = c_pin(di, CH_CHANNEL);
                handle_bit(s, di, fr_rx, pos);
                if (s->state != STATE_GET_BITS)
                    continue;
            }
        }
    }
}

static void flexray_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder flexray_c_decoder = {
    .id = "flexray_c",
    .name = "FlexRay(C)",
    .longname = "FlexRay (C)",
    .desc = "Automotive network communications protocol. (C implementation)",
    .license = "gplv2+",
    .channels = flexray_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = flexray_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = flexray_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = flexray_ann_rows,
    .reset = flexray_reset,
    .start = flexray_start,
    .decode = flexray_decode,
    .metadata = flexray_metadata,
    .destroy = flexray_destroy,
    .state_size = 0,
    .inputs = flexray_inputs,
    .num_inputs = 1,
    .outputs = flexray_outputs,
    .num_outputs = 0,
    .tags = flexray_tags,
    .num_tags = 1,
    .binary = NULL,
    .num_binary = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    flexray_options[0].def = g_variant_new_string("A");
    {
        GVariant *v0 = g_variant_new_string("A");
        GVariant *v1 = g_variant_new_string("B");
        GSList *vals = g_slist_append(NULL, v0);
        vals = g_slist_append(vals, v1);
        flexray_options[0].values = vals;
    }
    flexray_options[1].def = g_variant_new_int64(10000000);
    {
        GVariant *v0 = g_variant_new_int64(10000000);
        GVariant *v1 = g_variant_new_int64(5000000);
        GVariant *v2 = g_variant_new_int64(2500000);
        GSList *vals = g_slist_append(NULL, v0);
        vals = g_slist_append(vals, v1);
        vals = g_slist_append(vals, v2);
        flexray_options[1].values = vals;
    }
    return &flexray_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}