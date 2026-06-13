/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Hattori, Hiroki <seagull.kamome@gmail.com>
 * Copyright (C) 2025 C port contributors
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

#define RX 0
#define TX 1

enum {
    ANN_RX_CMD = 0,
    ANN_RX_ACL,
    ANN_RX_SCO,
    ANN_RX_EVENT,
    ANN_RX_ERROR,
    ANN_RX_NEGO,
    ANN_RX_JUNK,
    ANN_RX_DESC,
    ANN_RX_BIN,
    ANN_TX_CMD,
    ANN_TX_ACL,
    ANN_TX_SCO,
    ANN_TX_EVENT,
    ANN_TX_ERROR,
    ANN_TX_NEGO,
    ANN_TX_JUNK,
    ANN_TX_DESC,
    ANN_TX_BIN,
    NUM_ANN,
};

enum h4_state {
    H4_WAIT_PKT_TYPE,
    H4_WAIT_HEADER,
    H4_WAIT_PAYLOAD,
};

typedef struct {
    int out_ann;
    int out_proto;
    enum h4_state state[2];
    uint8_t pkt_type[2];
    uint8_t data[2][1024];
    int data_len[2];
    int pkt_length[2];
    uint64_t ss_block[2];
    uint64_t es;
} bluetooth_h4_state;

static const char *bluetooth_h4_inputs[] = {"uart", NULL};
static const char *bluetooth_h4_outputs[] = {"bluetooth_h4", NULL};
static const char *bluetooth_h4_tags[] = {"Embedded/bluetooth", NULL};

static const char *bluetooth_h4_ann_labels[][3] = {
    {"", "rx-cmd", "RX Command packet"},
    {"", "rx-acl", "RX ACL data packet"},
    {"", "rx-sco", "RX SCO data packet"},
    {"", "rx-event", "RX Event data packet"},
    {"", "rx-error", "RX Error message packet"},
    {"", "rx-nego", "RX Negotiation packet"},
    {"", "rx-junk", "RX Garbages"},
    {"", "rx-desc", "RX packet description"},
    {"", "rx-bin", "RX packet binary"},
    {"", "tx-cmd", "TX Command packet"},
    {"", "tx-acl", "TX ACL data packet"},
    {"", "tx-sco", "TX SCO data packet"},
    {"", "tx-event", "TX Event data packet"},
    {"", "tx-error", "TX Error message packet"},
    {"", "tx-nego", "TX Negotiation packet"},
    {"", "tx-junk", "TX Garbages"},
    {"", "tx-desc", "TX packet description"},
    {"", "tx-bin", "TX packet binary"},
};

static const int bluetooth_h4_row_rx_classes[] = {
    ANN_RX_CMD, ANN_RX_ACL, ANN_RX_SCO, ANN_RX_EVENT,
    ANN_RX_ERROR, ANN_RX_NEGO, ANN_RX_JUNK, -1
};
static const int bluetooth_h4_row_rx_bins_classes[] = {ANN_RX_BIN, -1};
static const int bluetooth_h4_row_tx_classes[] = {
    ANN_TX_CMD, ANN_TX_ACL, ANN_TX_SCO, ANN_TX_EVENT,
    ANN_TX_ERROR, ANN_TX_NEGO, ANN_TX_JUNK, -1
};
static const int bluetooth_h4_row_tx_bins_classes[] = {ANN_TX_BIN, -1};

static const struct srd_c_ann_row bluetooth_h4_ann_rows[] = {
    {"rx", "RX", bluetooth_h4_row_rx_classes, 7},
    {"rx-bins", "RX binary", bluetooth_h4_row_rx_bins_classes, 1},
    {"tx", "TX", bluetooth_h4_row_tx_classes, 7},
    {"tx-bins", "TX binary", bluetooth_h4_row_tx_bins_classes, 1},
};

static const char *hci_cmd_name(uint16_t opcode)
{
    switch (opcode) {
    case 0x0401: return "Inquiry";
    case 0x0402: return "Inquiry_Cancel";
    case 0x0405: return "Create_Connection";
    case 0x0406: return "Disconnect";
    case 0x0C01: return "Set_Event_Mask";
    case 0x0C03: return "Reset";
    case 0x0C05: return "Set_Event_Filter";
    case 0x0C08: return "Flush";
    case 0x1001: return "Read_Local_Version_Information";
    case 0x1002: return "Read_Local_Supported_Commands";
    case 0x1003: return "Read_Supported_Features";
    case 0x1005: return "Read_Buffer_Size";
    case 0x1009: return "Read_BD_ADDR";
    default: return NULL;
    }
}

static void bluetooth_h4_output_packet(struct srd_decoder_inst *di,
    bluetooth_h4_state *s, int rxtx)
{
    const char *dir = (rxtx == RX) ? "RX" : "TX";
    int base_ann = rxtx * 9;
    char t[512];
    uint8_t *d = s->data[rxtx];
    int len = s->data_len[rxtx];

    if (s->pkt_type[rxtx] == 0x01) {
        /* HCI Command */
        uint16_t opcode = d[1] | (d[2] << 8);
        const char *name = hci_cmd_name(opcode);
        if (len >= 4) {
            if (name)
                snprintf(t, sizeof(t), "%s: CMD=%04X [%s] LEN=%d(%02Xh)",
                         dir, opcode, name, d[3], d[3]);
            else
                snprintf(t, sizeof(t), "%s: CMD=%04X LEN=%d(%02Xh)",
                         dir, opcode, d[3], d[3]);
        } else {
            snprintf(t, sizeof(t), "%s: CMD (incomplete)", dir);
        }
        c_put(di, s->ss_block[rxtx], s->es, s->out_ann, base_ann + 0, t);
    } else if (s->pkt_type[rxtx] == 0x02) {
        /* ACL Data */
        if (len >= 5) {
            uint16_t pkt_len = d[3] | (d[4] << 8);
            snprintf(t, sizeof(t), "%s: ACL H=%02X%02X LEN=%d(%02X%02Xh)",
                     dir, d[2], d[1], pkt_len, d[4], d[3]);
        } else {
            snprintf(t, sizeof(t), "%s: ACL (incomplete)", dir);
        }
        c_put(di, s->ss_block[rxtx], s->es, s->out_ann, base_ann + 1, t);
    } else if (s->pkt_type[rxtx] == 0x03) {
        /* SCO Data */
        if (len >= 4) {
            snprintf(t, sizeof(t), "%s: SCO H=%02X%02X LEN=%d(%02Xh)",
                     dir, d[2], d[1], d[3], d[3]);
        } else {
            snprintf(t, sizeof(t), "%s: SCO (incomplete)", dir);
        }
        c_put(di, s->ss_block[rxtx], s->es, s->out_ann, base_ann + 2, t);
    } else if (s->pkt_type[rxtx] == 0x04) {
        /* HCI Event */
        if (len >= 3) {
            snprintf(t, sizeof(t), "%s: EVENT=%02X LEN=%d(%02Xh)",
                     dir, d[1], d[2], d[2]);
        } else {
            snprintf(t, sizeof(t), "%s: EVENT (incomplete)", dir);
        }
        c_put(di, s->ss_block[rxtx], s->es, s->out_ann, base_ann + 3, t);
    }

    /* Reset state */
    s->state[rxtx] = H4_WAIT_PKT_TYPE;
    s->data_len[rxtx] = 0;
    s->pkt_length[rxtx] = -1;
    s->ss_block[rxtx] = 0;
}

static void bluetooth_h4_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    bluetooth_h4_state *s = (bluetooth_h4_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;
    int rxtx = fields[1].u8; /* 0=RX, 1=TX */

    if (rxtx != RX && rxtx != TX)
        rxtx = RX;

    s->es = end_sample;

    switch (s->state[rxtx]) {
    case H4_WAIT_PKT_TYPE:
        if (byte_val < 0x01 || byte_val > 0x04) {
            /* Invalid packet type - junk */
            char t[32];
            snprintf(t, sizeof(t), "%02X ", byte_val);
            c_put(di, start_sample, end_sample, s->out_ann, 6 + rxtx * 9, t);
            return;
        }
        s->pkt_type[rxtx] = byte_val;
        s->data[rxtx][0] = byte_val;
        s->data_len[rxtx] = 1;
        s->pkt_length[rxtx] = -1;
        s->ss_block[rxtx] = start_sample;
        s->state[rxtx] = H4_WAIT_HEADER;
        break;

    case H4_WAIT_HEADER:
        if (s->data_len[rxtx] < (int)sizeof(s->data[rxtx]))
            s->data[rxtx][s->data_len[rxtx]++] = byte_val;

        if (s->pkt_type[rxtx] == 0x01) {
            /* Command: need 4 bytes total (type + opcode_lo + opcode_hi + len) */
            if (s->data_len[rxtx] >= 4) {
                s->pkt_length[rxtx] = s->data[rxtx][3];
                s->state[rxtx] = H4_WAIT_PAYLOAD;
            }
        } else if (s->pkt_type[rxtx] == 0x02) {
            /* ACL: need 5 bytes total (type + handle_lo + handle_hi + len_lo + len_hi) */
            if (s->data_len[rxtx] >= 5) {
                s->pkt_length[rxtx] = s->data[rxtx][3] | (s->data[rxtx][4] << 8);
                s->state[rxtx] = H4_WAIT_PAYLOAD;
            }
        } else if (s->pkt_type[rxtx] == 0x03) {
            /* SCO: need 4 bytes total (type + handle_lo + handle_hi + len) */
            if (s->data_len[rxtx] >= 4) {
                s->pkt_length[rxtx] = s->data[rxtx][3];
                s->state[rxtx] = H4_WAIT_PAYLOAD;
            }
        } else if (s->pkt_type[rxtx] == 0x04) {
            /* Event: need 3 bytes total (type + event_code + len) */
            if (s->data_len[rxtx] >= 3) {
                s->pkt_length[rxtx] = s->data[rxtx][2];
                s->state[rxtx] = H4_WAIT_PAYLOAD;
            }
        }
        break;

    case H4_WAIT_PAYLOAD:
        if (s->data_len[rxtx] < (int)sizeof(s->data[rxtx]))
            s->data[rxtx][s->data_len[rxtx]++] = byte_val;
        if (s->pkt_length[rxtx] > 0)
            s->pkt_length[rxtx]--;
        if (s->pkt_length[rxtx] == 0) {
            bluetooth_h4_output_packet(di, s, rxtx);
        }
        break;
    }
}

static void bluetooth_h4_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(bluetooth_h4_state)));
    bluetooth_h4_state *s = (bluetooth_h4_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(bluetooth_h4_state));
    s->state[RX] = H4_WAIT_PKT_TYPE;
    s->state[TX] = H4_WAIT_PKT_TYPE;
    s->pkt_length[RX] = -1;
    s->pkt_length[TX] = -1;
}

static void bluetooth_h4_start(struct srd_decoder_inst *di)
{
    bluetooth_h4_state *s = (bluetooth_h4_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "bluetooth_h4");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "bluetooth_h4");
}

static void bluetooth_h4_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void bluetooth_h4_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder bluetooth_h4_c_decoder = {
    .id = "bluetooth_h4_c",
    .name = "Bluetooth H4(C)",
    .longname = "Bluetooth H4 UART protocol (C)",
    .desc = "Bluetooth H4 UART packet decoder. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = bluetooth_h4_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = bluetooth_h4_ann_rows,
    .inputs = bluetooth_h4_inputs,
    .num_inputs = 1,
    .outputs = bluetooth_h4_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = bluetooth_h4_tags,
    .num_tags = 1,
    .reset = bluetooth_h4_reset,
    .start = bluetooth_h4_start,
    .decode = bluetooth_h4_decode,
    .destroy = bluetooth_h4_destroy,
    .decode_upper = bluetooth_h4_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &bluetooth_h4_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}