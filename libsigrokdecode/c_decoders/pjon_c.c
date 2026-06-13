/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Gerhard Sittig <gerhard.sittig@gmx.net>
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

enum {
    ANN_RX_INFO = 0,
    ANN_HDR_CFG,
    ANN_PKT_LEN,
    ANN_META_CRC,
    ANN_TX_INFO,
    ANN_SVC_ID,
    ANN_PKT_ID,
    ANN_ANON_DATA,
    ANN_PAYLOAD,
    ANN_END_CRC,
    ANN_SYN_RSP,
    ANN_RELATION,
    ANN_WARN,
    NUM_ANN,
};

#define PJON_MAX_FRAME_BYTES 65536
#define PJON_MAX_FIELDS 16

enum {
    STATE_IDLE,
    STATE_COLLECTING,
    STATE_ACK,
};

typedef struct {
    int out_ann;
    /* Frame state */
    uint8_t *frame_bytes;
    int frame_byte_count;
    int frame_byte_cap;
    uint64_t frame_ss;
    uint64_t frame_es;
    /* Config flags */
    uint8_t cfg_shared, cfg_tx_info, cfg_sync_ack, cfg_async_ack;
    uint8_t cfg_port, cfg_crc32, cfg_len16, cfg_pkt_id;
    int cfg_overhead;
    /* Field scanning */
    int field_idx;
    int field_got;
    int field_widths[PJON_MAX_FIELDS];
    int field_ann_classes[PJON_MAX_FIELDS];
    int num_fields;
    /* Annotation position */
    uint64_t ann_ss;
    uint64_t ann_es;
    /* ACK state */
    uint8_t ack_bytes[4];
    int ack_byte_count;
    /* Relation tracking */
    int frame_rx_id;
    char frame_rx_id_text[32];
    int frame_tx_id;
    char frame_tx_id_text[32];
    char *frame_payload_text;
    int frame_has_ack;
    /* State */
    int state;
} pjon_state;

static const char *pjon_inputs[] = {"pjon_link", NULL};
static const char *pjon_tags[] = {"Embedded/industrial", NULL};

static const char *pjon_ann_labels[][3] = {
    {"", "rx_info", "Receiver ID"},
    {"", "hdr_cfg", "Header config"},
    {"", "pkt_len", "Packet length"},
    {"", "meta_crc", "Meta CRC"},
    {"", "tx_info", "Sender ID"},
    {"", "port", "Service ID"},
    {"", "pkt_id", "Packet ID"},
    {"", "anon", "Anonymous data"},
    {"", "payload", "Payload"},
    {"", "end_crc", "End CRC"},
    {"", "syn_rsp", "Sync response"},
    {"", "relation", "Relation"},
    {"", "warning", "Warning"},
};

static const int pjon_row_fields_classes[] = {
    ANN_RX_INFO, ANN_HDR_CFG, ANN_PKT_LEN, ANN_META_CRC, ANN_TX_INFO,
    ANN_SVC_ID, ANN_ANON_DATA, ANN_PAYLOAD, ANN_END_CRC, ANN_SYN_RSP, -1
};
static const int pjon_row_relations_classes[] = {ANN_RELATION, -1};
static const int pjon_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row pjon_ann_rows[] = {
    {"fields", "Fields", pjon_row_fields_classes, 10},
    {"relations", "Relations", pjon_row_relations_classes, 1},
    {"warnings", "Warnings", pjon_row_warnings_classes, 1},
};

static uint8_t calc_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            int odd = crc & 1;
            crc >>= 1;
            if (odd) crc ^= 0x97;
        }
    }
    return crc;
}

static uint32_t calc_crc32(const uint8_t *data, int len)
{
    uint32_t crc = 0xffffffff;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            int odd = crc & 1;
            crc >>= 1;
            if (odd) crc ^= 0xedb88320;
        }
    }
    return crc ^ 0xffffffff;
}

static void pjon_frame_flush(struct srd_decoder_inst *di, pjon_state *s)
{
    if (!s->frame_bytes || s->frame_byte_count == 0)
        return;
    if (!s->frame_ss || !s->frame_es)
        return;

    char text[512];
    int pos = 0;

    if (s->frame_rx_id >= 0)
        pos += snprintf(text + pos, sizeof(text) - pos, "RX %s", s->frame_rx_id_text);
    if (s->frame_tx_id >= 0)
        pos += snprintf(text + pos, sizeof(text) - pos, "%sTX %s",
                        pos > 0 ? " - " : "", s->frame_tx_id_text);
    if (s->frame_payload_text)
        pos += snprintf(text + pos, sizeof(text) - pos, "%sDATA %s",
                        pos > 0 ? " - " : "", s->frame_payload_text);
    if (s->frame_has_ack >= 0)
        pos += snprintf(text + pos, sizeof(text) - pos, "%sACK %02x",
                        pos > 0 ? " - " : "", s->frame_has_ack);

    if (pos > 0)
        c_put(di, s->frame_ss, s->frame_es, s->out_ann, ANN_RELATION, text);
}

static void pjon_reset_frame(pjon_state *s)
{
    s->frame_byte_count = 0;
    s->frame_ss = 0;
    s->frame_es = 0;
    s->frame_rx_id = -1;
    s->frame_rx_id_text[0] = '\0';
    s->frame_tx_id = -1;
    s->frame_tx_id_text[0] = '\0';
    if (s->frame_payload_text) {
        g_free(s->frame_payload_text);
        s->frame_payload_text = NULL;
    }
    s->frame_has_ack = -1;
    s->ack_byte_count = 0;
    s->ann_ss = 0;
    s->ann_es = 0;
}

static void pjon_seed_fields(pjon_state *s)
{
    s->num_fields = 0;
    /* Field 0: RX ID (1 byte) */
    s->field_widths[s->num_fields] = 1;
    s->field_ann_classes[s->num_fields] = ANN_RX_INFO;
    s->num_fields++;
    /* Field 1: Header Config (1 byte) */
    s->field_widths[s->num_fields] = 1;
    s->field_ann_classes[s->num_fields] = ANN_HDR_CFG;
    s->num_fields++;

    s->field_idx = 0;
    s->field_got = 0;
}

static void pjon_handle_field_rx_id(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width)
{
    (void)width;
    uint8_t b = raw[0];
    const char *id_txt;

    if (b == 255)
        id_txt = "NA";
    else if (b == 0)
        id_txt = "BC";
    else {
        snprintf(s->frame_rx_id_text, sizeof(s->frame_rx_id_text), "%d", b);
        id_txt = s->frame_rx_id_text;
    }
    s->frame_rx_id = b;

    char buf[64];
    snprintf(buf, sizeof(buf), "RX_ID %s", id_txt);
    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_RX_INFO, buf);
}

static void pjon_handle_field_config(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width)
{
    (void)width;
    uint8_t b = raw[0];

    s->cfg_shared = b & (1 << 0);
    s->cfg_tx_info = b & (1 << 1);
    s->cfg_sync_ack = b & (1 << 2);
    s->cfg_async_ack = b & (1 << 3);
    s->cfg_port = b & (1 << 4);
    s->cfg_crc32 = b & (1 << 5);
    s->cfg_len16 = b & (1 << 6);
    s->cfg_pkt_id = b & (1 << 7);

    /* Text representation of flags */
    char text[128];
    int pos = 0;
    pos += snprintf(text + pos, sizeof(text) - pos, "CFG ");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_pkt_id ? "pkt_id" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_len16 ? "len16" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_crc32 ? "crc32" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_port ? "svc_id" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_async_ack ? "ack_mode" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_sync_ack ? "ack" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s ", s->cfg_tx_info ? "tx_info" : "-");
    pos += snprintf(text + pos, sizeof(text) - pos, "%s", s->cfg_shared ? "bus_id" : "-");

    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_HDR_CFG, text);

    /* Calculate overhead */
    int len_size = s->cfg_len16 ? 2 : 1;
    int crc_size = s->cfg_crc32 ? 4 : 1;
    s->cfg_overhead = 1 + 1 + len_size + 1; /* RX ID + HDR CFG + PKT LEN + META CRC */
    if (s->cfg_shared)
        s->cfg_overhead += 4; /* RX bus ID */
    if (s->cfg_tx_info) {
        if (s->cfg_shared)
            s->cfg_overhead += 4; /* TX bus ID */
        s->cfg_overhead += 1; /* TX ID */
    }
    if (s->cfg_port)
        s->cfg_overhead += 2; /* Service ID */
    if (s->cfg_pkt_id)
        s->cfg_overhead += 2; /* Packet ID */
    s->cfg_overhead += crc_size; /* End CRC */

    /* Register remaining fields */
    /* Packet length */
    s->field_widths[s->num_fields] = len_size;
    s->field_ann_classes[s->num_fields] = ANN_PKT_LEN;
    s->num_fields++;
    /* Meta CRC */
    s->field_widths[s->num_fields] = 1;
    s->field_ann_classes[s->num_fields] = ANN_META_CRC;
    s->num_fields++;
    /* Optional: RX bus ID */
    if (s->cfg_shared) {
        s->field_widths[s->num_fields] = 4;
        s->field_ann_classes[s->num_fields] = ANN_ANON_DATA;
        s->num_fields++;
    }
    /* Optional: TX bus ID + TX ID */
    if (s->cfg_tx_info) {
        if (s->cfg_shared) {
            s->field_widths[s->num_fields] = 4;
            s->field_ann_classes[s->num_fields] = ANN_ANON_DATA;
            s->num_fields++;
        }
        s->field_widths[s->num_fields] = 1;
        s->field_ann_classes[s->num_fields] = ANN_ANON_DATA;
        s->num_fields++;
    }
    /* Optional: Service ID */
    if (s->cfg_port) {
        s->field_widths[s->num_fields] = 2;
        s->field_ann_classes[s->num_fields] = ANN_ANON_DATA;
        s->num_fields++;
    }
    /* Optional: Packet ID */
    if (s->cfg_pkt_id) {
        s->field_widths[s->num_fields] = 2;
        s->field_ann_classes[s->num_fields] = ANN_ANON_DATA;
        s->num_fields++;
    }
    /* Payload (placeholder, width updated after pkt_len is known) */
    s->field_widths[s->num_fields] = 0;
    s->field_ann_classes[s->num_fields] = ANN_PAYLOAD;
    s->num_fields++;
    /* End CRC */
    s->field_widths[s->num_fields] = crc_size;
    s->field_ann_classes[s->num_fields] = ANN_END_CRC;
    s->num_fields++;

    /* Warnings for invalid flag combinations */
    int wants_ack = s->cfg_sync_ack || s->cfg_async_ack;
    int is_broadcast = (s->frame_rx_id == 0);
    char warn_buf[256];
    int warn_pos = 0;

    if (wants_ack && !s->cfg_tx_info)
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%sACK request without TX info", warn_pos > 0 ? ", " : "");
    if (wants_ack && is_broadcast)
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%sACK request for broadcast", warn_pos > 0 ? ", " : "");
    if (s->cfg_sync_ack && s->cfg_async_ack)
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%ssync and async ACK request", warn_pos > 0 ? ", " : "");
    if (s->cfg_len16 && !s->cfg_crc32)
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%sextended length needs CRC32", warn_pos > 0 ? ", " : "");

    if (warn_pos > 0)
        c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_WARN, warn_buf);
}

static void pjon_handle_field_pkt_len(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width)
{
    int pkt_len;
    if (width == 2)
        pkt_len = (raw[0] << 8) | raw[1];
    else
        pkt_len = raw[0];

    int pl_len = pkt_len - s->cfg_overhead;

    char warn_buf[256];
    int warn_pos = 0;
    if (pkt_len < s->cfg_overhead || pkt_len >= 65536)
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%ssuspicious packet length", warn_pos > 0 ? ", " : "");
    if (pkt_len > 15 && !s->cfg_crc32)
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%slength above 15 needs CRC32", warn_pos > 0 ? ", " : "");
    if (pl_len < 1) {
        warn_pos += snprintf(warn_buf + warn_pos, sizeof(warn_buf) - warn_pos,
                             "%ssuspicious payload length", warn_pos > 0 ? ", " : "");
        pl_len = 0;
    }
    if (warn_pos > 0)
        c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_WARN, warn_buf);

    /* Update payload field width (second last field) */
    if (s->num_fields >= 2)
        s->field_widths[s->num_fields - 2] = pl_len;

    char buf[256];
    snprintf(buf, sizeof(buf), "LENGTH %d (PAYLOAD %d)", pkt_len, pl_len);
    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_PKT_LEN, buf);
}

static void pjon_handle_field_meta_crc(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width)
{
    (void)width;
    uint8_t have = raw[0];
    uint8_t want = calc_crc8(s->frame_bytes, s->frame_byte_count - 1);

    char buf[64];
    if (want != have) {
        snprintf(buf, sizeof(buf), "META_CRC mismatch - want %02x have %02x", want, have);
        c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_WARN, buf);
    }
    snprintf(buf, sizeof(buf), "META_CRC %02x", have);
    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_META_CRC, buf);
}

static void pjon_handle_field_end_crc(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width)
{
    char buf[64];
    if (width == 4) {
        uint32_t have = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) |
                        ((uint32_t)raw[2] << 8) | raw[3];
        int crc_data_len = s->frame_byte_count - 4;
        uint32_t want = calc_crc32(s->frame_bytes, crc_data_len > 0 ? crc_data_len : 0);
        if (want != have) {
            snprintf(buf, sizeof(buf), "END_CRC mismatch - want %08x have %08x", want, have);
            c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_WARN, buf);
        }
        snprintf(buf, sizeof(buf), "END_CRC %08x", have);
    } else {
        uint8_t have = raw[0];
        int crc_data_len = s->frame_byte_count - 1;
        uint8_t want = calc_crc8(s->frame_bytes, crc_data_len > 0 ? crc_data_len : 0);
        if (want != have) {
            snprintf(buf, sizeof(buf), "END_CRC mismatch - want %02x have %02x", want, have);
            c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_WARN, buf);
        }
        snprintf(buf, sizeof(buf), "END_CRC %02x", have);
    }
    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_END_CRC, buf);
}

static void pjon_handle_field_payload(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width)
{
    char *text = (char *)g_malloc(width * 4 + 1);
    int pos = 0;
    for (int i = 0; i < width; i++)
        pos += snprintf(text + pos, (width * 4 + 1) - pos, "%s%02x", i > 0 ? " " : "", raw[i]);

    if (s->frame_payload_text)
        g_free(s->frame_payload_text);
    s->frame_payload_text = g_strdup(text);

    char buf[512];
    snprintf(buf, sizeof(buf), "PAYLOAD %s", text);
    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_PAYLOAD, buf);
    g_free(text);
}

static void pjon_handle_field_anon(struct srd_decoder_inst *di, pjon_state *s,
    const uint8_t *raw, int width, int field_index)
{
    char buf[256];
    int pos = 0;
    for (int i = 0; i < width && pos < (int)sizeof(buf) - 4; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%02x", i > 0 ? " " : "", raw[i]);
    c_put(di, s->ann_ss, s->ann_es, s->out_ann, ANN_ANON_DATA, buf);

    /* Track TX ID if present */
    if (s->cfg_tx_info && !s->cfg_shared && width == 1 && field_index >= 4) {
        s->frame_tx_id = raw[0];
        snprintf(s->frame_tx_id_text, sizeof(s->frame_tx_id_text), "%d", raw[0]);
    }
}

static void pjon_process_field(struct srd_decoder_inst *di, pjon_state *s)
{
    if (s->field_idx >= s->num_fields)
        return;

    int w = s->field_widths[s->field_idx];
    if (w <= 0 || s->frame_byte_count < w)
        return;

    const uint8_t *raw = s->frame_bytes + s->frame_byte_count - w;
    (void)s->field_ann_classes[s->field_idx]; /* cls unused but keep for reference */

    switch (s->field_idx) {
    case 0: /* RX ID */
        pjon_handle_field_rx_id(di, s, raw, w);
        break;
    case 1: /* Header Config */
        pjon_handle_field_config(di, s, raw, w);
        break;
    case 2: /* Packet Length */
        pjon_handle_field_pkt_len(di, s, raw, w);
        break;
    case 3: /* Meta CRC */
        pjon_handle_field_meta_crc(di, s, raw, w);
        break;
    default:
        if (s->field_idx == s->num_fields - 1) {
            /* End CRC */
            pjon_handle_field_end_crc(di, s, raw, w);
        } else if (s->field_idx == s->num_fields - 2) {
            /* Payload */
            pjon_handle_field_payload(di, s, raw, w);
        } else {
            /* Anonymous data fields */
            pjon_handle_field_anon(di, s, raw, w, s->field_idx);
        }
        break;
    }

    s->field_idx++;
    s->field_got = 0;
}

static void pjon_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    pjon_state *s = (pjon_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "FRAME_INIT") == 0) {
        pjon_frame_flush(di, s);
        pjon_reset_frame(s);
        pjon_seed_fields(s);
        s->state = STATE_COLLECTING;
        s->frame_ss = start_sample;
        s->frame_es = end_sample;
    } else if (strcmp(cmd, "DATA_BYTE") == 0) {
        uint8_t b = (n_fields > 0) ? fields[0].u8 : 0;

        /* ACK collection mode */
        if (s->state == STATE_ACK) {
            s->ack_bytes[s->ack_byte_count++] = b;
            s->frame_has_ack = b;
            char buf[32];
            snprintf(buf, sizeof(buf), "ACK %02x", b);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_SYN_RSP, buf);
            return;
        }

        /* Frame collection mode */
        if (s->state == STATE_COLLECTING) {
            if (s->ann_ss == 0)
                s->ann_ss = start_sample;

            /* Grow buffer if needed */
            if (s->frame_byte_count >= s->frame_byte_cap) {
                int new_cap = s->frame_byte_cap ? s->frame_byte_cap * 2 : 64;
                s->frame_bytes = (uint8_t *)g_realloc(s->frame_bytes, new_cap);
                s->frame_byte_cap = new_cap;
            }
            s->frame_bytes[s->frame_byte_count++] = b;
            s->frame_es = end_sample;
            s->ann_es = end_sample;

            /* Check if current field is complete */
            if (s->field_idx < s->num_fields) {
                s->field_got++;
                if (s->field_got == s->field_widths[s->field_idx]) {
                    pjon_process_field(di, s);
                }
            }
        }
    } else if (strcmp(cmd, "SYNC_RESP_WAIT") == 0) {
        s->state = STATE_ACK;
        s->ack_byte_count = 0;
        s->ann_ss = 0;
        s->ann_es = 0;
    } else if (strcmp(cmd, "IDLE") == 0 || strcmp(cmd, "FRAME_DATA") == 0) {
        pjon_frame_flush(di, s);
        pjon_reset_frame(s);
        s->state = STATE_IDLE;
    }
}

static void pjon_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(pjon_state)));
    }
    pjon_state *s = (pjon_state *)c_decoder_get_private(di);
    if (s->frame_bytes)
        g_free(s->frame_bytes);
    if (s->frame_payload_text)
        g_free(s->frame_payload_text);
    memset(s, 0, sizeof(pjon_state));
    s->frame_rx_id = -1;
    s->frame_tx_id = -1;
    s->frame_has_ack = -1;
    s->state = STATE_IDLE;
}

static void pjon_start(struct srd_decoder_inst *di)
{
    pjon_state *s = (pjon_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pjon");
}

static void pjon_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void pjon_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        pjon_state *s = (pjon_state *)priv;
        if (s->frame_bytes)
            g_free(s->frame_bytes);
        if (s->frame_payload_text)
            g_free(s->frame_payload_text);
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder pjon_c_decoder = {
    .id = "pjon_c",
    .name = "PJON(C)",
    .longname = "PJON (C)",
    .desc = "The PJON protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = pjon_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = pjon_ann_rows,
    .inputs = pjon_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = pjon_tags,
    .num_tags = 1,
    .reset = pjon_reset,
    .start = pjon_start,
    .decode = pjon_decode,
    .destroy = pjon_destroy,
    .decode_upper = pjon_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &pjon_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}