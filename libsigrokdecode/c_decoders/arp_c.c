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
    ANN_DATA = 0,
    ANN_MSG,
    NUM_ANN,
};

/* Decoder private state */
typedef struct {
    int out_ann;
    uint64_t ss_block;
    uint64_t es_block;
} arp_state;

/* --- EtherType lookup table (same as ethernet_c) --- */

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

/* --- Helper to read block ss/es --- */

static uint64_t block_ss(const c_field *blocks_data, int idx)
{
    return blocks_data[idx * 2].u64;
}

static uint64_t block_es(const c_field *blocks_data, int idx)
{
    return blocks_data[idx * 2 + 1].u64;
}

/* --- Decoder metadata --- */

static const char *arp_inputs[] = {"ethernet", NULL};
static const char *arp_tags[] = {"Networking", "PC", NULL};

static const char *arp_ann_labels[][3] = {
    {"", "data", "Decoded data"},
    {"", "msg", "Message"},
};

static const int row_datas_classes[] = {ANN_DATA, -1};
static const int row_msgs_classes[] = {ANN_MSG, -1};

static const struct srd_c_ann_row arp_ann_rows[] = {
    {"datas", "Datas", row_datas_classes, 1},
    {"msgs", "Messages", row_msgs_classes, 1},
};

/* --- Core recv_proto --- */

static void arp_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    arp_state *s = (arp_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "PAYLOAD") != 0 || !fields || n_fields < 4)
        return;

    /* Parse PAYLOAD data layout */
    uint16_t payload_len;
    payload_len = fields[0].u16;
    const uint8_t *payload = fields[1].bytes.data;

    uint16_t block_count;
    block_count = fields[2].u16;
    const c_field *blocks_data = fields + 4 + payload_len;

    /* ARP packet needs at least 28 bytes */
    if (payload_len < 28 || block_count < 28)
        return;

    /* Parse ARP fields: >2H2BH6s4s6s4s */
    uint16_t htype = ((uint16_t)payload[0] << 8) | payload[1];
    uint16_t ptype = ((uint16_t)payload[2] << 8) | payload[3];
    uint8_t hlen = payload[4];
    uint8_t plen = payload[5];
    uint16_t oper = ((uint16_t)payload[6] << 8) | payload[7];
    const uint8_t *sha = payload + 8;   /* 6 bytes */
    const uint8_t *spa = payload + 14;  /* 4 bytes */
    const uint8_t *tha = payload + 18;  /* 6 bytes */
    const uint8_t *tpa = payload + 24;  /* 4 bytes */

    /* Hardware Type */
    s->ss_block = block_ss(blocks_data, 0);
    s->es_block = block_es(blocks_data, 1);
    char t[256], t2[128];
    snprintf(t, sizeof(t), "Hardware Type: %d", htype);
    snprintf(t2, sizeof(t2), "Type: %d", htype);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Protocol Type (EtherType) */
    s->ss_block = block_ss(blocks_data, 2);
    s->es_block = block_es(blocks_data, 3);
    const ethertype_entry *entry = find_ethertype(ptype);
    if (entry) {
        snprintf(t, sizeof(t), "Protocol: %s (0x%04X)", entry->long_name, ptype);
        snprintf(t2, sizeof(t2), "Protocol: %s (0x%04X)", entry->short_name, ptype);
    } else {
        snprintf(t, sizeof(t), "Protocol: UNKNOWN");
        snprintf(t2, sizeof(t2), "UNKNOWN");
    }
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Hardware Address Length */
    s->ss_block = block_ss(blocks_data, 4);
    s->es_block = block_es(blocks_data, 4);
    snprintf(t, sizeof(t), "Hardware Address Length: %d", hlen);
    snprintf(t2, sizeof(t2), "HW Len: %d", hlen);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Protocol Address Length */
    s->ss_block = block_ss(blocks_data, 5);
    s->es_block = block_es(blocks_data, 5);
    snprintf(t, sizeof(t), "Protocol Address Length: %d", plen);
    snprintf(t2, sizeof(t2), "Prot addr Len: %d", plen);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Operation */
    const char *oper_str = "";
    if (oper == 1) oper_str = "Request";
    else if (oper == 2) oper_str = "Reply";
    else oper_str = "Unknown";
    s->ss_block = block_ss(blocks_data, 6);
    s->es_block = block_es(blocks_data, 7);
    snprintf(t, sizeof(t), "Operation: %s", oper_str);
    snprintf(t2, sizeof(t2), "OP: %s", oper_str);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Sender Hardware Address (SHA) MAC */
    char sha_str[24];
    snprintf(sha_str, sizeof(sha_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             sha[0], sha[1], sha[2], sha[3], sha[4], sha[5]);
    s->ss_block = block_ss(blocks_data, 8);
    s->es_block = block_es(blocks_data, 13);
    snprintf(t, sizeof(t), "Source MAC: %s", sha_str);
    snprintf(t2, sizeof(t2), "Src MAC: %s", sha_str);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);
    uint64_t msg_start = s->ss_block;

    /* Sender Protocol Address (SPA) IP */
    char spa_str[16];
    snprintf(spa_str, sizeof(spa_str), "%d.%d.%d.%d", spa[0], spa[1], spa[2], spa[3]);
    s->ss_block = block_ss(blocks_data, 14);
    s->es_block = block_es(blocks_data, 17);
    snprintf(t, sizeof(t), "Source IP: %s", spa_str);
    snprintf(t2, sizeof(t2), "Src IP: %s", spa_str);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Target Hardware Address (THA) MAC */
    char tha_str[24];
    snprintf(tha_str, sizeof(tha_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             tha[0], tha[1], tha[2], tha[3], tha[4], tha[5]);
    s->ss_block = block_ss(blocks_data, 18);
    s->es_block = block_es(blocks_data, 23);
    snprintf(t, sizeof(t), "Destination MAC: %s", tha_str);
    snprintf(t2, sizeof(t2), "Dst MAC: %s", tha_str);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);

    /* Target Protocol Address (TPA) IP */
    char tpa_str[16];
    snprintf(tpa_str, sizeof(tpa_str), "%d.%d.%d.%d", tpa[0], tpa[1], tpa[2], tpa[3]);
    s->ss_block = block_ss(blocks_data, 24);
    s->es_block = block_es(blocks_data, 27);
    snprintf(t, sizeof(t), "Destination IP: %s", tpa_str);
    snprintf(t2, sizeof(t2), "Dst IP: %s", tpa_str);
    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, t, t2);
    uint64_t msg_end = s->es_block;

    /* Message annotation */
    s->ss_block = msg_start;
    s->es_block = msg_end;

    if (oper == 1) { /* Request */
        if (strcmp(spa_str, tpa_str) == 0) {
            snprintf(t, sizeof(t), "ARP Announcement for %s (%s)", spa_str, sha_str);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_MSG, t);
        } else if (strcmp(spa_str, "0.0.0.0") == 0) {
            snprintf(t, sizeof(t), "ARP Probe for %s (%s)", tpa_str, sha_str);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_MSG, t);
        } else {
            snprintf(t, sizeof(t), "Who has %s? Tell %s (%s)", tpa_str, spa_str, sha_str);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_MSG, t);
        }
    } else if (oper == 2) { /* Reply */
        snprintf(t, sizeof(t), "%s is at %s", spa_str, sha_str);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_MSG, t);
    }
}

/* --- Decoder lifecycle --- */

static void arp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(arp_state)));
    arp_state *s = (arp_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(arp_state));
}

static void arp_start(struct srd_decoder_inst *di)
{
    arp_state *s = (arp_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "arp");
}

static void arp_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void arp_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder arp_c_decoder = {
    .id = "arp_c",
    .name = "ARP(C)",
    .longname = "Address Resolution Protocol (C)",
    .desc = "ARP (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = arp_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = arp_ann_rows,
    .inputs = arp_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = arp_tags,
    .num_tags = 2,
    .reset = arp_reset,
    .start = arp_start,
    .decode = arp_decode,
    .destroy = arp_destroy,
    .decode_upper = arp_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &arp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}