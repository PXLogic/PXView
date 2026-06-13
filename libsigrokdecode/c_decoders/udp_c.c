/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2021 original Python version
 * Copyright (C) 2024 C port
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* Annotations */
enum {
    ANN_HEADER = 0,
    ANN_DATA,
    NUM_ANN,
};

/* Decoder private state */
typedef struct {
    int out_ann;
    int out_python;
    int out_binary;
    uint64_t ss_block;
    uint64_t es_block;
    int format; /* 0=hex, 1=ascii, 2=dec, 3=oct, 4=bin */
} udp_state;

/* --- Helper to read block ss/es --- */

static uint64_t blk_ss(const uint8_t *blocks_data, int idx)
{
    uint64_t v;
    memcpy(&v, blocks_data + idx * 16, 8);
    return v;
}

static uint64_t blk_es(const uint8_t *blocks_data, int idx)
{
    uint64_t v;
    memcpy(&v, blocks_data + idx * 16 + 8, 8);
    return v;
}

/* --- Decoder metadata --- */

static const char *udp_inputs[] = {"ipv4", NULL};
static const char *udp_outputs[] = {"udp", NULL};
static const char *udp_tags[] = {"Networking", "PC", NULL};

static struct srd_decoder_option udp_options[] = {
    {"format", NULL, "Data format", NULL, NULL},
};

static const char *udp_ann_labels[][3] = {
    {"", "header", "Decoded header"},
    {"", "data", "Decoded data"},
};

static const int row_headers_classes[] = {ANN_HEADER, -1};
static const int row_datas_classes[] = {ANN_DATA, -1};

static const struct srd_c_ann_row udp_ann_rows[] = {
    {"headers", "Headers", row_headers_classes, 1},
    {"datas", "Datas", row_datas_classes, 1},
};

static const struct srd_decoder_binary udp_binary[] = {
    {0, "raw", "Raw UDP payload"},
};

/* --- Core recv_proto --- */

static void udp_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    udp_state *s = (udp_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "IP_PAYLOAD") != 0 || !fields || n_fields < 4)
        return;

    /* Parse IP_PAYLOAD from c_field array:
     *   fields[0] = C_U16(payload_len)
     *   fields[1] = C_BYTES(payload, payload_len)
     *   fields[2] = C_U16(block_count)
     *   fields[3] = C_BYTES(blocks, blocks_size)
     *   fields[4] = C_BYTES(src_ip, 4)
     *   fields[5] = C_BYTES(dst_ip, 4)
     */
    uint16_t payload_len = fields[0].u16;
    const uint8_t *payload = fields[1].bytes.data;
    /* payload_len field may differ from actual bytes length */
    if (fields[1].bytes.len < payload_len)
        payload_len = (uint16_t)fields[1].bytes.len;

    uint16_t block_count = fields[2].u16;
    const uint8_t *blocks_data = fields[3].bytes.data;

    uint8_t src_ip[4], dst_ip[4];
    if (n_fields >= 6 && fields[4].bytes.len >= 4 && fields[5].bytes.len >= 4) {
        memcpy(src_ip, fields[4].bytes.data, 4);
        memcpy(dst_ip, fields[5].bytes.data, 4);
    } else {
        memset(src_ip, 0, 4);
        memset(dst_ip, 0, 4);
    }

    /* UDP header needs at least 8 bytes */
    if (payload_len < 8 || block_count < 8)
        return;

    /* Parse UDP header: >4H */
    uint16_t src_port = ((uint16_t)payload[0] << 8) | payload[1];
    uint16_t dst_port = ((uint16_t)payload[2] << 8) | payload[3];
    uint16_t udp_length = ((uint16_t)payload[4] << 8) | payload[5];
    

    char t[64], t2[48];

    /* Source port */
    s->ss_block = blk_ss(blocks_data, 0);
    s->es_block = blk_es(blocks_data, 1);
    snprintf(t, sizeof(t), "Source Port: %d", src_port);
    snprintf(t2, sizeof(t2), "Src Port: %d", src_port);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

    /* Destination port */
    s->ss_block = blk_ss(blocks_data, 2);
    s->es_block = blk_es(blocks_data, 3);
    snprintf(t, sizeof(t), "Destination Port: %d", dst_port);
    snprintf(t2, sizeof(t2), "Dst Port: %d", dst_port);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

    /* Length */
    s->ss_block = blk_ss(blocks_data, 4);
    s->es_block = blk_es(blocks_data, 5);
    snprintf(t, sizeof(t), "Length: %d bytes", udp_length);
    snprintf(t2, sizeof(t2), "Len: %d", udp_length);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

    /* Verify checksum using IPv4 pseudo header */
    /* Pseudo header: src_ip(4) + dst_ip(4) + 0x00 + 0x11(UDP) + udp_length(2) */
    uint32_t sum = 0;
    /* Pseudo header */
    for (int i = 0; i < 4; i += 2)
        sum += ((uint32_t)src_ip[i] << 8) | src_ip[i + 1];
    for (int i = 0; i < 4; i += 2)
        sum += ((uint32_t)dst_ip[i] << 8) | dst_ip[i + 1];
    sum += 0x0011; /* protocol = UDP = 17 */
    sum += udp_length;
    /* UDP data */
    int udp_data_len = (udp_length <= payload_len) ? udp_length : payload_len;
    for (int i = 0; i < udp_data_len; i += 2) {
        if (i + 1 < udp_data_len)
            sum += ((uint32_t)payload[i] << 8) | payload[i + 1];
        else
            sum += (uint32_t)payload[i] << 8;
    }
    sum = (sum + (sum >> 16)) & 0xFFFF;
    const char *cs_str = (sum == 0xFFFF) ? "OK" : "FAILED";

    s->ss_block = blk_ss(blocks_data, 6);
    s->es_block = blk_es(blocks_data, 7);
    snprintf(t, sizeof(t), "Checksum: %s", cs_str);
    snprintf(t2, sizeof(t2), "Cksum: %s", cs_str);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

    /* UDP Payload annotations */
    int payload_start_idx = 8;
    int payload_end_idx = udp_length;
    if (payload_end_idx > (int)payload_len) payload_end_idx = payload_len;
    if (payload_end_idx > (int)block_count) payload_end_idx = block_count;

    for (int i = payload_start_idx; i < payload_end_idx; i++) {
        s->ss_block = blk_ss(blocks_data, i);
        s->es_block = blk_es(blocks_data, i);

        uint8_t b = payload[i];
        char data_str[16] = {0};

        switch (s->format) {
        case 1: /* ascii */
            if (b >= 0x20 && b < 0x7F)
                snprintf(data_str, sizeof(data_str), "'%c'", (char)b);
            else
                snprintf(data_str, sizeof(data_str), "[0x%02X]", b);
            break;
        case 2: /* dec */
            snprintf(data_str, sizeof(data_str), "%d", b);
            break;
        case 3: /* oct */
            snprintf(data_str, sizeof(data_str), "%o", b);
            break;
        case 4: /* bin */
            snprintf(data_str, sizeof(data_str), "0b");
            for (int bit = 7; bit >= 0; bit--) {
                int pos = (int)strlen(data_str);
                if (pos < (int)sizeof(data_str) - 2)
                    data_str[pos] = ((b >> bit) & 1) + '0';
            }
            break;
        default: /* hex */
            snprintf(data_str, sizeof(data_str), "0x%02X", b);
            break;
        }
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, data_str);
    }

    /* Push payload to stacked decoders */
    if (s->out_python >= 0 && payload_end_idx > payload_start_idx) {
        uint16_t plen = (uint16_t)(payload_end_idx - payload_start_idx);
        uint16_t bc = (uint16_t)(payload_end_idx - payload_start_idx);
        int buf_size = 2 + plen + 2 + bc * 16;
        uint8_t *buf = g_malloc(buf_size);
        int pos = 0;
        memcpy(buf + pos, &plen, 2); pos += 2;
        memcpy(buf + pos, payload + payload_start_idx, plen); pos += plen;
        memcpy(buf + pos, &bc, 2); pos += 2;
        for (int i = payload_start_idx; i < payload_end_idx; i++) {
            uint64_t ss_v = blk_ss(blocks_data, i);
            uint64_t es_v = blk_es(blocks_data, i);
            memcpy(buf + pos, &ss_v, 8); pos += 8;
            memcpy(buf + pos, &es_v, 8); pos += 8;
        }
        c_proto(di, blk_ss(blocks_data, payload_start_idx),
                             blk_es(blocks_data, payload_end_idx - 1),
                             s->out_python, "UDP_PAYLOAD", C_BYTES(buf, pos), C_END);
        g_free(buf);
    }

    /* Push payload to binary output */
    if (s->out_binary >= 0 && payload_end_idx > payload_start_idx) {
        c_put_bin(di, 0, 0, s->out_binary, 0,
                             payload_end_idx - payload_start_idx,
                             payload + payload_start_idx);
    }
}

/* --- Decoder lifecycle --- */

static void udp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(udp_state)));
    udp_state *s = (udp_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(udp_state));
    s->format = 0; /* hex default */
}

static void udp_start(struct srd_decoder_inst *di)
{
    udp_state *s = (udp_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "udp");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "udp");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "udp");

    const char *fmt = c_opt_str(di, "format", "hex");
    if (fmt) {
        if (strcmp(fmt, "ascii") == 0) s->format = 1;
        else if (strcmp(fmt, "dec") == 0) s->format = 2;
        else if (strcmp(fmt, "oct") == 0) s->format = 3;
        else if (strcmp(fmt, "bin") == 0) s->format = 4;
        else s->format = 0; /* hex */
    }
}

static void udp_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void udp_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder udp_c_decoder = {
    .id = "udp_c",
    .name = "UDP(C)",
    .longname = "User Datagram Protocol (C)",
    .desc = "UDP (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = udp_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = udp_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = udp_ann_rows,
    .inputs = udp_inputs,
    .num_inputs = 1,
    .outputs = udp_outputs,
    .num_outputs = 1,
    .binary = udp_binary,
    .num_binary = 1,
    .tags = udp_tags,
    .num_tags = 2,
    .reset = udp_reset,
    .start = udp_start,
    .decode = udp_decode,
    .destroy = udp_destroy,
    .decode_upper = udp_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    udp_options[0].def = g_variant_new_string("hex");
    GSList *fmt_vals = NULL;
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("ascii"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("dec"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("hex"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("oct"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("bin"));
    udp_options[0].values = fmt_vals;

    return &udp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}