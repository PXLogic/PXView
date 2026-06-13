/*
 * Copyright (C) 2023 edison ren <i2tv@qq.com>
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
    ANN_TYPE = 0,
    ANN_TRAINING,
    ANN_HEADER,
    ANN_DATA,
    ANN_CRC,
    ANN_WARNINGS,
    ANN_SRC,
    ANN_SNK,
    ANN_PAYLOAD,
    ANN_TEXT,
    ANN_CABLE,
    ANN_RESERVED,
    NUM_ANN,
};

#define UFCS_MAX_PKT 136

typedef struct {
    uint64_t ss_block;
    uint64_t es_block;
    int dataidx;
    uint8_t datapkt[UFCS_MAX_PKT];
    int plen;
    uint64_t bytepos_ss[UFCS_MAX_PKT];
    uint64_t bytepos_es[UFCS_MAX_PKT];
    char text[1024];
    int fulltext;
    int out_ann;
} ufcs_state;

static const char *ufcs_inputs[] = {"uart", NULL};
static const char *ufcs_tags[] = {"PC/Mobile", NULL};

static struct srd_decoder_option ufcs_options[] = {
    {"fulltext", "dec_ufcs_opt_fulltext", "Full text decoding of packets", NULL, NULL},
};

static const char *ufcs_ann_labels[][3] = {
    {"", "type", "Packet Type"},
    {"", "training", "Training"},
    {"", "header", "Header"},
    {"", "data", "Data"},
    {"", "crc", "Checksum"},
    {"", "warnings", "Warnings"},
    {"", "src", "Source Message"},
    {"", "snk", "Sink Message"},
    {"", "payload", "Payload"},
    {"", "text", "Plain text"},
    {"", "cable", "Cable Message"},
    {"", "reserved", "Reserved"},
};

static const int ufcs_row_phase_classes[] = {ANN_TRAINING, ANN_HEADER, ANN_DATA, ANN_CRC};
static const int ufcs_row_payload_classes[] = {ANN_PAYLOAD};
static const int ufcs_row_type_classes[] = {ANN_TYPE, ANN_SRC, ANN_SNK, ANN_CABLE, ANN_RESERVED};
static const int ufcs_row_warnings_classes[] = {ANN_WARNINGS};
static const int ufcs_row_text_classes[] = {ANN_TEXT};
static const struct srd_c_ann_row ufcs_ann_rows[] = {
    {"phase", "Parts", ufcs_row_phase_classes, 4},
    {"payload", "Payload", ufcs_row_payload_classes, 1},
    {"type", "Type", ufcs_row_type_classes, 5},
    {"warnings", "Warnings", ufcs_row_warnings_classes, 1},
    {"text", "Full text", ufcs_row_text_classes, 1},
};

static const char *ctrl_types[] = {
    "PING", "ACK", "NCK", "ACCEPT", "SOFT RESET", "POWER READY",
    "GET OUTPUT CAP", "GET SOURCE INFO", "GET SINK INFO", "GET CABLE INFO",
    "GET DEVICE INFO", "GET ERROR INFO", "DETECT CABLE INFO",
    "START CABLE DETECT", "END CABLE DETECT", "EXIT UFCS MODE",
};

static const char *data_types[] = {
    NULL, "OUTPUT CAP", "REQUEST", "SOURCE INFO", "SINK INFO",
    "CABLE INFO", "DEVICE INFO", "ERROR INFO", "CONFIG WATCHDOG",
    "REFUSE", "Verify_Request", "Verify_Response",
};

static uint8_t ufcs_compute_crc8(uint8_t *data, int len)
{
    const uint8_t CRC_8_POLY = 0x29;
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ CRC_8_POLY;
            else
                crc = crc << 1;
        }
    }
    return crc & 0xFF;
}

static uint16_t ufcs_get_short(ufcs_state *s, int i)
{
    if (i + 1 >= UFCS_MAX_PKT) return 0;
    return ((uint16_t)s->datapkt[i] << 8) | s->datapkt[i + 1];
}

static uint32_t ufcs_get_word(ufcs_state *s, int i)
{
    uint32_t hi = ufcs_get_short(s, i);
    uint32_t lo = ufcs_get_short(s, i + 2);
    return lo | (hi << 16);
}

static uint64_t ufcs_get_dword(ufcs_state *s, int i)
{
    uint64_t hi = ufcs_get_word(s, i);
    uint64_t lo = ufcs_get_word(s, i + 4);
    return lo | (hi << 32);
}

static int ufcs_head_id(ufcs_state *s)
{
    return (s->datapkt[0] >> 1) & 15;
}

static int ufcs_head_power_role(ufcs_state *s)
{
    return (s->datapkt[0] >> 5) & 7;
}

static int ufcs_head_rev(ufcs_state *s)
{
    return (s->datapkt[1] >> 3) & 31;
}

static int ufcs_head_type(ufcs_state *s)
{
    return s->datapkt[2];
}

static int ufcs_data_len(ufcs_state *s)
{
    if ((s->datapkt[1] & 7) == 1)
        return s->datapkt[3];
    return 0;
}

static void ufcs_puthead(struct srd_decoder_inst *di, ufcs_state *s)
{
    int pwr_role = ufcs_head_power_role(s);
    int ann_type;
    const char *role;
    if (pwr_role == 1) { ann_type = ANN_SRC; role = "SRC"; }
    else if (pwr_role == 2) { ann_type = ANN_SNK; role = "SNK"; }
    else if (pwr_role == 3) { ann_type = ANN_CABLE; role = "Cable"; }
    else { ann_type = ANN_RESERVED; role = "Reserved"; }

    int t = ufcs_head_type(s);
    char shortm[32];
    if (ufcs_data_len(s) == 0) {
        if (t > 15)
            snprintf(shortm, sizeof(shortm), "reserved cmd");
        else
            snprintf(shortm, sizeof(shortm), "%s", ctrl_types[t]);
    } else {
        if (t == 255)
            snprintf(shortm, sizeof(shortm), "Test Request");
        else if (t >= 1 && t <= 11)
            snprintf(shortm, sizeof(shortm), "%s", data_types[t]);
        else
            snprintf(shortm, sizeof(shortm), "DAT???");
    }

    char longm[128];
    snprintf(longm, sizeof(longm), "(r%d) %s[%d]: %s", ufcs_head_rev(s), role, ufcs_head_id(s), shortm);
    c_put(di, s->bytepos_ss[0], s->bytepos_es[2], s->out_ann, ann_type, longm, shortm);
    strncat(s->text, longm, sizeof(s->text) - strlen(s->text) - 1);
}

static void ufcs_decode_data_msg(struct srd_decoder_inst *di, ufcs_state *s)
{
    int dlen = ufcs_data_len(s);
    if (dlen == 0)
        return;

    int t = ufcs_head_type(s);
    char txt[256] = "";
    uint64_t d = 0;

    if (t == 1) {
        /* OUTPUT CAP - source capabilities with PDOs */
        int numbpdo = dlen / 8;
        for (int numb = 0; numb < numbpdo; numb++) {
            d = ufcs_get_dword(s, 4 + 8 * numb);
            int mode = (d >> 60) & 15;
            int step_ma = ((d >> 57) & 7) * 10 + 10;
            int step_mv = ((d >> 56) & 1) * 10 + 10;
            double max_mv = ((d >> 40) & 0xffff) * 0.01;
            double min_mv = ((d >> 24) & 0xffff) * 0.01;
            double max_ma = ((d >> 8) & 0xffff) * 0.01;
            double min_ma = (d & 0xff) * 0.01;

            int idx = 4 + 8 * numb;
            if (idx + 7 < UFCS_MAX_PKT) {
                char hexbuf[32];
                snprintf(hexbuf, sizeof(hexbuf), "[%d]%08llx", numb, (unsigned long long)d);
                c_put(di, s->bytepos_ss[idx], s->bytepos_es[idx + 7], s->out_ann, ANN_DATA, hexbuf);

                char pdobuf[128];
                snprintf(pdobuf, sizeof(pdobuf), "(PDO [%d] %g/%gV *%dmv %g/%gA *%dma)",
                    mode, min_mv, max_mv, step_mv, min_ma, max_ma, step_ma);
                c_put(di, s->bytepos_ss[idx], s->bytepos_es[idx + 7], s->out_ann, ANN_PAYLOAD, pdobuf);

                strncat(s->text, " - ", sizeof(s->text) - strlen(s->text) - 1);
                strncat(s->text, pdobuf, sizeof(s->text) - strlen(s->text) - 1);
            }
        }
        return;
    }

    int idx = 4;
    if (idx >= UFCS_MAX_PKT) return;

    if (t == 2) {
        /* REQUEST */
        d = ufcs_get_dword(s, 4);
        int mode = (d >> 60) & 15;
        double curr = (d & 0xffff) * 0.01;
        double volt = ((d >> 16) & 0xffff) * 0.01;
        snprintf(txt, sizeof(txt), "(PDO #%d) %gV %gA", mode, volt, curr);
    } else if (t == 3) {
        /* SOURCE INFO */
        d = ufcs_get_dword(s, 4);
        int it = ((d >> 40) & 0xff) - 50;
        int pt = ((d >> 32) & 0xff) - 50;
        double curr = (d & 0xffff) * 0.01;
        double volt = ((d >> 16) & 0xffff) * 0.01;
        char it_buf[32], pt_buf[32];
        snprintf(it_buf, sizeof(it_buf), "%s", it > -50 ? "no data" : "");
        if (it > -50) snprintf(it_buf, sizeof(it_buf), "%dC", it);
        snprintf(pt_buf, sizeof(pt_buf), "%s", pt > -50 ? "no data" : "");
        if (pt > -50) snprintf(pt_buf, sizeof(pt_buf), "%dC", pt);
        snprintf(txt, sizeof(txt), "(SRC info: %gV %gA, internal temp %s, usb port temp %s)",
            volt, curr, it_buf, pt_buf);
    } else if (t == 4) {
        /* SINK INFO */
        d = ufcs_get_dword(s, 4);
        int it = ((d >> 40) & 0xff) - 50;
        int pt = ((d >> 32) & 0xff) - 50;
        double curr = (d & 0xffff) * 0.01;
        double volt = ((d >> 16) & 0xffff) * 0.01;
        char it_buf[32], pt_buf[32];
        if (it > -50) snprintf(it_buf, sizeof(it_buf), "%dC", it);
        else snprintf(it_buf, sizeof(it_buf), "no data");
        if (pt > -50) snprintf(pt_buf, sizeof(pt_buf), "%dC", pt);
        else snprintf(pt_buf, sizeof(pt_buf), "no data");
        snprintf(txt, sizeof(txt), "(SNK info: %gV %gA, battery temp %s, usb port temp %s)",
            volt, curr, it_buf, pt_buf);
    } else if (t == 5) {
        /* CABLE INFO */
        d = ufcs_get_dword(s, 4);
        int vid = (d >> 48) & 0xffff;
        int emark = (d >> 32) & 0xffff;
        int imp = (d >> 16) & 0xffff;
        int curr = d & 0xff;
        int volt = (d >> 8) & 0xff;
        snprintf(txt, sizeof(txt), "(Cable VID %04X Emark VID %04X IMP %d %dV %dA)",
            vid, emark, imp, volt, curr);
    } else if (t == 6) {
        /* DEVICE INFO */
        d = ufcs_get_dword(s, 4);
        int vid = (d >> 48) & 0xffff;
        int pid = (d >> 32) & 0xffff;
        int hwv = (d >> 16) & 0xffff;
        int swv = d & 0xffff;
        snprintf(txt, sizeof(txt), "(Device VID %04X Protocol IC VID %04X HW rev%04X SW rev%04X)",
            vid, pid, hwv, swv);
    } else if (t == 7) {
        /* ERROR INFO */
        d = ufcs_get_word(s, 4);
        snprintf(txt, sizeof(txt), "(Error info: 0x%04X)", (unsigned)d);
    } else if (t == 8) {
        /* CONFIG WATCHDOG */
        d = ufcs_get_short(s, 4);
        snprintf(txt, sizeof(txt), "Watchdog overflow time %dms", (unsigned)d);
    } else if (t == 9) {
        /* REFUSE */
        d = ufcs_get_word(s, 4);
        snprintf(txt, sizeof(txt), "(Refuse: 0x%04X)", (unsigned)d);
    } else if (t == 10) {
        /* Verify Request */
        snprintf(txt, sizeof(txt), "(Verify Request)");
    } else if (t == 11) {
        /* Verify Response */
        snprintf(txt, sizeof(txt), "(Verify Response)");
    } else {
        snprintf(txt, sizeof(txt), "TODO...");
    }

    int end_idx = dlen + 3;
    if (end_idx < UFCS_MAX_PKT && idx < UFCS_MAX_PKT) {
        char hexbuf[32];
        snprintf(hexbuf, sizeof(hexbuf), "H:%08llx", (unsigned long long)d);
        c_put(di, s->bytepos_ss[idx], s->bytepos_es[end_idx], s->out_ann, ANN_DATA, hexbuf, "DATA");
        c_put(di, s->bytepos_ss[idx], s->bytepos_es[end_idx], s->out_ann, ANN_PAYLOAD, txt);
        strncat(s->text, " - ", sizeof(s->text) - strlen(s->text) - 1);
        strncat(s->text, txt, sizeof(s->text) - strlen(s->text) - 1);
    }
}

static void ufcs_decode_pkt(struct srd_decoder_inst *di, ufcs_state *s)
{
    /* Packet header */
    char buf[64];
    snprintf(buf, sizeof(buf), "HEAD:%04x", ((unsigned)s->datapkt[0] << 8) | s->datapkt[1]);
    c_put(di, s->bytepos_ss[0], s->bytepos_es[1], s->out_ann, ANN_HEADER, buf, "HD");

    ufcs_puthead(di, s);

    /* CMD */
    snprintf(buf, sizeof(buf), "CMD:%02x", s->datapkt[2]);
    c_put(di, s->bytepos_ss[2], s->bytepos_es[2], s->out_ann, ANN_HEADER, buf, "CMD");

    /* Data length */
    if (ufcs_data_len(s)) {
        snprintf(buf, sizeof(buf), "LEN:%02x", s->datapkt[3]);
        c_put(di, s->bytepos_ss[3], s->bytepos_es[3], s->out_ann, ANN_HEADER, buf, "LEN");
    }

    /* Decode data payload */
    ufcs_decode_data_msg(di, s);

    /* CRC check */
    uint8_t crc = s->datapkt[s->plen - 1];
    uint8_t ccrc = ufcs_compute_crc8(s->datapkt, s->plen - 1);
    if (crc != ccrc) {
        char warn[64];
        snprintf(warn, sizeof(warn), "Bad CRC %02x != %02x", crc, ccrc);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_WARNINGS, warn, "CRC!");
    }

    snprintf(buf, sizeof(buf), "CRC:%02x", crc);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_CRC, buf, "CRC");

    /* Full text trace */
    if (s->fulltext) {
        c_put(di, s->bytepos_ss[0], s->es_block, s->out_ann, ANN_TEXT, s->text, "...");
    }
}

static void ufcs_reset_state(ufcs_state *s)
{
    s->dataidx = 0;
    s->plen = 0;
    s->text[0] = '\0';
}

static void ufcs_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ufcs_state *s = (ufcs_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 1)
        return;

    s->ss_block = start_sample;
    s->es_block = end_sample;

    uint8_t val = fields[0].u8;

    /* SOP detection */
    if (val == 0xaa) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, "SOP:0xaa", "SOP");
        ufcs_reset_state(s);
        return;
    }

    /* Append data */
    if (s->dataidx < UFCS_MAX_PKT) {
        s->datapkt[s->dataidx] = val;
        s->bytepos_ss[s->dataidx] = start_sample;
        s->bytepos_es[s->dataidx] = end_sample;
    }

    /* Determine packet length at index 3 */
    if (s->dataidx == 3) {
        if ((s->datapkt[1] & 1) == 1)
            s->plen = s->datapkt[3] + 5;  /* data msg */
        else
            s->plen = 4;  /* ctrl msg */
    }

    s->dataidx++;

    /* Packet complete */
    if (s->dataidx == s->plen && s->plen > 0) {
        ufcs_decode_pkt(di, s);
        /* Do not reset state after decode - match Python behavior.
         * State is only reset on SOP (0xAA) byte. */
    }
}

static void ufcs_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ufcs_state)));
    }
    ufcs_state *s = (ufcs_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ufcs_state));
    ufcs_reset_state(s);
}

static void ufcs_start(struct srd_decoder_inst *di)
{
    ufcs_state *s = (ufcs_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ufcs");

    const char *ft = c_opt_str(di, "fulltext", "no");
    s->fulltext = (ft && strcmp(ft, "yes") == 0) ? 1 : 0;
}

static void ufcs_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ufcs_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ufcs_c_decoder = {
    .id = "ufcs_c",
    .name = "UFCS(C)",
    .longname = "Universal Fast Charging Specification (C)",
    .desc = "Universal fast charging specification for mobile devices. T/TAF 083-2021 (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ufcs_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ufcs_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = ufcs_ann_rows,
    .inputs = ufcs_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ufcs_tags,
    .num_tags = 1,
    .reset = ufcs_reset,
    .start = ufcs_start,
    .decode = ufcs_decode,
    .destroy = ufcs_destroy,
    .decode_upper = ufcs_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ufcs_options[0].def = g_variant_new_string("no");
    GSList *ft_vals = NULL;
    ft_vals = g_slist_append(ft_vals, g_variant_new_string("yes"));
    ft_vals = g_slist_append(ft_vals, g_variant_new_string("no"));
    ufcs_options[0].values = ft_vals;

    return &ufcs_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}