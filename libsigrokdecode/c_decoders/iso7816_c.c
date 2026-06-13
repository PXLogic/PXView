/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Sven Soltermann <sven@handyman.ch>
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

/* Channel indices — match Python channels tuple */
#define CH_CLK  0
#define CH_DATA 1

enum iso7816_ann {
    ANN_WARN = 0,
    ANN_BYTE,
    ANN_ATR,
    ANN_PPS,
    ANN_T0,
    ANN_T1,
    ANN_T1_IBLOCK,
    ANN_T1_RBLOCK,
    ANN_T1_SBLOCK,
    ANN_APDU,
    NUM_ANN,
};

enum iso7816_state {
    STATE_FIND_START,
    STATE_DATA,
};

/* Clock rate and baud rate tables — match Python clock_rate / baud_rate dicts */
static const int clock_rate_table[] = {
    372, 372, 558, 744, 1116, 1488, 1860, 0, 0, 512, 768, 1024, 1536, 2048
};
static const int clock_rate_count = 14;

static const int baud_rate_table[] = {
    1, 1, 2, 4, 8, 16, 32, 64, 12, 20
};
static const int baud_rate_count = 10;

static int get_clock_rate(int idx)
{
    if (idx < 0 || idx >= clock_rate_count)
        return 372;
    if (clock_rate_table[idx] == 0)
        return 372;
    return clock_rate_table[idx];
}

static int get_baud_rate(int idx)
{
    if (idx < 0 || idx >= baud_rate_count)
        return 1;
    return baud_rate_table[idx];
}

/* Decoder state struct — C_DECODER_STATE auto-generates iso7816_s typedef,
 * iso7816_reset (calloc), and iso7816_destroy (free). */
C_DECODER_STATE(iso7816, {
    enum iso7816_state state;
    int sample_as_clock;
    int detect_clock;
    int has_t0;
    int has_t1;
    int has_t15;
    int clock_skip;
    int detected_clock_skip;
    int fi;
    int di;
    uint64_t peeked_byte;
    uint64_t peeked_samplenum;
    int has_peeked;
    uint64_t atr_bytes[64];
    int atr_count;
    int wrote_pcap_header;
    uint8_t t1_chain_buf[4096];
    int t1_chain_len;
    uint64_t t1_chain_ss;
    int t1_i_seq;
    uint64_t samplerate;

    /* Output IDs */
    int out_ann;
    int out_python;
    int out_binary;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel iso7816_channels[] = {
    {"clk", "CLK", "clock", 0, SRD_CHANNEL_SCLK, "dec_iso7816_chan_clk"},
    {"data", "data", "data", 1, SRD_CHANNEL_SDATA, "dec_iso7816_chan_data"},
};

/* Options — match Python options tuple */
static struct srd_decoder_option iso7816_options_arr[3];

/* Annotation labels — match Python annotations tuple */
static const char *iso7816_ann_labels[][3] = {
    {"", "warning", "Human-readable warnings"},
    {"", "byte", "Byte"},
    {"", "atr", "ATR (Answer to Reset)"},
    {"", "pps", "PPS (Protocol and parameters selection)"},
    {"", "t0", "T=0 packet"},
    {"", "t1", "T=1 packet"},
    {"", "t1-iblock", "T=1 I-Block"},
    {"", "t1-rblock", "T=1 R-Block"},
    {"", "t1-sblock", "T=1 S-Block"},
    {"", "apdu", "APDU"},
};

static const int row_warnings_classes[] = {ANN_WARN, -1};
static const int row_bytes_classes[] = {ANN_BYTE, -1};
static const int row_type_classes[] = {ANN_ATR, ANN_PPS, ANN_T0, ANN_T1, -1};
static const int row_t1s_classes[] = {ANN_T1_IBLOCK, ANN_T1_RBLOCK, ANN_T1_SBLOCK, -1};
static const int row_apdus_classes[] = {ANN_APDU, -1};

static const struct srd_c_ann_row iso7816_ann_rows[] = {
    {"warnings", "Warnings", row_warnings_classes, 1},
    {"bytes", "Bytes", row_bytes_classes, 1},
    {"type", "Type", row_type_classes, 4},
    {"t1s", "T=1 Decode", row_t1s_classes, 3},
    {"apdus", "apdus", row_apdus_classes, 1},
};

static const char *iso7816_inputs[] = {"logic", NULL};
static const char *iso7816_outputs[] = {"iso7816", NULL};
static const char *iso7816_tags[] = {"Embedded/industrial", NULL};

static const struct srd_decoder_binary iso7816_binary[] = {
    {0, "pcap", "PCAP format"},
};

/* ---- Helper: write PCAP global header — match Python pcap_global_header() ---- */
static void write_pcap_header(struct srd_decoder_inst *di, iso7816_s *s)
{
    if (s->wrote_pcap_header)
        return;
    unsigned char hdr[24];
    hdr[0] = 0xd4; hdr[1] = 0xc3; hdr[2] = 0xb2; hdr[3] = 0xa1;
    hdr[4] = 0x02; hdr[5] = 0x00;
    hdr[6] = 0x04; hdr[7] = 0x00;
    hdr[8] = 0x00; hdr[9] = 0x00; hdr[10] = 0x00; hdr[11] = 0x00;
    hdr[12] = 0x00; hdr[13] = 0x00; hdr[14] = 0x00; hdr[15] = 0x00;
    hdr[16] = 0xff; hdr[17] = 0xff; hdr[18] = 0x00; hdr[19] = 0x00;
    hdr[20] = 0x01; hdr[21] = 0x00; hdr[22] = 0x00; hdr[23] = 0x00;
    c_put_bin(di, 0, 0, s->out_binary, 0, 24, hdr);
    s->wrote_pcap_header = 1;
}

/* ---- Helper: write PCAP packet — match Python pcap_udp_pkt ---- */
static void write_pcap_packet(struct srd_decoder_inst *di, iso7816_s *s,
    uint64_t ss, uint64_t es, const uint8_t *data, int n_fields)
{
    write_pcap_header(di, s);

    uint64_t samplerate = c_samplerate(di);
    uint32_t ts_sec = 0, ts_usec = 0;
    if (samplerate > 0) {
        double ts = (double)ss / (double)samplerate;
        ts_sec = (uint32_t)ts;
        ts_usec = (uint32_t)((ts - (double)ts_sec) * 1e6);
    }

    int gsmtap_len = 16;
    int udp_payload_len = gsmtap_len + n_fields;
    int udp_len = 8 + udp_payload_len;
    int ip_total_len = 20 + udp_len;
    int pkt_len = 14 + ip_total_len;

    unsigned char *pkt = (unsigned char *)g_malloc(pkt_len);
    memset(pkt, 0, pkt_len);
    int pos = 0;

    /* Ethernet II header (14 bytes) */
    pos += 6; pos += 6;
    pkt[12] = 0x08; pkt[13] = 0x00;
    pos = 14;

    /* IPv4 header (20 bytes) */
    pkt[pos + 0] = 0x45; pkt[pos + 1] = 0x00;
    pkt[pos + 2] = (ip_total_len >> 8) & 0xFF;
    pkt[pos + 3] = ip_total_len & 0xFF;
    pkt[pos + 4] = 0x2B; pkt[pos + 5] = 0x0D;
    pkt[pos + 6] = 0x40; pkt[pos + 7] = 0x00;
    pkt[pos + 8] = 0x40; pkt[pos + 9] = 0x11;
    pkt[pos + 10] = 0x00; pkt[pos + 11] = 0x00;
    pkt[pos + 12] = 0x7F; pkt[pos + 13] = 0x00; pkt[pos + 14] = 0x00; pkt[pos + 15] = 0x01;
    pkt[pos + 16] = 0x7F; pkt[pos + 17] = 0x00; pkt[pos + 18] = 0x00; pkt[pos + 19] = 0x01;
    pos += 20;

    /* UDP header (8 bytes) */
    pkt[pos + 0] = 0xCC; pkt[pos + 1] = 0x46;
    pkt[pos + 2] = 0x12; pkt[pos + 3] = 0x79;
    pkt[pos + 4] = (udp_len >> 8) & 0xFF;
    pkt[pos + 5] = udp_len & 0xFF;
    pkt[pos + 6] = 0x00; pkt[pos + 7] = 0x00;
    pos += 8;

    /* GSMTAP header (16 bytes) */
    pkt[pos + 0] = 0x02; pkt[pos + 1] = 0x04; pkt[pos + 2] = 0x04;
    pos += 16;

    if (n_fields > 0 && data != NULL)
        for (int j = 0; j < n_fields; j++) pkt[pos++] = data[j];

    /* PCAP record header */
    unsigned char rec_hdr[16];
    rec_hdr[0] = (ts_sec >> 0) & 0xFF; rec_hdr[1] = (ts_sec >> 8) & 0xFF;
    rec_hdr[2] = (ts_sec >> 16) & 0xFF; rec_hdr[3] = (ts_sec >> 24) & 0xFF;
    rec_hdr[4] = (ts_usec >> 0) & 0xFF; rec_hdr[5] = (ts_usec >> 8) & 0xFF;
    rec_hdr[6] = (ts_usec >> 16) & 0xFF; rec_hdr[7] = (ts_usec >> 24) & 0xFF;
    uint32_t cap_len = (uint32_t)pkt_len;
    rec_hdr[8] = (cap_len >> 0) & 0xFF; rec_hdr[9] = (cap_len >> 8) & 0xFF;
    rec_hdr[10] = (cap_len >> 16) & 0xFF; rec_hdr[11] = (cap_len >> 24) & 0xFF;
    rec_hdr[12] = (cap_len >> 0) & 0xFF; rec_hdr[13] = (cap_len >> 8) & 0xFF;
    rec_hdr[14] = (cap_len >> 16) & 0xFF; rec_hdr[15] = (cap_len >> 24) & 0xFF;

    c_put_bin(di, ss, es, s->out_binary, 0, 16, rec_hdr);
    c_put_bin(di, ss, es, s->out_binary, 0, (uint64_t)pkt_len, pkt);
    g_free(pkt);
}

/* ---- Helper: wait for CLK rising edge ---- */
static int wait_clk_rise(struct srd_decoder_inst *di)
{
    return c_wait(di, CW_R(CH_CLK), CW_END);
}

/* ---- Helper: sleep_cycles — match Python sleep_cycles() ---- */
static int sleep_cycles(struct srd_decoder_inst *di, iso7816_s *s)
{
    int count = s->clock_skip / 3;
    if (count < 1) count = 1;
    if (s->sample_as_clock) {
        return c_wait(di, CW_SKIP((uint64_t)count), CW_END);
    } else {
        for (int i = 0; i < count; i++) {
            int ret = wait_clk_rise(di);
            if (ret != SRD_OK) return ret;
        }
        return SRD_OK;
    }
}

/* ---- Helper: wait_clock_edge_for_bit ---- */
static int wait_clock_edge_for_bit(struct srd_decoder_inst *di, iso7816_s *s)
{
    if (s->sample_as_clock) {
        return c_wait(di, CW_SKIP((uint64_t)(s->clock_skip - 4)), CW_END);
    } else {
        for (int i = 0; i < s->clock_skip; i++) {
            int ret = wait_clk_rise(di);
            if (ret != SRD_OK) return ret;
        }
        return SRD_OK;
    }
}

/* ---- Helper: read_byte_no_wait — match Python read_byte_no_wait() ---- */
static int read_byte_no_wait(struct srd_decoder_inst *di, iso7816_s *s,
    uint8_t *out_byte, uint64_t *out_ss, uint64_t *out_es)
{
    int bits[10];
    int ret;

    for (int i = 0; i < 10; i++) {
        ret = c_wait(di, CW_SKIP(0), CW_END);
        if (ret != SRD_OK) return ret;
        bits[i] = c_pin(di, CH_DATA);
        if (i < 9) {
            ret = wait_clock_edge_for_bit(di, s);
            if (ret != SRD_OK) return ret;
        }
    }

    uint64_t ss = di_samplenum(di) - (s->sample_as_clock ? (uint64_t)(s->clock_skip - 4) * 9 : 0);
    /* Use a simpler approach: ss is the first sample we waited at */
    uint64_t es = di_samplenum(di);

    int ones = 0;
    for (int i = 0; i < 10; i++)
        if (bits[i]) ones++;

    if (ones % 2 != 0) {
        char warn_str[128];
        snprintf(warn_str, sizeof(warn_str),
            "CHKSUM ERROR bits=[%d, %d, %d, %d, %d, %d, %d, %d, %d, %d]",
            bits[0], bits[1], bits[2], bits[3], bits[4],
            bits[5], bits[6], bits[7], bits[8], bits[9]);
        c_put(di, ss, es, s->out_ann, ANN_WARN, warn_str);
    }

    uint8_t byte_val = 0;
    for (int i = 0; i < 8; i++)
        byte_val |= (bits[i + 1] << i);

    char hex_str[8];
    snprintf(hex_str, sizeof(hex_str), "0x%02x", byte_val);
    c_put(di, ss, es, s->out_ann, ANN_BYTE, hex_str);

    *out_byte = byte_val;
    if (out_ss) *out_ss = ss;
    if (out_es) *out_es = es;
    return SRD_OK;
}

/* ---- Helper: read_first_byte — match Python read_first_byte() ---- */
static int read_first_byte(struct srd_decoder_inst *di, iso7816_s *s,
    uint8_t *out_byte, uint64_t *out_ss, uint64_t *out_es)
{
    uint64_t ss = di_samplenum(di);
    s->clock_skip = 0;
    int bits[10];
    int ret;

    if (s->sample_as_clock) {
        ret = c_wait(di, CW_R(CH_DATA), CW_END);
        if (ret != SRD_OK) return ret;
        s->clock_skip = (int)(di_samplenum(di) - ss) + 2;
        s->detected_clock_skip = s->clock_skip;
        ret = c_wait(di, CW_SKIP((uint64_t)(s->clock_skip / 3)), CW_END);
        if (ret != SRD_OK) return ret;
        bits[0] = 0;
    } else if (s->detect_clock) {
        while (1) {
            ret = wait_clk_rise(di);
            if (ret != SRD_OK) return ret;
            s->clock_skip++;
            int data_val = c_pin(di, CH_DATA);
            if (data_val == 1) {
                bits[0] = 0;
                int half_skip = s->clock_skip / 2;
                for (int c = 0; c < half_skip; c++) {
                    ret = wait_clk_rise(di);
                    if (ret != SRD_OK) return ret;
                }
                break;
            }
        }
        s->detected_clock_skip = s->clock_skip;
    } else {
        s->clock_skip = 372;
        bits[0] = 0;
        int total = s->clock_skip / 2 + s->clock_skip;
        for (int c = 0; c < total; c++) {
            ret = wait_clk_rise(di);
            if (ret != SRD_OK) return ret;
        }
    }

    for (int i = 0; i < 9; i++) {
        ret = c_wait(di, CW_SKIP(0), CW_END);
        if (ret != SRD_OK) return ret;
        bits[i + 1] = c_pin(di, CH_DATA);
        if (s->sample_as_clock) {
            ret = c_wait(di, CW_SKIP((uint64_t)(s->clock_skip - 4)), CW_END);
            if (ret != SRD_OK) return ret;
        } else {
            for (int c = 0; c < s->clock_skip; c++) {
                ret = wait_clk_rise(di);
                if (ret != SRD_OK) return ret;
            }
        }
    }

    uint64_t es = di_samplenum(di);

    int ones = 0;
    for (int i = 0; i < 10; i++)
        if (bits[i]) ones++;

    if (ones % 2 != 0) {
        char warn_str[128];
        snprintf(warn_str, sizeof(warn_str),
            "CHKSUM ERROR bits=[%d, %d, %d, %d, %d, %d, %d, %d, %d, %d]",
            bits[0], bits[1], bits[2], bits[3], bits[4],
            bits[5], bits[6], bits[7], bits[8], bits[9]);
        c_put(di, ss, es, s->out_ann, ANN_WARN, warn_str);
    }

    uint8_t byte_val = 0;
    for (int i = 0; i < 8; i++)
        byte_val |= (bits[i + 1] << i);

    char hex_str[8];
    snprintf(hex_str, sizeof(hex_str), "0x%02x", byte_val);
    c_put(di, ss, es, s->out_ann, ANN_BYTE, hex_str);

    *out_byte = byte_val;
    if (out_ss) *out_ss = ss;
    if (out_es) *out_es = es;
    return SRD_OK;
}

/* ---- Helper: read_byte — match Python read_byte() ---- */
static int read_byte(struct srd_decoder_inst *di, iso7816_s *s,
    uint8_t *out_byte, uint64_t *out_ss, uint64_t *out_es)
{
    if (s->has_peeked) {
        *out_byte = (uint8_t)s->peeked_byte;
        if (out_ss) *out_ss = s->peeked_samplenum;
        if (out_es) *out_es = di_samplenum(di);
        s->has_peeked = 0;
        return SRD_OK;
    }

    int ret = c_wait(di, CW_F(CH_DATA), CW_END);
    if (ret != SRD_OK) return ret;
    ret = sleep_cycles(di, s);
    if (ret != SRD_OK) return ret;
    return read_byte_no_wait(di, s, out_byte, out_ss, out_es);
}

/* ---- Helper: peek_byte — match Python peek_byte() ---- */
static int peek_byte(struct srd_decoder_inst *di, iso7816_s *s,
    uint8_t *out_byte, uint64_t *out_ss)
{
    int ret = c_wait(di, CW_F(CH_DATA), CW_END);
    if (ret != SRD_OK) return ret;
    ret = sleep_cycles(di, s);
    if (ret != SRD_OK) return ret;
    s->peeked_samplenum = di_samplenum(di);
    uint64_t es_unused;
    ret = read_byte_no_wait(di, s, out_byte, &s->peeked_samplenum, &es_unused);
    if (ret != SRD_OK) return ret;
    s->peeked_byte = *out_byte;
    s->has_peeked = 1;
    if (out_ss) *out_ss = s->peeked_samplenum;
    return SRD_OK;
}

/* ---- Helper: handle_atr — match Python handle_atr() ---- */
static int handle_atr(struct srd_decoder_inst *di, iso7816_s *s, int is_first)
{
    uint64_t atr_start = di_samplenum(di);
    uint8_t byte_val;
    uint64_t byte_ss, byte_es;
    int ret;

    s->atr_count = 0;

    if (s->has_peeked) {
        atr_start = s->peeked_samplenum;
        ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
    } else if (is_first) {
        ret = read_first_byte(di, s, &byte_val, &byte_ss, &byte_es);
    } else {
        ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
    }
    if (ret != SRD_OK) return ret;
    s->atr_bytes[s->atr_count++] = byte_val;

    uint8_t t0;
    ret = read_byte(di, s, &t0, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;
    s->atr_bytes[s->atr_count++] = t0;

    uint8_t first_t0 = t0;
    s->has_t0 = 0; s->has_t1 = 0; s->has_t15 = 0;

    while (first_t0 & 0xF0) {
        if (first_t0 & 0x10) {
            ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
            if (ret != SRD_OK) return ret;
            s->atr_bytes[s->atr_count++] = byte_val;
        }
        if (first_t0 & 0x20) {
            ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
            if (ret != SRD_OK) return ret;
            s->atr_bytes[s->atr_count++] = byte_val;
        }
        if (first_t0 & 0x40) {
            ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
            if (ret != SRD_OK) return ret;
            s->atr_bytes[s->atr_count++] = byte_val;
        }
        if (first_t0 & 0x80) {
            ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
            if (ret != SRD_OK) return ret;
            s->atr_bytes[s->atr_count++] = byte_val;
            int proto = byte_val & 0x0F;
            if (proto == 0) s->has_t0 = 1;
            else if (proto == 1) s->has_t1 = 1;
            else if (proto == 15) s->has_t15 = 1;
            else {
                char warn_str[64];
                snprintf(warn_str, sizeof(warn_str), "Invalid Protocol in ATR T=%d", proto);
                c_put(di, atr_start, di_samplenum(di), s->out_ann, ANN_WARN, warn_str);
            }
            first_t0 = byte_val;
        } else {
            first_t0 = 0;
        }
    }

    int hist_count = t0 & 0x0F;
    for (int h = 0; h < hist_count; h++) {
        ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
        if (ret != SRD_OK) return ret;
        s->atr_bytes[s->atr_count++] = byte_val;
    }

    if (!s->has_t0 && !s->has_t1) s->has_t0 = 1;

    if (s->has_t1 || s->has_t15) {
        ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es);
        if (ret != SRD_OK) return ret;
        s->atr_bytes[s->atr_count++] = byte_val;
        uint8_t xor_val = 0;
        for (int i = 1; i < s->atr_count; i++)
            xor_val ^= (uint8_t)s->atr_bytes[i];
        if (xor_val != 0) {
            char warn_str[128];
            snprintf(warn_str, sizeof(warn_str),
                "Invalid TCK in ATR, got=0x%02x expected=0x%02x", byte_val, xor_val);
            c_put(di, atr_start, di_samplenum(di), s->out_ann, ANN_WARN, warn_str);
        }
    }

    char atr_hex[256];
    int pos = 0;
    for (int i = 0; i < s->atr_count && pos < (int)sizeof(atr_hex) - 4; i++)
        pos += snprintf(atr_hex + pos, sizeof(atr_hex) - pos, "%02x", (uint8_t)s->atr_bytes[i]);

    c_put(di, atr_start, di_samplenum(di), s->out_ann, ANN_ATR, "ATR", atr_hex);

    /* Protocol output */
    unsigned char atr_bytes_out[64];
    int out_len = s->atr_count > 64 ? 64 : s->atr_count;
    for (int i = 0; i < out_len; i++)
        atr_bytes_out[i] = (unsigned char)s->atr_bytes[i];
    c_proto(di, atr_start, di_samplenum(di), s->out_python,
            "ATR", C_BYTES(atr_bytes_out, out_len), C_END);

    /* PCAP binary output */
    write_pcap_packet(di, s, atr_start, di_samplenum(di), atr_bytes_out, out_len);

    s->state = STATE_DATA;

    const char *protocol = c_opt_str(di, "protocol", "auto");
    if (protocol && strcmp(protocol, "T=0") == 0) { s->has_t0 = 1; s->has_t1 = 0; }
    else if (protocol && strcmp(protocol, "T=1") == 0) { s->has_t0 = 0; s->has_t1 = 1; }

    return SRD_OK;
}

/* ---- Helper: handle_pps — match Python handle_pps() ---- */
static int handle_pps(struct srd_decoder_inst *di, iso7816_s *s)
{
    uint64_t pps_start = s->peeked_samplenum;
    uint8_t pps, pps0, pps1 = 0, pps2 = 0, pps3 = 0, pck;
    uint64_t byte_ss, byte_es;
    int ret;
    uint8_t lrc = 0;

    ret = read_byte(di, s, &pps, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;
    ret = read_byte(di, s, &pps0, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;

    if (pps0 & 0x10) { ret = read_byte(di, s, &pps1, &byte_ss, &byte_es); if (ret != SRD_OK) return ret; lrc ^= pps1; }
    if (pps0 & 0x20) { ret = read_byte(di, s, &pps2, &byte_ss, &byte_es); if (ret != SRD_OK) return ret; lrc ^= pps2; }
    if (pps0 & 0x40) { ret = read_byte(di, s, &pps3, &byte_ss, &byte_es); if (ret != SRD_OK) return ret; lrc ^= pps3; }
    ret = read_byte(di, s, &pck, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;

    lrc ^= pps ^ pps0 ^ pck;
    if (lrc != 0) {
        char warn_str[128];
        snprintf(warn_str, sizeof(warn_str),
            "INVALID Checksum on PPS Request, got=0x%02x expected=0x%02x",
            pck, (uint8_t)(lrc ^ pps ^ pps0));
        c_put(di, pps_start, di_samplenum(di), s->out_ann, ANN_WARN, warn_str);
    }

    uint8_t r_lrc = 0;
    uint8_t r_pps, r_pps0, r_pps1 = 0, r_pps2 = 0, r_pps3 = 0, r_pck;

    ret = read_byte(di, s, &r_pps, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;
    if (r_pps != 0xFF)
        c_put(di, pps_start, di_samplenum(di), s->out_ann, ANN_WARN, "PPS Request not confirmed");
    ret = read_byte(di, s, &r_pps0, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;

    if (r_pps0 & 0x10) { ret = read_byte(di, s, &r_pps1, &byte_ss, &byte_es); if (ret != SRD_OK) return ret; r_lrc ^= r_pps1; }
    if (r_pps0 & 0x20) { ret = read_byte(di, s, &r_pps2, &byte_ss, &byte_es); if (ret != SRD_OK) return ret; r_lrc ^= r_pps2; }
    if (r_pps0 & 0x40) { ret = read_byte(di, s, &r_pps3, &byte_ss, &byte_es); if (ret != SRD_OK) return ret; r_lrc ^= r_pps3; }
    ret = read_byte(di, s, &r_pck, &byte_ss, &byte_es);
    if (ret != SRD_OK) return ret;

    r_lrc ^= r_pps ^ r_pps0 ^ r_pck;
    if (r_lrc != 0) {
        char warn_str[128];
        snprintf(warn_str, sizeof(warn_str),
            "INVALID Checksum on PPS Response, got=0x%02x expected=0x%02x",
            r_pck, (uint8_t)(r_lrc ^ r_pps ^ r_pps0));
        c_put(di, pps_start, di_samplenum(di), s->out_ann, ANN_WARN, warn_str);
    }

    if (pps0 == r_pps0 && pps1 == r_pps1 && pps2 == r_pps2 && pps3 == r_pps3) {
        if (s->detect_clock || s->sample_as_clock) {
            int tmp_fi = get_clock_rate(pps1 >> 4);
            int tmp_di = get_baud_rate(pps1 & 0x0F);
            int tmp_clock_skip = tmp_fi / tmp_di;
            s->clock_skip = (int)((uint64_t)tmp_clock_skip * s->detected_clock_skip / 372);
            s->fi = tmp_fi;
            s->di = tmp_di;
        } else {
            s->fi = get_clock_rate(pps1 >> 4);
            s->di = get_baud_rate(pps1 & 0x0F);
            s->clock_skip = s->fi / s->di;
        }
    } else {
        c_put(di, pps_start, di_samplenum(di), s->out_ann, ANN_WARN,
              "INVALID PPS. Request & Response not matching");
    }

    char pps_str[128];
    snprintf(pps_str, sizeof(pps_str), "PPS DI=%d FI=%d clock_skip=%d", s->di, s->fi, s->clock_skip);
    c_put(di, pps_start, di_samplenum(di), s->out_ann, ANN_PPS, "PPS", pps_str);

    return SRD_OK;
}

/* ---- Helper: handle_t0_packet — match Python T=0 handling in decode() ---- */
static int handle_t0_packet(struct srd_decoder_inst *di, iso7816_s *s, uint64_t pkt_start)
{
    uint8_t bClass, bIns, p1, p2, p3, procByte;
    uint8_t byte_val;
    uint64_t byte_ss, byte_es;
    int ret;
    uint8_t packet[4096];
    int pkt_len = 0;

    ret = read_byte(di, s, &bClass, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    packet[pkt_len++] = bClass;
    ret = read_byte(di, s, &bIns, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    packet[pkt_len++] = bIns;
    ret = read_byte(di, s, &p1, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    packet[pkt_len++] = p1;
    ret = read_byte(di, s, &p2, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    packet[pkt_len++] = p2;
    ret = read_byte(di, s, &p3, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    packet[pkt_len++] = p3;
    ret = read_byte(di, s, &procByte, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;

    uint8_t sw1 = 0, sw2 = 0;
    if (procByte == bIns) {
        for (int d = 0; d < p3; d++) {
            ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
            if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = byte_val;
        }
        ret = read_byte(di, s, &sw1, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
        if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = sw1;
        ret = read_byte(di, s, &sw2, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
        if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = sw2;
    } else if (procByte == 0x60) {
        sw1 = procByte;
        if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = procByte;
        ret = read_byte(di, s, &sw2, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
        if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = sw2;
    } else if ((procByte & 0xF0) == 0x60 || (procByte & 0xF0) == 0x90) {
        sw1 = procByte;
        if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = procByte;
        ret = read_byte(di, s, &sw2, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
        if (pkt_len < (int)sizeof(packet)) packet[pkt_len++] = sw2;
    } else {
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_WARN, "INVALID Procedure Byte");
    }

    c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T0, "T=0");

    char apdu_short[64];
    snprintf(apdu_short, sizeof(apdu_short), "APDU cls=0x%02x ins=0x%02x", bClass, bIns);
    char apdu_long[256];
    snprintf(apdu_long, sizeof(apdu_long),
        "APDU cls=0x%02x ins=0x%02x p1=0x%02x p2=0x%02x p3=0x%02x len=%d status=0x%02x%02x",
        bClass, bIns, p1, p2, p3, (int)p3, sw1, sw2);
    c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_APDU, "APDU", apdu_short, apdu_long);

    write_pcap_packet(di, s, pkt_start, di_samplenum(di), packet, pkt_len);
    return SRD_OK;
}

/* ---- Helper: handle_t1_block — match Python t1_parse_block() ---- */
static int handle_t1_block(struct srd_decoder_inst *di, iso7816_s *s,
    uint64_t pkt_start, int *out_is_iblock, uint8_t *out_packet, int *out_pkt_len)
{
    uint8_t nad, pcb, bLen, bLrc;
    uint8_t byte_val;
    uint64_t byte_ss, byte_es;
    int ret;
    uint8_t lrc = 0;
    uint8_t block_data[4096];
    int block_data_len = 0;

    *out_is_iblock = 0;
    *out_pkt_len = 0;

    ret = read_byte(di, s, &nad, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    lrc ^= nad;
    ret = read_byte(di, s, &pcb, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    lrc ^= pcb;

    int is_iblock = 0, is_rblock = 0, is_sblock = 0;
    int chaining = 0, i_seq = 0;
    if ((pcb & 0xC0) == 0xC0) is_sblock = 1;
    else if ((pcb & 0x80) == 0x80) is_rblock = 1;
    else { is_iblock = 1; chaining = (pcb & 0x20) >> 5; i_seq = (pcb & 0x40) >> 6; }

    ret = read_byte(di, s, &bLen, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    lrc ^= bLen;

    for (int b = 0; b < bLen; b++) {
        ret = read_byte(di, s, &byte_val, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
        lrc ^= byte_val;
        if (block_data_len < (int)sizeof(block_data))
            block_data[block_data_len++] = byte_val;
    }

    ret = read_byte(di, s, &bLrc, &byte_ss, &byte_es); if (ret != SRD_OK) return ret;
    lrc ^= bLrc;

    if (lrc != 0) {
        char warn_str[128];
        snprintf(warn_str, sizeof(warn_str),
            "Invalid checksum on T=1 block, got=0x%02x expected=0x%02x", lrc, bLrc);
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_WARN, warn_str);
    }

    if (is_iblock) {
        *out_is_iblock = 1;
        char iblock_str[128];
        snprintf(iblock_str, sizeof(iblock_str),
            "I-Block seq=%d len=%d chaining=%d", i_seq, (int)bLen, chaining);
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T1_IBLOCK, "I-Block", iblock_str);

        int copy_len = block_data_len < 4096 ? block_data_len : 4096;
        memcpy(out_packet, block_data, copy_len);
        *out_pkt_len = copy_len;

        if (chaining) {
            int append_len = block_data_len;
            if (s->t1_chain_len + append_len > (int)sizeof(s->t1_chain_buf))
                append_len = (int)sizeof(s->t1_chain_buf) - s->t1_chain_len;
            if (append_len > 0) {
                memcpy(s->t1_chain_buf + s->t1_chain_len, block_data, append_len);
                s->t1_chain_len += append_len;
            }
            s->t1_i_seq = i_seq;
            if (s->t1_chain_len == append_len)
                s->t1_chain_ss = pkt_start;
        } else {
            if (s->t1_chain_len > 0) {
                int append_len = block_data_len;
                if (s->t1_chain_len + append_len > (int)sizeof(s->t1_chain_buf))
                    append_len = (int)sizeof(s->t1_chain_buf) - s->t1_chain_len;
                if (append_len > 0) {
                    memcpy(s->t1_chain_buf + s->t1_chain_len, block_data, append_len);
                    s->t1_chain_len += append_len;
                }
                c_put(di, s->t1_chain_ss, di_samplenum(di), s->out_ann, ANN_T1, "T=1", "T=1 (reassembled)");
                if (s->t1_chain_len >= 8) {
                    char apdu_short[64], apdu_long[256];
                    snprintf(apdu_short, sizeof(apdu_short), "APDU cls=0x%02x ins=0x%02x", s->t1_chain_buf[0], s->t1_chain_buf[1]);
                    snprintf(apdu_long, sizeof(apdu_long),
                        "APDU cls=0x%02x ins=0x%02x p1=0x%02x p2=0x%02x p3=0x%02x len=%d status=0x%02x%02x",
                        s->t1_chain_buf[0], s->t1_chain_buf[1], s->t1_chain_buf[2], s->t1_chain_buf[3],
                        s->t1_chain_buf[4], s->t1_chain_len - 7,
                        s->t1_chain_buf[s->t1_chain_len - 2], s->t1_chain_buf[s->t1_chain_len - 1]);
                    c_put(di, s->t1_chain_ss, di_samplenum(di), s->out_ann, ANN_APDU, "APDU", apdu_short, apdu_long);
                }
                write_pcap_packet(di, s, s->t1_chain_ss, di_samplenum(di), s->t1_chain_buf, s->t1_chain_len);
                s->t1_chain_len = 0;
            } else {
                c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T1, "T=1", "T=1 (reassembled)");
                if (block_data_len >= 8) {
                    char apdu_short[64], apdu_long[256];
                    snprintf(apdu_short, sizeof(apdu_short), "APDU cls=0x%02x ins=0x%02x", block_data[0], block_data[1]);
                    snprintf(apdu_long, sizeof(apdu_long),
                        "APDU cls=0x%02x ins=0x%02x p1=0x%02x p2=0x%02x p3=0x%02x len=%d status=0x%02x%02x",
                        block_data[0], block_data[1], block_data[2], block_data[3],
                        block_data[4], block_data_len - 7,
                        block_data[block_data_len - 2], block_data[block_data_len - 1]);
                    c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_APDU, "APDU", apdu_short, apdu_long);
                }
                write_pcap_packet(di, s, pkt_start, di_samplenum(di), block_data, block_data_len);
            }
        }
    }
    if (is_rblock) {
        char rblock_str[64];
        snprintf(rblock_str, sizeof(rblock_str), "R-Block seq=%d flag=0x%02x", (pcb & 0x10) >> 4, pcb & 0x0F);
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T1_RBLOCK, "R-Block", rblock_str);
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T1, "T=1");
        uint8_t rblock_data[3] = {nad, pcb, bLen};
        write_pcap_packet(di, s, pkt_start, di_samplenum(di), rblock_data, 3);
    }
    if (is_sblock) {
        char sblock_str[64];
        snprintf(sblock_str, sizeof(sblock_str), "S-Block flag=0x%02x len=%d", pcb & 0x3F, (int)bLen);
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T1_SBLOCK, "S-Block", sblock_str);
        c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_T1, "T=1");
        int sblock_total = 3 + bLen;
        uint8_t sblock_data[259];
        sblock_data[0] = nad; sblock_data[1] = pcb; sblock_data[2] = bLen;
        for (int b = 0; b < bLen && b < 256; b++)
            sblock_data[3 + b] = block_data[b];
        write_pcap_packet(di, s, pkt_start, di_samplenum(di), sblock_data, sblock_total > 259 ? 259 : sblock_total);
    }

    return SRD_OK;
}

/* ---- start callback — match Python start() ---- */
static void iso7816_start(struct srd_decoder_inst *di)
{
    iso7816_s *s = (iso7816_s *)c_decoder_get_private(di);

    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "iso7816");
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "iso7816");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "iso7816");

    const char *clock_opt = c_opt_str(di, "clock_option", "native");
    s->sample_as_clock = (clock_opt && strcmp(clock_opt, "sample_as_clock") == 0);
    s->detect_clock = (clock_opt && strcmp(clock_opt, "detect") == 0);

    const char *starts_atr = c_opt_str(di, "starts_with_atr", "true");
    if (starts_atr && strcmp(starts_atr, "false") == 0)
        s->state = STATE_DATA;
    else
        s->state = STATE_FIND_START;

    const char *protocol = c_opt_str(di, "protocol", "auto");
    if (protocol && strcmp(protocol, "T=0") == 0) { s->has_t0 = 1; s->has_t1 = 0; }
    else if (protocol && strcmp(protocol, "T=1") == 0) { s->has_t0 = 0; s->has_t1 = 1; }

    /* Non-zero defaults that calloc didn't set */
    s->clock_skip = 372;
    s->fi = 372;
    s->di = 1;
    s->has_t0 = 1;
    s->samplerate = c_samplerate(di);
}

/* ---- decode callback — match Python decode() ---- */
static void iso7816_decode(struct srd_decoder_inst *di)
{
    iso7816_s *s = (iso7816_s *)c_decoder_get_private(di);
    int ret;

    /* Write PCAP global header at start */
    write_pcap_header(di, s);

    while (1) {
        if (s->state == STATE_FIND_START) {
            /* Wait for data high — match Python: self.wait({1: 'h'}) */
            ret = c_wait(di, CW_H(CH_DATA), CW_END);
            if (ret != SRD_OK) return;

            /* Wait for data fall — match Python: self.wait({1: 'f'}) */
            ret = c_wait(di, CW_F(CH_DATA), CW_END);
            if (ret != SRD_OK) return;

            ret = handle_atr(di, s, 1);
            if (ret != SRD_OK) return;

        } else if (s->state == STATE_DATA) {
            uint8_t first_byte;
            uint64_t peek_ss;

            ret = peek_byte(di, s, &first_byte, &peek_ss);
            if (ret != SRD_OK) return;

            if (first_byte == 0xFF) {
                ret = handle_pps(di, s);
                if (ret != SRD_OK) return;
                continue;
            }

            if (first_byte == 0x3B) {
                ret = c_wait(di, CW_SKIP(0), CW_END);
                if (ret != SRD_OK) return;
                s->has_peeked = 0;
                ret = handle_atr(di, s, 0);
                if (ret != SRD_OK) return;
                continue;
            }

            uint64_t pkt_start = s->peeked_samplenum;

            if (s->has_t0) {
                ret = handle_t0_packet(di, s, pkt_start);
                if (ret != SRD_OK) return;
            } else if (s->has_t1) {
                int is_iblock = 0;
                uint8_t t1_packet[4096];
                int t1_pkt_len = 0;

                ret = handle_t1_block(di, s, pkt_start, &is_iblock, t1_packet, &t1_pkt_len);
                if (ret != SRD_OK) return;

                if (is_iblock && t1_pkt_len > 0) {
                    int is_iblock2 = 0;
                    uint8_t t1_packet2[4096];
                    int t1_pkt_len2 = 0;

                    ret = peek_byte(di, s, &first_byte, &peek_ss);
                    if (ret != SRD_OK) return;

                    uint64_t resp_start = s->peeked_samplenum;
                    ret = handle_t1_block(di, s, resp_start, &is_iblock2, t1_packet2, &t1_pkt_len2);
                    if (ret != SRD_OK) return;

                    if (is_iblock2 && t1_pkt_len2 > 0) {
                        int combined_len = t1_pkt_len + t1_pkt_len2;
                        uint8_t combined[8192];
                        if (combined_len > (int)sizeof(combined))
                            combined_len = (int)sizeof(combined);
                        memcpy(combined, t1_packet, t1_pkt_len);
                        memcpy(combined + t1_pkt_len, t1_packet2, combined_len - t1_pkt_len);

                        if (combined_len >= 8) {
                            char apdu_short[64], apdu_long[256];
                            snprintf(apdu_short, sizeof(apdu_short), "APDU cls=0x%02x ins=0x%02x", combined[0], combined[1]);
                            snprintf(apdu_long, sizeof(apdu_long),
                                "APDU cls=0x%02x ins=0x%02x p1=0x%02x p2=0x%02x p3=0x%02x len=%d status=0x%02x%02x",
                                combined[0], combined[1], combined[2], combined[3],
                                combined[4], combined_len - 7,
                                combined[combined_len - 2], combined[combined_len - 1]);
                            c_put(di, pkt_start, di_samplenum(di), s->out_ann, ANN_APDU, "APDU", apdu_short, apdu_long);
                        }
                        write_pcap_packet(di, s, pkt_start, di_samplenum(di), combined, combined_len);
                    }
                }
            }
        } else {
            break;
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder iso7816_c_def = {
    .id = "iso7816_c",
    .name = "ISO 7816(C)",
    .longname = "Smartcard (C)",
    .desc = "ISO 7816 decoder (smartcard, C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = iso7816_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = iso7816_options_arr,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = iso7816_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = iso7816_ann_rows,
    .inputs = iso7816_inputs,
    .num_inputs = 1,
    .outputs = iso7816_outputs,
    .num_outputs = 1,
    .binary = iso7816_binary,
    .num_binary = 1,
    .tags = iso7816_tags,
    .num_tags = 1,
    .state_size = sizeof(iso7816_s),
    .reset = iso7816_reset,
    .start = iso7816_start,
    .decode = iso7816_decode,
    .end = NULL,
    .metadata = NULL,
    .destroy = iso7816_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *clock_option_vals[] = {
        g_variant_new_string("native"),
        g_variant_new_string("detect"),
        g_variant_new_string("sample_as_clock"),
    };
    GSList *clock_option_list = NULL;
    clock_option_list = g_slist_append(clock_option_list, clock_option_vals[0]);
    clock_option_list = g_slist_append(clock_option_list, clock_option_vals[1]);
    clock_option_list = g_slist_append(clock_option_list, clock_option_vals[2]);
    iso7816_options_arr[0].id = "clock_option";
    iso7816_options_arr[0].idn = "dec_iso7816_opt_clock_option";
    iso7816_options_arr[0].desc = "Clock option";
    iso7816_options_arr[0].def = g_variant_new_string("native");
    iso7816_options_arr[0].values = clock_option_list;

    GVariant *protocol_vals[] = {
        g_variant_new_string("auto"),
        g_variant_new_string("T=0"),
        g_variant_new_string("T=1"),
    };
    GSList *protocol_list = NULL;
    protocol_list = g_slist_append(protocol_list, protocol_vals[0]);
    protocol_list = g_slist_append(protocol_list, protocol_vals[1]);
    protocol_list = g_slist_append(protocol_list, protocol_vals[2]);
    iso7816_options_arr[1].id = "protocol";
    iso7816_options_arr[1].idn = "dec_iso7816_opt_protocol";
    iso7816_options_arr[1].desc = "Protocol";
    iso7816_options_arr[1].def = g_variant_new_string("auto");
    iso7816_options_arr[1].values = protocol_list;

    GVariant *starts_with_atr_vals[] = {
        g_variant_new_string("true"),
        g_variant_new_string("false"),
    };
    GSList *starts_with_atr_list = NULL;
    starts_with_atr_list = g_slist_append(starts_with_atr_list, starts_with_atr_vals[0]);
    starts_with_atr_list = g_slist_append(starts_with_atr_list, starts_with_atr_vals[1]);
    iso7816_options_arr[2].id = "starts_with_atr";
    iso7816_options_arr[2].idn = "dec_iso7816_opt_starts_with_atr";
    iso7816_options_arr[2].desc = "Starts with ATR";
    iso7816_options_arr[2].def = g_variant_new_string("true");
    iso7816_options_arr[2].values = starts_with_atr_list;

    return &iso7816_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}