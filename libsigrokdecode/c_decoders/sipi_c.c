/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Soeren Apel <soeren@apelpie.net>
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

/* See tc27xD_um_v2.2.pdf, Table 20-2 */
enum {
    ANN_HEADER_TAG = 0,
    ANN_HEADER_CMD,
    ANN_HEADER_CH,
    ANN_ADDRESS,
    ANN_DATA,
    ANN_CRC,
    ANN_WARNING,
    NUM_ANN,
};

typedef struct {
    uint8_t code;
    const char *name;
    int addr_len;
    int data_len;
    int n_fields;
} sipi_command_entry;

static const sipi_command_entry command_table[] = {
    {0x00, "Read byte", 4, 0, 0},
    {0x01, "Read 2 byte", 4, 0, 0},
    {0x02, "Read 4 byte", 4, 0, 0},
    {0x04, "Write byte with ACK", 4, 4, 4},
    {0x05, "Write 2 byte with ACK", 4, 4, 4},
    {0x06, "Write 4 byte with ACK", 4, 4, 4},
    {0x08, "ACK", 0, 0, 0},
    {0x09, "NACK (Target Error)", 0, 0, 0},
    {0x0A, "Read Answer with ACK", 4, 4, 4},
    {0x0C, "Trigger with ACK", 0, 0, 0},
    {0x12, "Read 4-byte JTAG ID", 0, 0, 0},
    {0x17, "Stream 32 byte with ACK", 0, 32, 32},
};
#define COMMAND_TABLE_SIZE 12

typedef struct {
    int out_ann;
    double bit_len;
    int addr_len;
    int data_len;
    int n_fields;
    int frame_len;
} sipi_state;

static const char *sipi_inputs[] = {"lfast", NULL};
static const char *sipi_tags[] = {"Embedded/industrial", NULL};

static const char *sipi_ann_labels[][3] = {
    {"", "header_tag", "Transaction Tag"},
    {"", "header_cmd", "Command Code"},
    {"", "header_ch", "Channel"},
    {"", "address", "Address"},
    {"", "data", "Data"},
    {"", "crc", "CRC"},
    {"", "warning", "Warning"},
};

static const int sipi_row_fields_classes[] = {
    ANN_HEADER_TAG, ANN_HEADER_CMD, ANN_HEADER_CH, ANN_ADDRESS, ANN_DATA, ANN_CRC, -1
};
static const int sipi_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row sipi_ann_rows[] = {
    {"fields", "Fields", sipi_row_fields_classes, 6},
    {"warnings", "Warnings", sipi_row_warnings_classes, 1},
};

static uint16_t crc_ccitt(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

static const sipi_command_entry *sipi_lookup_command(uint8_t cmd_id)
{
    for (int i = 0; i < COMMAND_TABLE_SIZE; i++) {
        if (command_table[i].code == cmd_id)
            return &command_table[i];
    }
    return NULL;
}

static void sipi_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    sipi_state *s = (sipi_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;

    /* data format from lfast_c: each entry is 17 bytes:
       [value(1B)][ss(8B LE)][es(8B LE)] */
    int num_bytes = (int)(n_fields / 17);
    if (num_bytes < 2) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING,
                  "Header too short");
        return;
    }

    /* Parse byte entries */
    uint8_t *byte_vals = (uint8_t *)g_malloc(num_bytes);
    uint64_t *byte_ss = (uint64_t *)g_malloc(num_bytes * sizeof(uint64_t));
    uint64_t *byte_es = (uint64_t *)g_malloc(num_bytes * sizeof(uint64_t));

    for (int i = 0; i < num_bytes; i++) {
        const c_field *p = fields + i * 17;
        byte_vals[i] = p[0].u8;
        memcpy(&byte_ss[i], p + 1, 8);
        memcpy(&byte_es[i], p + 9, 8);
    }

    /* Calculate bit_len */
    s->bit_len = (double)(byte_es[0] - byte_ss[0]) / 8.0;

    /* Parse Header (2 bytes) */
    uint16_t header = ((uint16_t)byte_vals[0] << 8) | byte_vals[1];
    uint64_t ss_header = byte_ss[0];
    uint64_t es_header = byte_es[1];
    (void)es_header;

    /* Tag (bits 15-13) */
    uint64_t ss = ss_header;
    uint64_t es = ss + (uint64_t)(3 * s->bit_len);
    uint8_t tag = (header & 0xE000) >> 13;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02X", tag);
    c_put(di, ss, es, s->out_ann, ANN_HEADER_TAG, buf);

    /* Command Code (bits 12-8) */
    ss = es;
    es = ss + (uint64_t)(5 * s->bit_len);
    uint8_t cmd_id = (header & 0x1F00) >> 8;
    const sipi_command_entry *entry = sipi_lookup_command(cmd_id);
    if (entry) {
        s->addr_len = entry->addr_len;
        s->data_len = entry->data_len;
        s->frame_len = 2 + 2 + s->addr_len + s->data_len;
        c_put(di, ss, es, s->out_ann, ANN_HEADER_CMD, entry->name);
    } else {
        s->addr_len = 0;
        s->data_len = 0;
        s->frame_len = 4;
        snprintf(buf, sizeof(buf), "Reserved (%02X)", cmd_id);
        c_put(di, ss, es, s->out_ann, ANN_HEADER_CMD, buf);
    }

    /* Reserved bits 4-7 */
    ss = es;
    es = ss + (uint64_t)(4 * s->bit_len);
    uint8_t reserved_bits = (header & 0x00F0) >> 4;
    if (reserved_bits > 0) {
        c_put(di, ss, es, s->out_ann, ANN_WARNING,
                  "Reserved bits #4..7 should be 0");
    }

    /* Channel (bits 3-1) */
    ss = es;
    es = ss + (uint64_t)(3 * s->bit_len);
    uint8_t ch = (header & 0x000E) >> 1;
    snprintf(buf, sizeof(buf), "%d", ch);
    c_put(di, ss, es, s->out_ann, ANN_HEADER_CH, buf);

    /* Reserved bit 0 */
    if (header & 0x0001) {
        ss = es;
        es = ss + (uint64_t)(s->bit_len);
        c_put(di, ss, es, s->out_ann, ANN_WARNING,
                  "Reserved bit #0 should be 0");
    }

    /* Parse Payload */
    int byte_idx = 2;
    int payload_len = s->frame_len - 2 - 2;

    if (payload_len > 0 && num_bytes >= s->frame_len) {
        /* Address bytes */
        if (s->addr_len > 0) {
            for (int i = 0; i < s->addr_len && byte_idx < num_bytes; i++, byte_idx++) {
                snprintf(buf, sizeof(buf), "%02X", byte_vals[byte_idx]);
                c_put(di, byte_ss[byte_idx], byte_es[byte_idx],
                          s->out_ann, ANN_ADDRESS, buf);
            }
        }

        /* Data bytes */
        if (s->data_len > 0) {
            for (int i = 0; i < s->data_len && byte_idx < num_bytes; i++, byte_idx++) {
                snprintf(buf, sizeof(buf), "%02X", byte_vals[byte_idx]);
                c_put(di, byte_ss[byte_idx], byte_es[byte_idx],
                          s->out_ann, ANN_DATA, buf);
            }
        }
    }

    /* Parse CRC (2 bytes at end) */
    if (num_bytes >= s->frame_len && byte_idx + 1 < num_bytes) {
        uint64_t crc_ss = byte_ss[byte_idx];
        uint64_t crc_es = byte_es[byte_idx + 1];
        uint16_t crc_value = ((uint16_t)byte_vals[byte_idx] << 8) | byte_vals[byte_idx + 1];

        /* CRC is calculated over header + payload bytes */
        int crc_data_len = s->frame_len - 2;
        if (crc_data_len > 0 && crc_data_len <= num_bytes) {
            uint8_t *crc_payload = (uint8_t *)g_malloc(crc_data_len);
            for (int i = 0; i < crc_data_len; i++)
                crc_payload[i] = byte_vals[i];
            uint16_t calculated_crc = crc_ccitt(crc_payload, crc_data_len);
            g_free(crc_payload);

            if (calculated_crc == crc_value) {
                c_put(di, crc_ss, crc_es, s->out_ann, ANN_CRC, "CRC OK");
            } else {
                snprintf(buf, sizeof(buf),
                         "Have %02X but calculated %02X", crc_value, calculated_crc);
                c_put(di, crc_ss, crc_es, s->out_ann, ANN_CRC, buf);
                c_put(di, crc_ss, crc_es, s->out_ann, ANN_WARNING, "CRC mismatch");
            }
        }
    } else if (num_bytes > byte_idx) {
        c_put(di, byte_ss[byte_idx], byte_es[num_bytes - 1],
                  s->out_ann, ANN_WARNING, "CRC incomplete or missing");
    }

    g_free(byte_vals);
    g_free(byte_ss);
    g_free(byte_es);
}

static void sipi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(sipi_state)));
    }
    sipi_state *s = (sipi_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(sipi_state));
}

static void sipi_start(struct srd_decoder_inst *di)
{
    sipi_state *s = (sipi_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sipi");
}

static void sipi_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void sipi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sipi_c_decoder = {
    .id = "sipi_c",
    .name = "SIPI(C)",
    .longname = "NXP SIPI (C)",
    .desc = "Serial Inter-Processor Interface (SIPI) aka Zipwire, aka HSSL. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = sipi_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = sipi_ann_rows,
    .inputs = sipi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sipi_tags,
    .num_tags = 1,
    .reset = sipi_reset,
    .start = sipi_start,
    .decode = sipi_decode,
    .destroy = sipi_destroy,
    .decode_upper = sipi_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &sipi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}