/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Google, Inc
 * Copyright (C) 2018 davidanger <davidanger@163.com>
 * Copyright (C) 2018 Peter Hazenberg <sigrok@haas-en-berg.nl>
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
    ANN_PREAMBLE,
    ANN_SOP,
    ANN_HEADER,
    ANN_DATA,
    ANN_CRC,
    ANN_EOP,
    ANN_SYM,
    ANN_WARNINGS,
    ANN_SRC,
    ANN_SNK,
    ANN_PAYLOAD,
    ANN_TEXT,
    NUM_ANN,
};

/* BMC encoding with 600kHz datarate */
#define UI_US (1000000.0 / 600000.0)
#define THRESHOLD_US ((UI_US + 2 * UI_US) / 2)

/* 4b5b decode table */
static const int DEC4B5B[32] = {
    0x10, 0x10, 0x10, 0x10,
    0x10, 0x10, 0x13, 0x14,
    0x10, 0x01, 0x04, 0x05,
    0x10, 0x16, 0x06, 0x07,
    0x10, 0x12, 0x08, 0x09,
    0x02, 0x03, 0x0A, 0x0B,
    0x11, 0x15, 0x0C, 0x0D,
    0x0E, 0x0F, 0x00, 0x10,
};

#define SYM_ERR 0x10
#define SYNC1   0x11
#define SYNC2   0x12
#define SYNC3   0x13
#define RST1    0x14
#define RST2    0x15
#define EOP_SYM 0x16

#define CH_CC1 0
#define CH_CC2 1

static const char *CTRL_TYPES[] = {
    "reserved", "GOOD CRC", "GOTO MIN", "ACCEPT", "REJECT", "PING",
    "PS RDY", "GET SOURCE CAP", "GET SINK CAP", "DR SWAP", "PR SWAP",
    "VCONN SWAP", "WAIT", "SOFT RESET", "reserved", "reserved",
    "Not Supported", "Get_Source_Cap_Extended", "Get_Status", "FR_Swap",
    "Get_PPS_Status", "Get_Country_Codes", "Get_Sink_Cap_Extended",
    "Get_Source_Info", "Get_Revision"
};

static const char *DATA_TYPES[] = {
    NULL, "SOURCE CAP", "REQUEST", "BIST", "SINK CAP",
    "Battery_Status", "Alert", "Get_Country_Info", "Enter_USB",
    "EPR_Request", "EPR_Mode", "Source_Info", "Revision",
    NULL, NULL, "VDM"
};

static const char *EXTENDED_TYPES[] = {
    NULL, "Source_Cap_Extended", "Status", "Get_Battery_Cap",
    "Get_Battery_Status", "Battery_Cap", "Get_Manufacturer_Info",
    "Manufacturer_Info", "Security_Request", "Security_Response",
    "Firmware_Update_Request", "Firmware_Update_Response",
    "PPS_Status", "Country_Info", "Country_Codes",
    "Sink_Capabilities_Extended", "Extended_Control",
    "EPR_Source_Capabilities", "EPR_Sink_Capabilities",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "Vendor_Defined_Extended"
};

static const char *SYM_NAME[][2] = {
    {"0x0", "0"}, {"0x1", "1"}, {"0x2", "2"}, {"0x3", "3"},
    {"0x4", "4"}, {"0x5", "5"}, {"0x6", "6"}, {"0x7", "7"},
    {"0x8", "8"}, {"0x9", "9"}, {"0xA", "A"}, {"0xB", "B"},
    {"0xC", "C"}, {"0xD", "D"}, {"0xE", "E"}, {"0xF", "F"},
    {"ERROR", "X"}, {"SYNC-1", "S1"}, {"SYNC-2", "S2"}, {"SYNC-3", "S3"},
    {"RST-1", "R1"}, {"RST-2", "R2"}, {"EOP", "#"},
};

static const int SOP_SEQUENCES[7][4] = {
    {SYNC1, SYNC1, SYNC1, SYNC2},
    {SYNC1, SYNC1, SYNC3, SYNC3},
    {SYNC1, SYNC3, SYNC1, SYNC3},
    {SYNC1, RST2,  RST2,  SYNC3},
    {SYNC1, RST2,  SYNC3, SYNC2},
    {RST1,  SYNC1, RST1,  SYNC3},
    {RST1,  RST1,  RST1,  RST2},
};

static const char *SOP_NAMES[] = {
    "SOP", "SOP'", "SOP\"", "SOP' Debug", "SOP\" Debug",
    "Cable Reset", "Hard Reset"
};

typedef struct {
    uint64_t samplerate;
    uint64_t maxbit;
    uint64_t threshold;

    /* BMC decode state */
    uint64_t previous;
    uint64_t startsample;
    uint8_t bits[4096];
    int num_bits;
    uint64_t edges[4096];
    int num_edges;
    int half_one;
    uint64_t start_one;

    /* Packet decode state */
    int idx;
    int packet_seq;
    uint16_t head;
    uint16_t ext_head;
    uint32_t data[16];
    uint32_t ext_data[16];
    int num_data;
    int chunked;
    int chunk_num;
    int req_chunk;
    int data_size;

    /* Stored PDO info */
    int cap_mark[17];

    /* Text tracking */
    char text[2048];
    int fulltext;

    int out_ann;
    int out_python;
    int out_binary;
    int out_bitrate;
} usb_pd_state;

static uint32_t compute_crc32(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

static void pd_put(usb_pd_state *s, struct srd_decoder_inst *di,
                   uint64_t ss, uint64_t es, int ann_class, const char **txts)
{
    struct srd_c_annotation ann;
    ann.ann_class = ann_class;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, ss, es, s->out_ann, &ann);
}

static void pd_putx(usb_pd_state *s, struct srd_decoder_inst *di,
                    int s0, int s1, int ann_class, const char **txts)
{
    if (s0 >= 0 && s0 < s->num_edges) {
        int e1;
        if (s1 < 0)
            e1 = s->num_edges - 1;
        else if (s1 < s->num_edges)
            e1 = s1;
        else
            e1 = s->num_edges - 1;
        pd_put(s, di, s->edges[s0], s->edges[e1], ann_class, txts);
    }
}

static void pd_putwarn(usb_pd_state *s, struct srd_decoder_inst *di,
                       const char *longm, const char *shortm)
{
    const char *txts[] = {longm, shortm, NULL};
    pd_putx(s, di, 0, -1, ANN_WARNINGS, txts);
}

static void rec_sym(usb_pd_state *s, struct srd_decoder_inst *di, int i, int sym)
{
    if (sym >= 0 && sym < 23) {
        const char *txts[] = {SYM_NAME[sym][0], SYM_NAME[sym][1], NULL};
        pd_putx(s, di, i, i + 5, ANN_SYM, txts);
    }
}

static int get_sym(usb_pd_state *s, struct srd_decoder_inst *di, int i, int rec)
{
    if (i + 4 >= s->num_bits)
        return SYM_ERR;
    int v = s->bits[i] | (s->bits[i+1] << 1) | (s->bits[i+2] << 2) |
            (s->bits[i+3] << 3) | (s->bits[i+4] << 4);
    int sym = DEC4B5B[v];
    if (rec)
        rec_sym(s, di, i, sym);
    return sym;
}

static uint16_t get_short(usb_pd_state *s, struct srd_decoder_inst *di)
{
    int i = s->idx;
    if (s->num_bits - i <= 20) {
        pd_putwarn(s, di, "Truncated", "!");
        return 0x0BAD;
    }
    int k[4];
    k[0] = get_sym(s, di, i, 1);
    k[1] = get_sym(s, di, i + 5, 1);
    k[2] = get_sym(s, di, i + 10, 1);
    k[3] = get_sym(s, di, i + 15, 1);
    uint16_t val = (uint16_t)(k[0] | (k[1] << 4) | (k[2] << 8) | (k[3] << 12));
    s->idx += 20;
    return val;
}

static uint32_t get_word(usb_pd_state *s, struct srd_decoder_inst *di)
{
    uint16_t lo = get_short(s, di);
    uint16_t hi = get_short(s, di);
    return (uint32_t)lo | ((uint32_t)hi << 16);
}

static int head_ext(usb_pd_state *s) { return (s->head >> 15) & 1; }
static int head_count(usb_pd_state *s) { return (s->head >> 12) & 7; }
static int head_id(usb_pd_state *s) { return (s->head >> 9) & 7; }
static int head_power_role(usb_pd_state *s) { return (s->head >> 8) & 1; }
static int head_rev(usb_pd_state *s) { return ((s->head >> 6) & 3) + 1; }
static int head_data_role(usb_pd_state *s) { return (s->head >> 5) & 1; }
static int head_type(usb_pd_state *s) { return s->head & 0x1F; }

static const char *find_corrupted_sop(int k[4])
{
    for (int seq = 0; seq < 7; seq++) {
        int matches = 0;
        for (int i = 0; i < 4; i++)
            if (k[i] == SOP_SEQUENCES[seq][i])
                matches++;
        if (matches >= 3)
            return SOP_NAMES[seq];
    }
    return NULL;
}

static int scan_eop(usb_pd_state *s, struct srd_decoder_inst *di)
{
    for (int i = 0; i < s->num_bits - 19; i++) {
        int k[4];
        k[0] = get_sym(s, di, i, 0);
        k[1] = get_sym(s, di, i + 5, 0);
        k[2] = get_sym(s, di, i + 10, 0);
        k[3] = get_sym(s, di, i + 15, 0);

        const char *sym_name = NULL;
        for (int seq = 0; seq < 7; seq++) {
            if (k[0] == SOP_SEQUENCES[seq][0] && k[1] == SOP_SEQUENCES[seq][1] &&
                k[2] == SOP_SEQUENCES[seq][2] && k[3] == SOP_SEQUENCES[seq][3]) {
                sym_name = SOP_NAMES[seq];
                break;
            }
        }
        if (!sym_name)
            sym_name = find_corrupted_sop(k);

        if (sym_name) {
            /* Annotate preamble */
            const char *pre_txts[] = {"Preamble", "...", NULL};
            pd_putx(s, di, 0, i, ANN_PREAMBLE, pre_txts);

            /* Annotate SOP symbols */
            rec_sym(s, di, i, k[0]);
            rec_sym(s, di, i + 5, k[1]);
            rec_sym(s, di, i + 10, k[2]);
            rec_sym(s, di, i + 15, k[3]);

            if (strcmp(sym_name, "Hard Reset") == 0) {
                strcat(s->text, "HRST");
                return -1;
            } else if (strcmp(sym_name, "Cable Reset") == 0) {
                strcat(s->text, "CRST");
                return -1;
            } else {
                const char *sop_txts[] = {sym_name, "S", NULL};
                pd_putx(s, di, i, i + 20, ANN_SOP, sop_txts);
            }
            return i + 20;
        }
    }

    const char *junk_txts[] = {"Junk???", "XXX", NULL};
    pd_putx(s, di, 0, s->num_bits - 1, ANN_PREAMBLE, junk_txts);
    strcat(s->text, "Junk???");
    pd_putwarn(s, di, "No start of packet found", "XXX");
    return -1;
}

static void puthead(usb_pd_state *s, struct srd_decoder_inst *di)
{
    int ann_type = head_power_role(s) ? ANN_SRC : ANN_SNK;
    const char *role = head_power_role(s) ? "SRC" : "SNK";
    if (head_data_role(s) != head_power_role(s))
        strcat((char[]){0}, ""); /* just skip role extension for simplicity */

    int t = head_type(s);
    const char *shortm;

    if (head_ext(s) == 1) {
        shortm = (t <= 30 && EXTENDED_TYPES[t]) ? EXTENDED_TYPES[t] : "EXTENDED???";
    } else if (head_count(s) == 0) {
        if (t >= 25 && t <= 31)
            shortm = "reserved";
        else if (t < 25)
            shortm = CTRL_TYPES[t];
        else
            shortm = "reserved";
    } else {
        shortm = (t <= 15 && DATA_TYPES[t]) ? DATA_TYPES[t] : "DAT???";
    }

    char longm[128];
    snprintf(longm, sizeof(longm), "(r%d) %s[%d]: %s", head_rev(s), role, head_id(s), shortm);
    const char *txts[] = {longm, shortm, NULL};
    pd_putx(s, di, 0, -1, ann_type, txts);

    int tlen = strlen(s->text);
    if (tlen + strlen(longm) < (int)sizeof(s->text) - 1)
        strcat(s->text, longm);
}

static void get_request(usb_pd_state *s, uint32_t rdo, char *buf, int bufsize)
{
    int pos = (rdo >> 28) & 0x0F;
    int mark = s->cap_mark[pos];
    char t_settings[64] = {0};

    if (mark == 3) {
        double op_v = ((rdo >> 9) & 0x7ff) * 0.02;
        double op_a = (rdo & 0xff) * 0.05;
        snprintf(t_settings, sizeof(t_settings), "%gV %gA", op_v, op_a);
    } else if (mark == 2) {
        double op_w = ((rdo >> 10) & 0x3ff) * 0.25;
        snprintf(t_settings, sizeof(t_settings), "%gW (operating)", op_w);
    } else {
        double op_a = ((rdo >> 10) & 0x3ff) * 0.01;
        double max_a = (rdo & 0x3ff) * 0.01;
        snprintf(t_settings, sizeof(t_settings), "%gA (operating) / %gA (max)", op_a, max_a);
    }

    /* RDO flags in reverse order of bit position */
    static const struct { uint32_t bit; const char *name; } rdo_flags[] = {
        {1 << 27, "give_back"},
        {1 << 26, "cap_mismatch"},
        {1 << 25, "comm_cap"},
        {1 << 24, "no_suspend"},
        {1 << 23, "unchunked"},
    };

    char t_flags[128] = {0};
    for (int i = 0; i < (int)(sizeof(rdo_flags) / sizeof(rdo_flags[0])); i++) {
        if (rdo & rdo_flags[i].bit) {
            strcat(t_flags, " [");
            strcat(t_flags, rdo_flags[i].name);
            strcat(t_flags, "]");
        }
    }

    snprintf(buf, bufsize, "(PDO #%d) %s%s", pos, t_settings, t_flags);
}

static void get_source_sink_cap(usb_pd_state *s, uint32_t pdo, int idx, int source, char *buf, int bufsize)
{
    int t1 = (pdo >> 30) & 3;
    s->cap_mark[idx] = t1;

    char t_name[32] = {0};
    char p[128] = {0};
    char t_flags[256] = {0};

    if (t1 == 0) {
        strcpy(t_name, "Fixed");
        if (source) {
            static const struct { uint32_t bit; const char *name; } src_flags[] = {
                {1 << 29, "dual_role_power"},
                {1 << 28, "suspend"},
                {1 << 27, "unconstrained"},
                {1 << 26, "comm_cap"},
                {1 << 25, "dual_role_data"},
                {1 << 24, "unchunked"},
            };
            for (int i = 0; i < (int)(sizeof(src_flags) / sizeof(src_flags[0])); i++) {
                if (pdo & src_flags[i].bit) {
                    strcat(t_flags, " [");
                    strcat(t_flags, src_flags[i].name);
                    strcat(t_flags, "]");
                }
            }
        }
        double mv = ((pdo >> 10) & 0x3ff) * 0.05;
        double ma = (pdo & 0x3ff) * 0.01;
        snprintf(p, sizeof(p), "%gV %gA (%gW)", mv, ma, mv * ma);
    } else if (t1 == 1) {
        strcpy(t_name, "Battery");
        double minv = ((pdo >> 10) & 0x3ff) * 0.05;
        double maxv = ((pdo >> 20) & 0x3ff) * 0.05;
        double mw = (pdo & 0x3ff) * 0.25;
        snprintf(p, sizeof(p), "%g/%gV %gW", minv, maxv, mw);
    } else if (t1 == 2) {
        strcpy(t_name, "Variable");
        double minv = ((pdo >> 10) & 0x3ff) * 0.05;
        double maxv = ((pdo >> 20) & 0x3ff) * 0.05;
        double ma = (pdo & 0x3ff) * 0.01;
        snprintf(p, sizeof(p), "%g/%gV %gA (%gW)", minv, maxv, ma, maxv * ma);
    } else if (t1 == 3) {
        int t2 = (pdo >> 28) & 3;
        if (t2 == 0) {
            strcpy(t_name, "PPS");
            double minv = ((pdo >> 8) & 0xff) * 0.1;
            double maxv = ((pdo >> 17) & 0xff) * 0.1;
            double ma = (pdo & 0xff) * 0.05;
            snprintf(p, sizeof(p), "%g/%gV %gA (%gW)", minv, maxv, ma, maxv * ma);
            if ((pdo >> 27) & 1)
                strcat(p, " [limited]");
            if (pdo & (1 << 29)) {
                strcat(t_flags, " [power_limited]");
            }
        } else {
            snprintf(t_name, sizeof(t_name), "Reserved APDO: %s", "???");
            snprintf(p, sizeof(p), "[raw: %s]", "???");
        }
    }

    snprintf(buf, bufsize, "[%s] %s%s", t_name, p, t_flags);
}

static void putpayload(usb_pd_state *s, struct srd_decoder_inst *di,
                       int s0, int s1, int idx)
{
    int t;
    if (head_ext(s) == 0)
        t = head_type(s);
    else
        t = 255;

    char txt[256];
    snprintf(txt, sizeof(txt), "[%d] ", idx + 1);

    if (t == 255) {
        /* Extended Message - simplified */
        char hex[32];
        snprintf(hex, sizeof(hex), "%08x", s->data[idx]);
        strcat(txt, hex);
    } else if (t == 2) {
        char req[128];
        get_request(s, s->data[idx], req, sizeof(req));
        strcat(txt, req);
    } else if (t == 1 || t == 4) {
        char cap[128];
        get_source_sink_cap(s, s->data[idx], idx + 1, t == 1, cap, sizeof(cap));
        strcat(txt, cap);
    } else if (t == 15) {
        /* VDM - simplified */
        char vdm[32];
        snprintf(vdm, sizeof(vdm), "VDM:%08x", s->data[idx]);
        strcat(txt, vdm);
    } else if (t == 3) {
        /* BIST - simplified */
        int mode = s->data[idx] >> 28;
        static const char *bist_modes[] = {
            "Receiver", "Transmit", "Counters", "Carrier 0",
            "Carrier 1", "Carrier 2", "Carrier 3", "Eye"
        };
        if (mode >= 0 && mode < 8)
            snprintf(txt + strlen(txt), sizeof(txt) - strlen(txt), "mode %s", bist_modes[mode]);
    } else if (t == 10) {
        /* EPR Mode - simplified */
        int action = s->data[idx] >> 24;
        static const char *epr_actions[] = {
            "", "Enter", "Enter Acknowledged", "Enter Succeeded",
            "Enter Failed", "Exit"
        };
        if (action >= 1 && action <= 5)
            strcat(txt, epr_actions[action]);
    } else if (t == 9) {
        if (idx == 0) {
            char req[128];
            get_request(s, s->data[idx], req, sizeof(req));
            strcat(txt, req);
        } else {
            char cap[128];
            get_source_sink_cap(s, s->data[idx], idx + 1, 1, cap, sizeof(cap));
            strcat(txt, cap);
        }
    }

    const char *txts[] = {txt, txt, NULL};
    pd_putx(s, di, s0, s1, ANN_PAYLOAD, txts);

    int tlen = strlen(s->text);
    if (tlen + 3 + strlen(txt) < (int)sizeof(s->text) - 1) {
        strcat(s->text, " - ");
        strcat(s->text, txt);
    }
}

static void decode_packet(usb_pd_state *s, struct srd_decoder_inst *di)
{
    s->num_data = 0;
    s->idx = 0;
    s->text[0] = '\0';

    if (s->num_edges < 50)
        return;

    s->packet_seq++;
    double tstamp = (double)s->startsample / (double)s->samplerate;
    snprintf(s->text, sizeof(s->text), "#%-4d (%8.6fms): ", s->packet_seq, tstamp * 1000);

    s->idx = scan_eop(s, di);
    if (s->idx < 0) {
        const char *txts[] = {s->text, "...", NULL};
        pd_putx(s, di, 0, s->idx, ANN_TEXT, txts);
        return;
    }

    /* Packet header */
    s->head = get_short(s, di);
    char h_str[32], h_short[8];
    snprintf(h_str, sizeof(h_str), "H:%04X", s->head);
    snprintf(h_short, sizeof(h_short), "HD");
    const char *h_txts[] = {h_str, h_short, NULL};
    pd_putx(s, di, s->idx - 20, s->idx, ANN_HEADER, h_txts);
    puthead(s, di);

    /* Decode data payload */
    if (head_ext(s) == 1) {
        /* Extended header */
        s->ext_head = get_short(s, di);
        s->chunked = (s->ext_head >> 15) & 0x01;
        s->chunk_num = (s->ext_head >> 11) & 0x0F;
        s->req_chunk = (s->ext_head >> 10) & 0x01;
        s->data_size = s->ext_head & 0x01FF;

        char ex_str[32], ex_short[8];
        snprintf(ex_str, sizeof(ex_str), "Ext H:%04X", s->ext_head);
        snprintf(ex_short, sizeof(ex_short), "EXHD");
        const char *ex_txts[] = {ex_str, ex_short, NULL};
        pd_putx(s, di, s->idx - 20, s->idx, ANN_HEADER, ex_txts);

        /* Extended header payload annotation */
        char ext_info[128];
        snprintf(ext_info, sizeof(ext_info), "Chunked: %d  Chunk Num: %d  Req Chunk: %d  Data Size: %d",
                 s->chunked, s->chunk_num, s->req_chunk, s->data_size);
        const char *ext_txts[] = {ext_info, ext_info, NULL};
        pd_putx(s, di, s->idx - 20, s->idx, ANN_PAYLOAD, ext_txts);

        /* Read data words */
        for (int i = 0; i < head_count(s); i++) {
            s->data[i] = get_word(s, di);
            char d_str[32], d_short[8];
            snprintf(d_str, sizeof(d_str), "[%d]%08x", i, s->data[i]);
            snprintf(d_short, sizeof(d_short), "D%d", i);
            const char *d_txts[] = {d_str, d_short, NULL};
            pd_putx(s, di, s->idx - 40, s->idx, ANN_DATA, d_txts);
        }
    } else {
        /* Control/Data message */
        for (int i = 0; i < head_count(s); i++) {
            s->data[i] = get_word(s, di);
            char d_str[32], d_short[8];
            snprintf(d_str, sizeof(d_str), "[%d]%08x", i, s->data[i]);
            snprintf(d_short, sizeof(d_short), "D%d", i);
            const char *d_txts[] = {d_str, d_short, NULL};
            pd_putx(s, di, s->idx - 40, s->idx, ANN_DATA, d_txts);
            putpayload(s, di, s->idx - 40, s->idx, i);
        }
    }

    /* CRC check */
    s->num_data = head_count(s);
    uint32_t crc = get_word(s, di);

    /* Compute CRC32 */
    uint8_t crc_buf[4 + 16 * 4];
    int crc_len = 0;
    crc_buf[crc_len++] = s->head & 0xFF;
    crc_buf[crc_len++] = (s->head >> 8) & 0xFF;
    for (int i = 0; i < s->num_data; i++) {
        crc_buf[crc_len++] = s->data[i] & 0xFF;
        crc_buf[crc_len++] = (s->data[i] >> 8) & 0xFF;
        crc_buf[crc_len++] = (s->data[i] >> 16) & 0xFF;
        crc_buf[crc_len++] = (s->data[i] >> 24) & 0xFF;
    }
    uint32_t ccrc = compute_crc32(crc_buf, crc_len);

    if (crc != ccrc) {
        char warn_str[64];
        snprintf(warn_str, sizeof(warn_str), "Bad CRC %08x != %08x", crc, ccrc);
        pd_putwarn(s, di, warn_str, "CRC!");
    }

    char crc_str[32], crc_short[16];
    snprintf(crc_str, sizeof(crc_str), "CRC:%08x", crc);
    snprintf(crc_short, sizeof(crc_short), "CRC");
    const char *crc_txts[] = {crc_str, crc_short, NULL};
    pd_putx(s, di, s->idx - 40, s->idx, ANN_CRC, crc_txts);

    /* End of Packet */
    if (s->num_bits >= s->idx + 5 && get_sym(s, di, s->idx, 0) == EOP_SYM) {
        const char *eop_txts[] = {"EOP", "E", NULL};
        pd_putx(s, di, s->idx, s->idx + 5, ANN_EOP, eop_txts);
        s->idx += 5;
    } else {
        pd_putwarn(s, di, "No EOP", "EOP!");
    }

    /* Full text */
    if (s->fulltext) {
        const char *fulltext_txts[] = {s->text, "...", NULL};
        pd_putx(s, di, 0, s->idx, ANN_TEXT, fulltext_txts);
    }

    /* Meta data for bitrate */
    if (s->num_edges >= 2) {
        uint64_t ss = s->edges[0];
        uint64_t es = s->edges[s->num_edges - 1];
        if (es > ss) {
            int64_t bitrate = (int64_t)((double)s->samplerate * s->num_bits / (double)(es - ss));
            c_put_meta_int(di, es, ss, s->out_bitrate, bitrate);
        }
        /* Raw binary data */
        c_put_bin(di, es, ss, s->out_binary, 0, s->num_bits, s->bits);
    }
}

static struct srd_channel usb_pd_channels[] = {
    {"cc1", "CC1", "Configuration Channel 1", 0, SRD_CHANNEL_SDATA, "dec_usb_power_delivery_chan_cc1"},
};

static struct srd_channel usb_pd_optional_channels[] = {
    {"cc2", "CC2", "Configuration Channel 2", 0, SRD_CHANNEL_SDATA, "dec_usb_power_delivery_opt_chan_cc2"},
};

static struct srd_decoder_option usb_pd_options[] = {
    {"fulltext", "dec_usb_power_delivery_opt_fulltext", "Full text decoding of packets", NULL, NULL},
};

static const char *usb_pd_ann_labels[][3] = {
    {"", "type", "Packet Type"},
    {"", "preamble", "Preamble"},
    {"", "sop", "Start of Packet"},
    {"", "header", "Header"},
    {"", "data", "Data"},
    {"", "crc", "Checksum"},
    {"", "eop", "End Of Packet"},
    {"", "sym", "4b5b symbols"},
    {"", "warnings", "Warnings"},
    {"", "src", "Source Message"},
    {"", "snk", "Sink Message"},
    {"", "payload", "Payload"},
    {"", "text", "Plain text"},
};

static const int usb_pd_row_4b5b_classes[] = {ANN_SYM};
static const int usb_pd_row_phase_classes[] = {ANN_PREAMBLE, ANN_SOP, ANN_HEADER, ANN_DATA, ANN_CRC, ANN_EOP};
static const int usb_pd_row_payload_classes[] = {ANN_PAYLOAD};
static const int usb_pd_row_type_classes[] = {ANN_TYPE, ANN_SRC, ANN_SNK};
static const int usb_pd_row_warnings_classes[] = {ANN_WARNINGS};
static const int usb_pd_row_text_classes[] = {ANN_TEXT};
static const struct srd_c_ann_row usb_pd_ann_rows[] = {
    {"4b5b", "Symbols", usb_pd_row_4b5b_classes, 1},
    {"phase", "Parts", usb_pd_row_phase_classes, 6},
    {"payload", "Payload", usb_pd_row_payload_classes, 1},
    {"type", "Type", usb_pd_row_type_classes, 3},
    {"warnings", "Warnings", usb_pd_row_warnings_classes, 1},
    {"text", "Full text", usb_pd_row_text_classes, 1},
};

static struct srd_decoder_binary usb_pd_binary[] = {
    {0, "raw-data", "RAW binary data"},
};

static const char *usb_pd_inputs[] = {"logic"};
static const char *usb_pd_outputs[] = {"usb_pd"};
static const char *usb_pd_tags[] = {"PC"};

static void usb_pd_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(usb_pd_state)));
    usb_pd_state *s = (usb_pd_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(usb_pd_state));
    s->out_ann = 0;
    s->out_python = -1;
    s->out_binary = -1;
    s->out_bitrate = -1;
    s->packet_seq = 0;
}

static void usb_pd_start(struct srd_decoder_inst *di)
{
    usb_pd_state *s = (usb_pd_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "usb_pd");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "usb_pd");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "usb_pd");
    s->out_bitrate = c_reg_meta(di, SRD_OUTPUT_META,
        "usb_pd", "i", "Bitrate", "Bitrate during the packet");

    const char *ft = c_opt_str(di, "fulltext", "no");
    s->fulltext = (ft && strcmp(ft, "yes") == 0);

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0) {
        s->maxbit = (uint64_t)(3.0 * UI_US * (double)s->samplerate / 1000000.0);
        s->threshold = (uint64_t)(THRESHOLD_US * (double)s->samplerate / 1000000.0);
    }
}

static void usb_pd_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    usb_pd_state *s = (usb_pd_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->samplerate > 0) {
            s->maxbit = (uint64_t)(3.0 * UI_US * (double)s->samplerate / 1000000.0);
            s->threshold = (uint64_t)(THRESHOLD_US * (double)s->samplerate / 1000000.0);
        }
    }
}

static void usb_pd_decode(struct srd_decoder_inst *di)
{
    usb_pd_state *s = (usb_pd_state *)c_decoder_get_private(di);
    /* Fallback samplerate */
    if (s->samplerate == 0)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0)
        return;

    if (s->maxbit == 0) {
        s->maxbit = (uint64_t)(3.0 * UI_US * (double)s->samplerate / 1000000.0);
        s->threshold = (uint64_t)(THRESHOLD_US * (double)s->samplerate / 1000000.0);
    }

    int has_cc2 = c_has_ch(di, CH_CC2);

    while (1) {
        /* Wait for CC1 edge, CC2 edge (if present), or timeout — single combined wait */
        int ret;
        if (has_cc2)
            ret = c_wait(di, CW_E(CH_CC1), CW_OR, CW_E(CH_CC2), CW_OR, CW_SKIP(s->samplerate / 1000), CW_END);
        else
            ret = c_wait(di, CW_E(CH_CC1), CW_OR, CW_SKIP(s->samplerate / 1000), CW_END);
        if (ret != SRD_OK)
            return;

        /* First sample of the packet */
        if (!s->startsample) {
            s->startsample = di_samplenum(di);
            s->previous = di_samplenum(di);
            continue;
        }

        uint64_t diff = di_samplenum(di) - s->previous;

        /* Large idle: end of packet */
        if (diff > s->maxbit) {
            /* Last edge of the packet */
            if (s->num_edges < 4096)
                s->edges[s->num_edges++] = s->previous;

            /* Export the packet */
            decode_packet(s, di);

            /* Reset for next packet */
            s->startsample = di_samplenum(di);
            s->num_bits = 0;
            s->num_edges = 0;
            s->half_one = 0;
            s->start_one = 0;
        } else {
            /* BMC decode */
            int is_zero = diff > s->threshold;
            if (is_zero && !s->half_one) {
                if (s->num_bits < 4096)
                    s->bits[s->num_bits++] = 0;
                if (s->num_edges < 4096)
                    s->edges[s->num_edges++] = s->previous;
            } else if (!is_zero && s->half_one) {
                if (s->num_bits < 4096)
                    s->bits[s->num_bits++] = 1;
                if (s->num_edges < 4096)
                    s->edges[s->num_edges++] = s->start_one;
                s->half_one = 0;
            } else if (!is_zero && !s->half_one) {
                s->half_one = 1;
                s->start_one = s->previous;
            } else {
                /* Invalid BMC sequence */
                if (s->num_bits < 4096)
                    s->bits[s->num_bits++] = 0;
                if (s->num_edges < 4096)
                    s->edges[s->num_edges++] = s->previous;
                s->half_one = 0;
            }
        }
        s->previous = di_samplenum(di);
    }
}

static void usb_pd_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder usb_power_delivery_c_decoder = {
    .id = "usb_power_delivery_c",
    .name = "USB PD(C)",
    .longname = "USB Power Delivery (C)",
    .desc = "USB Power Delivery protocol. (C implementation)",
    .license = "gplv2+",
    .channels = usb_pd_channels,
    .num_channels = 1,
    .optional_channels = usb_pd_optional_channels,
    .num_optional_channels = 1,
    .options = usb_pd_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = usb_pd_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = usb_pd_ann_rows,
    .reset = usb_pd_reset,
    .start = usb_pd_start,
    .decode = usb_pd_decode,
    .metadata = usb_pd_metadata,
    .destroy = usb_pd_destroy,
    .state_size = 0,
    .inputs = usb_pd_inputs,
    .num_inputs = 1,
    .outputs = usb_pd_outputs,
    .num_outputs = 1,
    .tags = usb_pd_tags,
    .num_tags = 1,
    .binary = usb_pd_binary,
    .num_binary = 1,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    usb_pd_options[0].def = g_variant_new_string("no");
    {
        GVariant *v0 = g_variant_new_string("yes");
        GVariant *v1 = g_variant_new_string("no");
        GSList *vals = g_slist_append(NULL, v0);
        vals = g_slist_append(vals, v1);
        usb_pd_options[0].values = vals;
    }
    return &usb_power_delivery_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}