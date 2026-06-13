/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2021 original Python version
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

/* Annotations */
enum {
    ANN_HEADER = 0,
    ANN_DATA,
    NUM_ANN,
};

/* State machine */
enum eth_state {
    ETH_IDLE,
    ETH_WAIT_SFD,
    ETH_DST_MAC,
    ETH_SRC_MAC,
    ETH_ETH_TYPE,
    ETH_PAYLOAD,
};

/* Block entry for payload sample positions */
typedef struct {
    uint64_t ss;
    uint64_t es;
} eth_block_t;

/* Decoder private state — C_DECODER_STATE auto-generates ethernet_s typedef,
 * ethernet_reset (calloc), and ethernet_destroy (free). */
C_DECODER_STATE(ethernet, {
    enum eth_state state;
    int out_ann;
    int out_proto;
    int out_binary;

    int jk_seen_j;
    int jk_seen_k;
    uint64_t frame_start;

    uint8_t buffer[2048];
    int buffer_len;

    uint8_t frame[2048];
    int frame_len;

    uint64_t header_start;
    uint64_t payload_start;

    uint8_t payload[2048];
    int payload_len;

    eth_block_t blocks[2048];
    int block_count;

    uint64_t ss_block;
    uint64_t es_block;
})

/* --- EtherType lookup table --- */

typedef struct {
    uint16_t type;
    const char *long_name;
    const char *short_name;
} ethertype_entry;

static const ethertype_entry ethertype_table[] = {
    {0x0800, "Internet Protocol Version 4", "IPv4"},
    {0x0806, "Address Resolution Protocol", "ARP"},
    {0x0842, "Wake-on-LAN", "WoL"},
    {0x22F0, "Audio Video Transport Protocol", "AVTP"},
    {0x22F3, "IETF TRILL Protocol", "TRILL"},
    {0x22EA, "Stream Reservation Protocol", "SRP"},
    {0x6002, "DEC MOP", "DEC MOP RC"},
    {0x6003, "DECnet Phase IV", "DECnet"},
    {0x6004, "DEC LAT", "DEC LAT"},
    {0x8035, "Reverse Address Resolution Protocol", "RARP"},
    {0x809B, "AppleTalk (Ethertalk)", "AppleTalk"},
    {0x80F3, "AppleTalk Address Resolution Protocol", "AARP"},
    {0x8100, "VLAN-tagged Frame", "VLAN"},
    {0x8102, "Simple Loop Prevention Protocol", "SLPP"},
    {0x8103, "Virtual Link Aggregation Control Protocol", "VLACP"},
    {0x8137, "Internetwork Packet Exchange", "IPX"},
    {0x8204, "QNX Qnet", "QNX Qnet"},
    {0x86DD, "Internet Protocol Version 6", "IPv6"},
    {0x8808, "Ethernet flow control", "Flow"},
    {0x8809, "Link Aggregation Control Protocol", "LACP"},
    {0x8819, "CobraNet", "CobraNet"},
    {0x8847, "Multiprotocol Label Switching unicast", "MPLS"},
    {0x8848, "Multiprotocol Label Switching multicast", "MPLS"},
    {0x8863, "PPPoE Discovery Stage", "PPPoE"},
    {0x8864, "PPPoE Session Stage", "PPPoE"},
    {0x887B, "HomePlug 1.0 MME", "HomePlug"},
    {0x888E, "Extensible Authentication Protocol", "EAP"},
    {0x8892, "PROFINET Protocol", "PROFINET"},
    {0x889A, "HyperSCSI (SCSI over Ethernet)", "SCSI"},
    {0x88A2, "ATA over Ethernet", "ATA"},
    {0x88A4, "EtherCAT Protocol", "EtherCAT"},
    {0x88A8, "Service VLAN tag identifier", "VLAN"},
    {0x88AB, "Ethernet Powerlink", "Powerlink"},
    {0x88B8, "Generic Object Oriented Substation event", "GOOSE"},
    {0x88B9, "Generic Substation Events Management Services", "GSE"},
    {0x88BA, "Sampled Value Transmission", "SV"},
    {0x88BF, "MikroTik RoMON", "RoMON"},
    {0x88CC, "Link Layer Discovery Protocol", "LLDP"},
    {0x88CD, "SERCOS III", "SERCOS"},
    {0x88E3, "Media Redundancy Protocol", "MRP"},
    {0x88E5, "IEEE 802.1AE MAC security", "MACsec"},
    {0x88E7, "Provider Backbone Bridges", "PBB"},
    {0x88F7, "Precision Time Protocol", "PTP"},
    {0x88F8, "Network Controller Sideband Interface", "NC-SI"},
    {0x88FB, "Parallel Redundancy Protocol", "PRP"},
    {0x8902, "Connectivity Fault Management", "CFM"},
    {0x8906, "Fibre Channel over Ethernet", "FCoE"},
    {0x8914, "FCoE Initialization Protocol", "FCoE"},
    {0x8915, "Remote Direct Memory Access", "RDMA"},
    {0x891D, "TTEthernet Protocol Control Frame", "TTE"},
    {0x893A, "IEEE 1905.1 Protocol", "1905.1"},
    {0x892F, "High-availability Seamless Redundancy", "HSR"},
    {0x9000, "Ethernet Configuration Testing Protocol", "ECTP"},
    {0x9100, "VLAN-tagged Frame", "VLAN"},
    {0xF1C1, "Frame Replication and Elimination for Reliability", "FRER"},
};
#define ETHERTYPE_COUNT (sizeof(ethertype_table) / sizeof(ethertype_table[0]))

static const ethertype_entry *find_ethertype(uint16_t type)
{
    for (int i = 0; i < (int)ETHERTYPE_COUNT; i++)
        if (ethertype_table[i].type == type) return &ethertype_table[i];
    return NULL;
}

/* --- CRC32 --- */

static uint32_t crc32_table[256];
static int crc32_initialized = 0;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        crc32_table[i] = crc;
    }
    crc32_initialized = 1;
}

static uint32_t crc32_calc(const uint8_t *buf, int len)
{
    if (!crc32_initialized) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ buf[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/* --- Helper to reset frame state --- */

static void ethernet_reset_vars(ethernet_s *s)
{
    s->state = ETH_IDLE;
    s->jk_seen_j = 0;
    s->jk_seen_k = 0;
    s->frame_start = 0;
    s->buffer_len = 0;
    s->frame_len = 0;
    s->payload_len = 0;
    s->block_count = 0;
}

/* --- Decoder metadata --- */

static const char *ethernet_inputs[] = {"4b5b", NULL};
static const char *ethernet_outputs[] = {"ethernet", NULL};
static const char *ethernet_tags[] = {"Networking", "PC", NULL};

static const char *ethernet_ann_labels[][3] = {
    {"", "header", "Decoded header"},
    {"", "data", "Decoded data"},
};

static const int row_headers_classes[] = {ANN_HEADER, -1};
static const int row_datas_classes[] = {ANN_DATA, -1};

static const struct srd_c_ann_row ethernet_ann_rows[] = {
    {"headers", "Headers", row_headers_classes, 1},
    {"datas", "Datas", row_datas_classes, 1},
};

static const struct srd_decoder_binary ethernet_binary[] = {
    {0, "pcapng", "Wireshark packet capture (.pcapng)"},
};

/* --- pcapng output --- */

static void pcap_headers(struct srd_decoder_inst *di, ethernet_s *s)
{
    /* Section Header Block */
    uint8_t shb[28];
    uint32_t v;
    v = 0x0A0D0D0A; memcpy(shb + 0, &v, 4);
    v = 28;         memcpy(shb + 4, &v, 4);
    v = 0x1A2B3C4D; memcpy(shb + 8, &v, 4);
    uint16_t major = 1, minor = 0;
    memcpy(shb + 12, &major, 2);
    memcpy(shb + 14, &minor, 2);
    int64_t sl = -1;
    memcpy(shb + 16, &sl, 8);
    v = 28;         memcpy(shb + 24, &v, 4);
    c_put_bin(di, 0, 0, s->out_binary, 0, 28, shb);

    /* Interface Description Block */
    uint8_t idb[20];
    v = 1;    memcpy(idb + 0, &v, 4);
    v = 20;   memcpy(idb + 4, &v, 4);
    uint16_t link_type = 1; /* Ethernet */
    uint16_t reserved = 0;
    memcpy(idb + 8, &link_type, 2);
    memcpy(idb + 10, &reserved, 2);
    v = 1522; memcpy(idb + 12, &v, 4);
    v = 20;   memcpy(idb + 16, &v, 4);
    c_put_bin(di, 0, 0, s->out_binary, 0, 20, idb);
}

static void pcap_append(struct srd_decoder_inst *di, ethernet_s *s)
{
    int frame_len = s->frame_len;
    int pad_len = (4 - (frame_len % 4)) % 4;
    int block_len = 16 + frame_len + pad_len;

    uint8_t *pkt = g_malloc(block_len);
    uint32_t v;
    v = 3;            memcpy(pkt + 0, &v, 4);
    v = block_len;    memcpy(pkt + 4, &v, 4);
    v = frame_len;    memcpy(pkt + 8, &v, 4);
    memcpy(pkt + 12, s->frame, frame_len);
    if (pad_len > 0) memset(pkt + 12 + frame_len, 0, pad_len);
    v = block_len;    memcpy(pkt + 12 + frame_len + pad_len, &v, 4);

    c_put_bin(di, 0, 0, s->out_binary, 0, block_len, pkt);
    g_free(pkt);
}

/* --- Handle data byte --- */

static void ethernet_handle_data_byte(struct srd_decoder_inst *di, ethernet_s *s,
                                       uint8_t byte_val, uint64_t start_sample, uint64_t end_sample)
{
    if (s->state == ETH_IDLE) return;

    if (s->state == ETH_WAIT_SFD) {
        s->buffer[s->buffer_len++] = byte_val;
        if (byte_val == 0xD5) {
            /* Preamble annotation */
            s->ss_block = s->frame_start;
            s->es_block = end_sample - ((end_sample - s->frame_start) / s->buffer_len);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, "Preamble");

            /* SFD annotation */
            s->ss_block = s->es_block;
            s->es_block = end_sample;
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, "Start Frame Delimiter", "SFD");

            s->buffer_len = 0;
            s->header_start = end_sample;
            s->ss_block = end_sample;
            s->state = ETH_DST_MAC;
        }
        return;
    }

    if (s->state == ETH_DST_MAC) {
        s->buffer[s->buffer_len++] = byte_val;
        if (s->buffer_len == 6) {
            char mac_str[24];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     s->buffer[0], s->buffer[1], s->buffer[2],
                     s->buffer[3], s->buffer[4], s->buffer[5]);

            int is_broadcast = (s->buffer[0] == 0xFF && s->buffer[1] == 0xFF &&
                                s->buffer[2] == 0xFF && s->buffer[3] == 0xFF &&
                                s->buffer[4] == 0xFF && s->buffer[5] == 0xFF);

            char t[64];
            if (is_broadcast)
                snprintf(t, sizeof(t), "Destination MAC: %s (Broadcast)", mac_str);
            else
                snprintf(t, sizeof(t), "Destination MAC: %s", mac_str);
            char t2[40];
            snprintf(t2, sizeof(t2), "Dst MAC: %s", mac_str);
            s->es_block = end_sample;
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

            memcpy(s->frame + s->frame_len, s->buffer, 6);
            s->frame_len += 6;
            s->buffer_len = 0;
            s->ss_block = end_sample;
            s->state = ETH_SRC_MAC;
        }
        return;
    }

    if (s->state == ETH_SRC_MAC) {
        s->buffer[s->buffer_len++] = byte_val;
        if (s->buffer_len == 6) {
            char mac_str[24];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     s->buffer[0], s->buffer[1], s->buffer[2],
                     s->buffer[3], s->buffer[4], s->buffer[5]);

            char t[64];
            snprintf(t, sizeof(t), "Source MAC:    %s", mac_str);
            char t2[40];
            snprintf(t2, sizeof(t2), "Src MAC: %s", mac_str);
            s->es_block = end_sample;
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

            memcpy(s->frame + s->frame_len, s->buffer, 6);
            s->frame_len += 6;
            s->buffer_len = 0;
            s->ss_block = end_sample;
            s->state = ETH_ETH_TYPE;
        }
        return;
    }

    if (s->state == ETH_ETH_TYPE) {
        s->buffer[s->buffer_len++] = byte_val;
        if (s->buffer_len == 2) {
            uint16_t et = ((uint16_t)s->buffer[0] << 8) | s->buffer[1];
            const ethertype_entry *entry = find_ethertype(et);

            char t[80], t2[64];
            if (entry) {
                snprintf(t, sizeof(t), "EtherType: %s (0x%04X)", entry->long_name, et);
                snprintf(t2, sizeof(t2), "EtherType: %s (0x%04X)", entry->short_name, et);
            } else {
                snprintf(t, sizeof(t), "EtherType: UNKNOWN");
                snprintf(t2, sizeof(t2), "UNKNOWN");
            }
            s->es_block = end_sample;
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_HEADER, t, t2);

            memcpy(s->frame + s->frame_len, s->buffer, 2);
            s->frame_len += 2;
            s->buffer_len = 0;
            s->payload_start = end_sample;
            s->ss_block = end_sample;
            s->state = ETH_PAYLOAD;
        }
        return;
    }

    if (s->state == ETH_PAYLOAD) {
        s->payload[s->payload_len] = byte_val;
        s->payload_len++;

        if (s->block_count < 2048) {
            s->blocks[s->block_count].ss = start_sample;
            s->blocks[s->block_count].es = end_sample;
            s->block_count++;
        }

        char t[8];
        snprintf(t, sizeof(t), "0x%02X", byte_val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, t);
    }
}

/* --- Core decode_upper --- */

static void ethernet_decode_upper(struct srd_decoder_inst *di,
    uint64_t start_sample, uint64_t end_sample,
    const char *cmd, const c_field *fields, int n_fields)
{
    ethernet_s *s = (ethernet_s *)c_decoder_get_private(di);
    if (!s) return;

    /* J control symbol (SSD first part) */
    if (strcmp(cmd, "J") == 0) {
        s->jk_seen_j = 1;
        s->frame_start = start_sample;
        return;
    }

    /* K control symbol (SSD second part) */
    if (strcmp(cmd, "K") == 0) {
        if (s->jk_seen_j) {
            s->state = ETH_WAIT_SFD;
        } else {
            ethernet_reset_vars(s);
        }
        s->jk_seen_k = 1;
        return;
    }

    /* T control symbol (ESD - frame end) */
    if (strcmp(cmd, "T") == 0) {
        /* Add payload to frame for FCS verification */
        memcpy(s->frame + s->frame_len, s->payload, s->payload_len);
        s->frame_len += s->payload_len;

        /* Verify FCS */
        uint32_t crc = crc32_calc(s->frame, s->frame_len);
        int fcs_ok = (crc == 0x2144DF1C);
        const char *fcs_str = fcs_ok ? "OK" : "FAILED";

        /* FCS annotation */
        uint64_t fcs_start = start_sample - (uint64_t)((end_sample - start_sample) * 8);
        uint64_t fcs_end = end_sample - (end_sample - start_sample);
        char t[64], t2[32];
        snprintf(t, sizeof(t), "Frame Check Sequence: %s", fcs_str);
        snprintf(t2, sizeof(t2), "FCS: %s", fcs_str);
        c_put(di, fcs_start, fcs_end, s->out_ann, ANN_HEADER, t, t2);

        /* Add frame to pcapng */
        pcap_append(di, s);

        /* Push payload to stacked decoders */
        if (s->out_proto >= 0 && s->payload_len >= 4) {
            uint16_t plen = (uint16_t)(s->payload_len - 4); /* without FCS */
            c_proto(di, s->payload_start, fcs_start, s->out_proto, "PAYLOAD",
                    C_BYTES(s->payload, plen),
                    C_BYTES((const uint8_t *)s->blocks, s->block_count * sizeof(eth_block_t)), C_END);
        }
        ethernet_reset_vars(s);
        return;
    }

    /* R control symbol (ESD second part) */
    if (strcmp(cmd, "R") == 0) {
        ethernet_reset_vars(s);
        return;
    }

    /* Idle/pause/error control symbols — Python ignores these, only resets on R */
    /* Removed: I/S/Q/H/L reset to match Python behavior */

    /* Data byte */
    if (strcmp(cmd, "DATA") == 0) {
        uint8_t byte_val = (n_fields > 0 && fields[0].type == C_FIELD_U8) ? fields[0].u8 : 0;
        ethernet_handle_data_byte(di, s, byte_val, start_sample, end_sample);
    }
}

/* --- Decoder lifecycle --- */

static void ethernet_start(struct srd_decoder_inst *di)
{
    ethernet_s *s = (ethernet_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ethernet");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "ethernet");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "ethernet");
    pcap_headers(di, s);
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder ethernet_c_def = {
    .id = "ethernet_c",
    .name = "Ethernet(C)",
    .longname = "Ethernet II (IEEE 802.3) (C)",
    .desc = "Ethernet networking protocol (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ethernet_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = ethernet_ann_rows,
    .inputs = ethernet_inputs,
    .num_inputs = 1,
    .outputs = ethernet_outputs,
    .num_outputs = 1,
    .binary = ethernet_binary,
    .num_binary = 1,
    .tags = ethernet_tags,
    .num_tags = 2,
    .state_size = sizeof(ethernet_s),
    .reset = ethernet_reset,
    .start = ethernet_start,
    .decode = NULL,
    .end = NULL,
    .metadata = NULL,
    .destroy = ethernet_destroy,
    .decode_upper = ethernet_decode_upper,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ethernet_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}