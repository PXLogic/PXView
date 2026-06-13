/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2011 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
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

enum {
    ANN_SYNC_OK = 0,
    ANN_SYNC_ERR,
    ANN_PID,
    ANN_FRAMENUM,
    ANN_ADDR,
    ANN_EP,
    ANN_CRC5_OK,
    ANN_CRC5_ERR,
    ANN_DATA,
    ANN_CRC16_OK,
    ANN_CRC16_ERR,
    ANN_PKT_OUT,
    ANN_PKT_IN,
    ANN_PKT_SOF,
    ANN_PKT_SETUP,
    ANN_PKT_DATA0,
    ANN_PKT_DATA1,
    ANN_PKT_DATA2,
    ANN_PKT_MDATA,
    ANN_PKT_ACK,
    ANN_PKT_NAK,
    ANN_PKT_STALL,
    ANN_PKT_NYET,
    ANN_PKT_PRE,
    ANN_PKT_ERR,
    ANN_PKT_SPLIT,
    ANN_PKT_PING,
    ANN_PKT_RESERVED,
    ANN_PKT_INVALID,
    NUM_ANN,
};

#define USB_PKT_MAX_BITS 4096

enum usb_pkt_state {
    PKT_WAIT_SOP = 0,
    PKT_GET_BIT = 1,
};

enum pid_category {
    PID_CAT_TOKEN = 0,
    PID_CAT_DATA,
    PID_CAT_HANDSHAKE,
    PID_CAT_SPECIAL,
};

/* Decoder private state — C_DECODER_STATE auto-generates usb_pkt_s typedef,
 * usb_pkt_reset (calloc), and usb_pkt_destroy (free). */
C_DECODER_STATE(usb_pkt, {
    int state;
    char bits[USB_PKT_MAX_BITS];
    int bits_len;
    uint64_t bit_ss[USB_PKT_MAX_BITS];
    uint64_t bit_es[USB_PKT_MAX_BITS];
    uint64_t ss_packet;
    uint64_t es_packet;
    int out_ann;
    int out_proto;
})

static const char *usb_pkt_inputs[] = {"usb_signalling", NULL};
static const char *usb_pkt_outputs[] = {"usb_packet", NULL};
static const char *usb_pkt_tags[] = {"PC", NULL};

static struct srd_decoder_option usb_pkt_options[] = {
    {"signalling", "dec_usb_packet_opt_signalling", "Signalling", NULL, NULL},
};

static const char *usb_pkt_ann_labels[][3] = {
    {"", "sync-ok", "SYNC"},
    {"", "sync-err", "SYNC (error)"},
    {"", "pid", "PID"},
    {"", "framenum", "FRAMENUM"},
    {"", "addr", "ADDR"},
    {"", "ep", "EP"},
    {"", "crc5-ok", "CRC5"},
    {"", "crc5-err", "CRC5 (error)"},
    {"", "data", "DATA"},
    {"", "crc16-ok", "CRC16"},
    {"", "crc16-err", "CRC16 (error)"},
    {"", "packet-out", "Packet: OUT"},
    {"", "packet-in", "Packet: IN"},
    {"", "packet-sof", "Packet: SOF"},
    {"", "packet-setup", "Packet: SETUP"},
    {"", "packet-data0", "Packet: DATA0"},
    {"", "packet-data1", "Packet: DATA1"},
    {"", "packet-data2", "Packet: DATA2"},
    {"", "packet-mdata", "Packet: MDATA"},
    {"", "packet-ack", "Packet: ACK"},
    {"", "packet-nak", "Packet: NAK"},
    {"", "packet-stall", "Packet: STALL"},
    {"", "packet-nyet", "Packet: NYET"},
    {"", "packet-pre", "Packet: PRE"},
    {"", "packet-err", "Packet: ERR"},
    {"", "packet-split", "Packet: SPLIT"},
    {"", "packet-ping", "Packet: PING"},
    {"", "packet-reserved", "Packet: Reserved"},
    {"", "packet-invalid", "Packet: Invalid"},
};

static const int usb_pkt_row_fields_classes[] = {
    ANN_SYNC_OK, ANN_SYNC_ERR, ANN_PID, ANN_FRAMENUM, ANN_ADDR, ANN_EP,
    ANN_CRC5_OK, ANN_CRC5_ERR, ANN_DATA, ANN_CRC16_OK, ANN_CRC16_ERR, -1
};
static const int usb_pkt_row_packet_classes[] = {
    ANN_PKT_OUT, ANN_PKT_IN, ANN_PKT_SOF, ANN_PKT_SETUP,
    ANN_PKT_DATA0, ANN_PKT_DATA1, ANN_PKT_DATA2, ANN_PKT_MDATA,
    ANN_PKT_ACK, ANN_PKT_NAK, ANN_PKT_STALL, ANN_PKT_NYET,
    ANN_PKT_PRE, ANN_PKT_ERR, ANN_PKT_SPLIT, ANN_PKT_PING,
    ANN_PKT_RESERVED, ANN_PKT_INVALID, -1
};
static const struct srd_c_ann_row usb_pkt_ann_rows[] = {
    {"fields", "Packet fields", usb_pkt_row_fields_classes, 11},
    {"packet", "Packets", usb_pkt_row_packet_classes, 18},
};

/* PID lookup table */
static const struct { const char *bitstr; const char *name; int category; int pkt_ann; } pid_table[] = {
    /* Token */
    {"10000111", "OUT",    PID_CAT_TOKEN,     ANN_PKT_OUT},
    {"10010110", "IN",     PID_CAT_TOKEN,     ANN_PKT_IN},
    {"10100101", "SOF",    PID_CAT_TOKEN,     ANN_PKT_SOF},
    {"10110100", "SETUP",  PID_CAT_TOKEN,     ANN_PKT_SETUP},
    /* Data */
    {"11000011", "DATA0",  PID_CAT_DATA,      ANN_PKT_DATA0},
    {"11010010", "DATA1",  PID_CAT_DATA,      ANN_PKT_DATA1},
    {"11100001", "DATA2",  PID_CAT_DATA,      ANN_PKT_DATA2},
    {"11110000", "MDATA",  PID_CAT_DATA,      ANN_PKT_MDATA},
    /* Handshake */
    {"01001011", "ACK",    PID_CAT_HANDSHAKE, ANN_PKT_ACK},
    {"01011010", "NAK",    PID_CAT_HANDSHAKE, ANN_PKT_NAK},
    {"01111000", "STALL",  PID_CAT_HANDSHAKE, ANN_PKT_STALL},
    {"01101001", "NYET",   PID_CAT_HANDSHAKE, ANN_PKT_NYET},
    /* Special */
    {"00111100", "PRE",    PID_CAT_SPECIAL,   ANN_PKT_PRE},
    {"00011110", "SPLIT",  PID_CAT_SPECIAL,   ANN_PKT_SPLIT},
    {"00101101", "PING",   PID_CAT_SPECIAL,   ANN_PKT_PING},
    {"00001111", "Reserved", PID_CAT_SPECIAL, ANN_PKT_RESERVED},
};
#define NUM_PIDS (sizeof(pid_table) / sizeof(pid_table[0]))

/* CRC5 calculation for token packets */
static uint8_t usb_pkt_calc_crc5(const char *bitstr, int len)
{
    uint8_t poly5 = 0x25;
    uint8_t crc5 = 0x1f;
    for (int i = 0; i < len; i++) {
        crc5 <<= 1;
        if ((bitstr[i] - '0') != (crc5 >> 5))
            crc5 ^= poly5;
        crc5 &= 0x1f;
    }
    crc5 ^= 0x1f;
    /* Reverse bit order */
    uint8_t out = 0;
    for (int i = 0; i < 5; i++) {
        if (crc5 >> i & 1)
            out |= (1 << (4 - i));
    }
    return out;
}

/* CRC16 calculation for data packets */
static uint16_t usb_pkt_calc_crc16(const char *bitstr, int len)
{
    uint32_t poly16 = 0x18005;
    uint32_t crc16 = 0xffff;
    for (int i = 0; i < len; i++) {
        crc16 <<= 1;
        if ((uint32_t)(bitstr[i] - '0') != (crc16 >> 16))
            crc16 ^= poly16;
        crc16 &= 0xffff;
    }
    crc16 ^= 0xffff;
    /* Reverse bit order */
    uint16_t out = 0;
    for (int i = 0; i < 16; i++) {
        if (crc16 >> i & 1)
            out |= (1 << (15 - i));
    }
    return out;
}

/* Convert bits to byte (LSB first) */
static uint8_t usb_pkt_bits_to_byte(const char *bits, int start, int len)
{
    uint8_t val = 0;
    for (int i = 0; i < len && i < 8; i++) {
        if (bits[start + i] == '1')
            val |= (1 << i);
    }
    return val;
}

/* Convert bits to integer (LSB first) */
static uint32_t usb_pkt_bits_to_int(const char *bits, int start, int len)
{
    uint32_t val = 0;
    for (int i = 0; i < len && i < 32; i++) {
        if (bits[start + i] == '1')
            val |= (1 << i);
    }
    return val;
}

/* Find PID entry by bit string */
static int usb_pkt_find_pid(const char *bitstr8)
{
    for (int i = 0; i < (int)NUM_PIDS; i++) {
        if (strncmp(pid_table[i].bitstr, bitstr8, 8) == 0)
            return i;
    }
    return -1;
}

static void usb_pkt_handle_packet(struct srd_decoder_inst *di, usb_pkt_s *s)
{
    if (s->bits_len < 8) return;

    /* Check SYNC field (first 8 bits should be 00000001) */
    int sync_ok = 1;
    for (int i = 0; i < 7; i++) {
        if (s->bits[i] != '0') { sync_ok = 0; break; }
    }
    if (s->bits[7] != '1') sync_ok = 0;

    if (sync_ok) {
        c_put(di, s->bit_ss[0], s->bit_es[7], s->out_ann, ANN_SYNC_OK, "SYNC: 00000001", "SYNC", "S");
    } else {
        char sync_bits[9];
        for (int i = 0; i < 8; i++) sync_bits[i] = s->bits[i];
        sync_bits[8] = '\0';
        char t[48];
        snprintf(t, sizeof(t), "SYNC ERROR: %s", sync_bits);
        char t2[48];
        snprintf(t2, sizeof(t2), "SYNC ERR: %s", sync_bits);
        c_put(di, s->bit_ss[0], s->bit_es[7], s->out_ann, ANN_SYNC_ERR,
              t, t2, "SYNC ERR", "SE", "S");
    }

    if (s->bits_len < 16) {
        c_put(di, s->ss_packet, s->es_packet, s->out_ann, ANN_PKT_INVALID,
              "Invalid packet (shorter than 16 bits)");
        return;
    }

    /* If SYNC was bad, we still reported it but now return since we can't parse further */
    if (!sync_ok) {
        c_put(di, s->ss_packet, s->es_packet, s->out_ann, ANN_PKT_INVALID,
              "Invalid packet (SYNC error)", "Invalid (SYNC)", "Invalid", "Inv", "I");
        return;
    }

    /* Parse PID (bits 8-15) */
    int pid_idx = usb_pkt_find_pid(&s->bits[8]);
    if (pid_idx < 0) {
        char pid_str[16];
        for (int i = 0; i < 8; i++) pid_str[i] = s->bits[8 + i];
        pid_str[8] = '\0';
        char buf[32];
        snprintf(buf, sizeof(buf), "PID: %s (invalid)", pid_str);
        c_put(di, s->bit_ss[8], s->bit_es[15], s->out_ann, ANN_PID, buf);
        c_put(di, s->ss_packet, s->es_packet, s->out_ann, ANN_PKT_INVALID, "Invalid");
        return;
    }

    const char *pid_name = pid_table[pid_idx].name;
    int pid_cat = pid_table[pid_idx].category;
    int pkt_ann = pid_table[pid_idx].pkt_ann;

    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "PID: %s", pid_name);
    char short_pid[2] = {pid_name[0], '\0'};
    c_put(di, s->bit_ss[8], s->bit_es[15], s->out_ann, ANN_PID, pid_buf, pid_name, short_pid);

    if (pid_cat == PID_CAT_TOKEN) {
        /* Token packet: SYNC + PID + ADDR(7) + ENDP(4) + CRC5(5) */
        if (s->bits_len < 40) {
            c_put(di, s->ss_packet, s->es_packet, s->out_ann, ANN_PKT_INVALID, "Invalid");
            return;
        }

        if (strcmp(pid_name, "SOF") == 0) {
            uint32_t framenum = usb_pkt_bits_to_int(s->bits, 16, 11);
            char fn_buf[32];
            snprintf(fn_buf, sizeof(fn_buf), "Frame: %d", framenum);
            c_put(di, s->bit_ss[16], s->bit_es[26], s->out_ann, ANN_FRAMENUM,
                  fn_buf, "Frame", "Fr", "F");

            uint32_t crc5_rx = usb_pkt_bits_to_int(s->bits, 27, 5);
            uint8_t crc5_calc = usb_pkt_calc_crc5(&s->bits[8], 19);
            if (crc5_rx == crc5_calc) {
                char crc5_buf[32];
                snprintf(crc5_buf, sizeof(crc5_buf), "CRC5: 0x%02X", crc5_rx);
                c_put(di, s->bit_ss[27], s->bit_es[31], s->out_ann, ANN_CRC5_OK, crc5_buf, "CRC5", "C");
            } else {
                char crc5_buf[64];
                snprintf(crc5_buf, sizeof(crc5_buf), "CRC5 ERROR: 0x%02X", crc5_rx);
                c_put(di, s->bit_ss[27], s->bit_es[31], s->out_ann, ANN_CRC5_ERR, crc5_buf, "CRC5 ERR", "CE", "C");
            }

            char pkt_buf[64];
            snprintf(pkt_buf, sizeof(pkt_buf), "SOF %d", framenum);
            c_put(di, s->ss_packet, s->es_packet, s->out_ann, pkt_ann, pkt_buf);

            c_proto(di, s->ss_packet, s->es_packet, s->out_proto, "PACKET",
                    C_STR("TOKEN"), C_STR("SOF"), C_U16(framenum), C_END);
        } else {
            /* OUT, IN, SETUP, PING */
            uint32_t addr = usb_pkt_bits_to_int(s->bits, 16, 7);
            uint32_t ep = usb_pkt_bits_to_int(s->bits, 23, 4);

            char addr_buf[32];
            snprintf(addr_buf, sizeof(addr_buf), "Address: %d", addr);
            c_put(di, s->bit_ss[16], s->bit_es[22], s->out_ann, ANN_ADDR,
                  addr_buf, "Addr", "A");

            char ep_buf[32];
            snprintf(ep_buf, sizeof(ep_buf), "Endpoint: %d", ep);
            c_put(di, s->bit_ss[23], s->bit_es[26], s->out_ann, ANN_EP,
                  ep_buf, "EP", "E");

            uint32_t crc5_rx = usb_pkt_bits_to_int(s->bits, 27, 5);
            uint8_t crc5_calc = usb_pkt_calc_crc5(&s->bits[8], 19);
            if (crc5_rx == crc5_calc) {
                char crc5_buf[32];
                snprintf(crc5_buf, sizeof(crc5_buf), "CRC5: 0x%02X", crc5_rx);
                c_put(di, s->bit_ss[27], s->bit_es[31], s->out_ann, ANN_CRC5_OK, crc5_buf, "CRC5", "C");
            } else {
                char crc5_buf[64];
                snprintf(crc5_buf, sizeof(crc5_buf), "CRC5 ERROR: 0x%02X", crc5_rx);
                c_put(di, s->bit_ss[27], s->bit_es[31], s->out_ann, ANN_CRC5_ERR, crc5_buf, "CRC5 ERR", "CE", "C");
            }

            char pkt_buf[64];
            snprintf(pkt_buf, sizeof(pkt_buf), "%s: ADDR %d EP %d", pid_name, addr, ep);
            c_put(di, s->ss_packet, s->es_packet, s->out_ann, pkt_ann, pkt_buf);

            c_proto(di, s->ss_packet, s->es_packet, s->out_proto, "PACKET",
                    C_STR("TOKEN"), C_STR(pid_name), C_U8(addr), C_U8(ep), C_END);
        }

    } else if (pid_cat == PID_CAT_DATA) {
        /* Data packet: SYNC + PID + DATA(N*8) + CRC16(16) */
        int data_bits = s->bits_len - 16 - 16;
        if (data_bits < 0 || (data_bits % 8) != 0) {
            c_put(di, s->ss_packet, s->es_packet, s->out_ann, ANN_PKT_INVALID, "Invalid");
            return;
        }

        int data_bytes = data_bits / 8;

        /* CRC16 covers PID + DATA (bits 8 to bits_len-17) */
        int crc_bits = s->bits_len - 16 - 8;
        uint16_t crc16_rx = (uint16_t)usb_pkt_bits_to_int(s->bits, s->bits_len - 16, 16);
        uint16_t crc16_calc = usb_pkt_calc_crc16(&s->bits[8], crc_bits);

        /* Format data bytes */
        for (int i = 0; i < data_bytes; i++) {
            uint8_t b = usb_pkt_bits_to_byte(s->bits, 16 + i * 8, 8);
            char db1[32], db2[32], db3[32], db4[32];
            snprintf(db1, sizeof(db1), "Databyte: %02X", b);
            snprintf(db2, sizeof(db2), "Data: %02X", b);
            snprintf(db3, sizeof(db3), "DB: %02X", b);
            snprintf(db4, sizeof(db4), "%02X", b);
            c_put(di, s->bit_ss[16 + i * 8], s->bit_es[16 + i * 8 + 7], s->out_ann, ANN_DATA, db1, db2, db3, db4);
        }

        if (crc16_rx == crc16_calc) {
            char crc16_buf[32];
            snprintf(crc16_buf, sizeof(crc16_buf), "CRC16: 0x%04X", crc16_rx);
            c_put(di, s->bit_ss[s->bits_len - 16], s->bit_es[s->bits_len - 1],
                  s->out_ann, ANN_CRC16_OK, crc16_buf, "CRC16", "C");
        } else {
            char crc16_buf[64];
            snprintf(crc16_buf, sizeof(crc16_buf), "CRC16 ERROR: 0x%04X", crc16_rx);
            c_put(di, s->bit_ss[s->bits_len - 16], s->bit_es[s->bits_len - 1],
                  s->out_ann, ANN_CRC16_ERR, crc16_buf, "CRC16 ERR", "CE", "C");
        }

        /* Packet summary */
        char pkt_buf[128];
        if (data_bytes <= 8) {
            int pos = snprintf(pkt_buf, sizeof(pkt_buf), "%s [", pid_name);
            for (int i = 0; i < data_bytes; i++) {
                uint8_t b = usb_pkt_bits_to_byte(s->bits, 16 + i * 8, 8);
                pos += snprintf(pkt_buf + pos, sizeof(pkt_buf) - pos, " %02X", b);
            }
            snprintf(pkt_buf + pos, sizeof(pkt_buf) - pos, " ]");
        } else {
            snprintf(pkt_buf, sizeof(pkt_buf), "%s: %d bytes", pid_name, data_bytes);
        }
        c_put(di, s->ss_packet, s->es_packet, s->out_ann, pkt_ann, pkt_buf);

        /* Protocol output */
        uint8_t data_bytes_arr[1024];
        int db_cnt = (data_bytes < 1024) ? data_bytes : 1024;
        for (int i = 0; i < db_cnt; i++)
            data_bytes_arr[i] = usb_pkt_bits_to_byte(s->bits, 16 + i * 8, 8);
        c_proto(di, s->ss_packet, s->es_packet, s->out_proto, "PACKET",
                C_STR("DATA"), C_STR(pid_name), C_BYTES(data_bytes_arr, db_cnt), C_END);

    } else if (pid_cat == PID_CAT_HANDSHAKE) {
        /* Handshake packet: SYNC + PID only */
        char pkt_buf[32];
        snprintf(pkt_buf, sizeof(pkt_buf), "%s", pid_name);
        c_put(di, s->ss_packet, s->es_packet, s->out_ann, pkt_ann, pkt_buf);

        c_proto(di, s->ss_packet, s->es_packet, s->out_proto, "PACKET",
                C_STR("HANDSHAKE"), C_STR(pid_name), C_END);

    } else if (pid_cat == PID_CAT_SPECIAL) {
        /* Special packet */
        char pkt_buf[32];
        snprintf(pkt_buf, sizeof(pkt_buf), "%s", pid_name);
        c_put(di, s->ss_packet, s->es_packet, s->out_ann, pkt_ann, pkt_buf);

        c_proto(di, s->ss_packet, s->es_packet, s->out_proto, "PACKET",
                C_STR("SPECIAL"), C_STR(pid_name), C_END);
    }
}

/* --- Core decode_upper --- */

static void usb_pkt_decode_upper(struct srd_decoder_inst *di,
    uint64_t start_sample, uint64_t end_sample,
    const char *cmd, const c_field *fields, int n_fields)
{
    usb_pkt_s *s = (usb_pkt_s *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "SOP") == 0) {
        if (s->state != PKT_WAIT_SOP) return;
        s->ss_packet = start_sample;
        s->bits_len = 0;
        s->state = PKT_GET_BIT;
    } else if (strcmp(cmd, "BIT") == 0) {
        if (s->state != PKT_GET_BIT) return;
        uint8_t bit_val = (n_fields > 0 && fields[0].type == C_FIELD_U8) ? fields[0].u8 : 0;
        if (s->bits_len < USB_PKT_MAX_BITS) {
            s->bits[s->bits_len] = (bit_val == 1) ? '1' : '0';
            s->bit_ss[s->bits_len] = start_sample;
            s->bit_es[s->bits_len] = end_sample;
            s->bits_len++;
        }
    } else if (strcmp(cmd, "EOP") == 0 || strcmp(cmd, "ERR") == 0) {
        if (s->state != PKT_GET_BIT) return;
        s->es_packet = end_sample;
        usb_pkt_handle_packet(di, s);
        s->bits_len = 0;
        s->state = PKT_WAIT_SOP;
    }
    /* Ignore STUFF BIT, SYM, RESET, KEEP ALIVE */
}

/* --- Decoder lifecycle --- */

static void usb_pkt_start(struct srd_decoder_inst *di)
{
    usb_pkt_s *s = (usb_pkt_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "usb_packet");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "usb_packet");
    s->state = PKT_WAIT_SOP;
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder usb_packet_c_def = {
    .id = "usb_packet_c",
    .name = "USB packet(C)",
    .longname = "Universal Serial Bus (LS/FS) packet (C)",
    .desc = "USB (low-speed and full-speed) packet protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = usb_pkt_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = usb_pkt_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = usb_pkt_ann_rows,
    .inputs = usb_pkt_inputs,
    .num_inputs = 1,
    .outputs = usb_pkt_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = usb_pkt_tags,
    .num_tags = 1,
    .state_size = sizeof(usb_pkt_s),
    .reset = usb_pkt_reset,
    .start = usb_pkt_start,
    .decode = NULL,
    .end = NULL,
    .metadata = NULL,
    .destroy = usb_pkt_destroy,
    .decode_upper = usb_pkt_decode_upper,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    usb_pkt_options[0].def = g_variant_new_string("full-speed");
    GSList *sig_vals = NULL;
    sig_vals = g_slist_append(sig_vals, g_variant_new_string("full-speed"));
    sig_vals = g_slist_append(sig_vals, g_variant_new_string("low-speed"));
    usb_pkt_options[0].values = sig_vals;

    return &usb_packet_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}