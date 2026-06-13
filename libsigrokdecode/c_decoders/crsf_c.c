/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2023 James Cordell <james@cordell.org.uk>
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

enum {
    ANN_TEXT = 0,
    ANN_ERROR,
    NUM_ANN,
};

enum crsf_state {
    CRSF_WAIT_SYNC,
    CRSF_WAIT_LEN,
    CRSF_WAIT_TYPE,
    CRSF_WAIT_PAYLOAD,
};

/* CRSF sync byte definitions */
static const struct {
    uint8_t byte;
    const char *name;
    const char *short_name;
} crsf_sync_bytes[] = {
    {0xEE, "To Transmitter Module", "To TX Module"},
    {0xEA, "To Handset", "To HS"},
    {0xC8, "To Flight Controller", "To FC"},
    {0xEC, "To Receiver", "To RX"},
    {0, NULL, NULL}
};

/* CRSF frame type definitions */
static const struct {
    uint8_t type;
    const char *name;
} crsf_frame_types[] = {
    {0x02, "GPS"},
    {0x07, "Vario"},
    {0x08, "Battery sensor"},
    {0x09, "Baro altitude"},
    {0x10, "OpenTX sync"},
    {0x14, "LINK_STATISTICS"},
    {0x16, "RC_CHANNELS_PACKED"},
    {0x1E, "Attitude"},
    {0x21, "Flight mode"},
    {0x28, "DEVICE_PING"},
    {0x29, "DEVICE_INFO"},
    {0x2A, "Request settings"},
    {0x2B, "PARAMETER_SETTINGS_ENTRY"},
    {0x2C, "PARAMETER_READ"},
    {0x2D, "PARAMETER_WRITE"},
    {0x32, "COMMAND"},
    {0x3A, "Radio"},
    {0, NULL}
};

typedef struct {
    int out_ann;
    enum crsf_state state;
    uint8_t sync_byte;
    uint8_t len_byte;
    uint8_t frame_type;
    uint8_t payload[256];
    int payload_len;
    uint64_t ss_block;
    uint64_t es;
} crsf_state;

static const char *crsf_inputs[] = {"uart", NULL};
static const char *crsf_outputs[] = {"crsf", NULL};
static const char *crsf_tags[] = {"Radio/control/RC", NULL};

static const char *crsf_ann_labels[][3] = {
    {"", "text-verbose", "Human-readable text (verbose)"},
    {"", "text-error", "Human-readable Error text"},
};

static const int crsf_row_normal_classes[] = {ANN_TEXT, ANN_ERROR, -1};

static const struct srd_c_ann_row crsf_ann_rows[] = {
    {"normal", "Normal", crsf_row_normal_classes, 2},
};

    static const char *crsf_get_frame_type_name(uint8_t type)
{
    for (int i = 0; crsf_frame_types[i].name; i++) {
        if (crsf_frame_types[i].type == type)
            return crsf_frame_types[i].name;
    }
    return NULL;
}

static void crsf_handle_rc_channels(struct srd_decoder_inst *di, crsf_state *s)
{
    /* RC_CHANNELS_PACKED: 16 channels of 11 bits each = 22 bytes payload */
    if (s->payload_len < 22)
        return;

    for (int chan = 0; chan < 16; chan++) {
        int bit_offset = chan * 11;

        uint32_t raw = 0;
        /* Extract 11 bits starting at bit_offset */
        for (int b = 0; b < 11; b++) {
            int boff = bit_offset + b;
            int bbyte = boff / 8;
            int bbit = boff % 8;
            if (bbyte < s->payload_len) {
                if (s->payload[bbyte] & (1 << bbit))
                    raw |= (1 << b);
            }
        }
        char t[64];
        snprintf(t, sizeof(t), "Chan:%d Value:%u", chan, raw);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
    }
}

static void crsf_handle_link_statistics(struct srd_decoder_inst *di, crsf_state *s)
{
    /* LINK_STATISTICS: 10 bytes of link statistics */
    if (s->payload_len < 10)
        return;

    char t[128];
    snprintf(t, sizeof(t), "Uplink RSSI 1: -%ddB", s->payload[0]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Uplink RSSI 2: -%ddB", s->payload[1]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Uplink Link Quality: %d", s->payload[2]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Uplink SNR: %ddB", (int8_t)s->payload[3]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Active Antenna: %d", s->payload[4]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "RF Mode: %d", s->payload[5]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Uplink TX Power: %d mW", s->payload[6]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Downlink RSSI: -%ddB", s->payload[7]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Downlink Link Quality: %d", s->payload[8]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);

    snprintf(t, sizeof(t), "Downlink SNR: %ddB", (int8_t)s->payload[9]);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
}

static void crsf_handle_gps(struct srd_decoder_inst *di, crsf_state *s)
{
    if (s->payload_len < 15)
        return;
    int32_t lat = s->payload[0] | (s->payload[1] << 8) | (s->payload[2] << 16) | (s->payload[3] << 24);
    int32_t lon = s->payload[4] | (s->payload[5] << 8) | (s->payload[6] << 16) | (s->payload[7] << 24);
    uint16_t speed = s->payload[8] | (s->payload[9] << 8);
    uint16_t heading = s->payload[10] | (s->payload[11] << 8);
    uint16_t altitude = s->payload[12] | (s->payload[13] << 8);
    uint8_t satellites = s->payload[14];

    char t[256];
    snprintf(t, sizeof(t), "GPS: Lat=%d/1e7 Lon=%d/1e7 Speed=%u Heading=%u Alt=%u Sats=%u",
             lat, lon, speed, heading, altitude, satellites);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
}

static void crsf_handle_battery(struct srd_decoder_inst *di, crsf_state *s)
{
    if (s->payload_len < 8)
        return;
    uint16_t voltage = s->payload[0] | (s->payload[1] << 8);
    uint16_t current = s->payload[2] | (s->payload[3] << 8);
    uint32_t capacity = s->payload[4] | (s->payload[5] << 8) | (s->payload[6] << 16);
    uint8_t remaining = s->payload[7];

    char t[128];
    snprintf(t, sizeof(t), "Battery: V=%umV I=%umA Cap=%umAh Rem=%u%%",
             voltage, current, capacity, remaining);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
}

static void crsf_handle_attitude(struct srd_decoder_inst *di, crsf_state *s)
{
    if (s->payload_len < 6)
        return;
    int16_t pitch = s->payload[0] | (s->payload[1] << 8);
    int16_t roll = s->payload[2] | (s->payload[3] << 8);
    int16_t yaw = s->payload[4] | (s->payload[5] << 8);

    char t[128];
    snprintf(t, sizeof(t), "Attitude: Pitch=%d Roll=%d Yaw=%d", pitch, roll, yaw);
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
}

static void crsf_handle_flight_mode(struct srd_decoder_inst *di, crsf_state *s)
{
    char t[128];
    if (s->payload_len > 0) {
        /* Flight mode is a null-terminated string */
        int len = s->payload_len;
        if (len > (int)sizeof(t) - 16)
            len = (int)sizeof(t) - 16;
        snprintf(t, sizeof(t), "Flight mode: %.*s", len, (const char *)s->payload);
    } else {
        snprintf(t, sizeof(t), "Flight mode: (empty)");
    }
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
}

static void crsf_process_frame(struct srd_decoder_inst *di, crsf_state *s)
{
    const char *type_name = crsf_get_frame_type_name(s->frame_type);

    if (s->frame_type == 0x16) {
        crsf_handle_rc_channels(di, s);
    } else if (s->frame_type == 0x14) {
        crsf_handle_link_statistics(di, s);
    } else if (s->frame_type == 0x02) {
        crsf_handle_gps(di, s);
    } else if (s->frame_type == 0x08) {
        crsf_handle_battery(di, s);
    } else if (s->frame_type == 0x1E) {
        crsf_handle_attitude(di, s);
    } else if (s->frame_type == 0x21) {
        crsf_handle_flight_mode(di, s);
    } else if (type_name) {
        char t[128];
        snprintf(t, sizeof(t), "%s (%d bytes payload)", type_name, s->payload_len - 1);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
    } else {
        char t[64];
        snprintf(t, sizeof(t), "Unknown packet type%d", s->frame_type);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, t);
    }

    /* CRC byte */
    c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEXT, "Checksum crc8 poly 0xD5");
}

static void crsf_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    crsf_state *s = (crsf_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (n_fields < 1)
        return;

    uint8_t byte_val = fields[0].u8;
    s->es = end_sample;
    char t[128];

    switch (s->state) {
    case CRSF_WAIT_SYNC:
        /* Look for valid sync byte */
        for (int i = 0; crsf_sync_bytes[i].name; i++) {
            if (crsf_sync_bytes[i].byte == byte_val) {
                s->sync_byte = byte_val;
                s->ss_block = start_sample;
                c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT,
                          crsf_sync_bytes[i].name);
                s->state = CRSF_WAIT_LEN;
                return;
            }
        }
        /* Unknown byte, ignore */
        snprintf(t, sizeof(t), "Unknown packet type%d", byte_val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT, t);
        break;

    case CRSF_WAIT_LEN:
        if (byte_val >= 2 && byte_val <= 62) {
            s->len_byte = byte_val;
            snprintf(t, sizeof(t), "Num of bytes succeeding: %d", byte_val - 2);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT, t);
            s->state = CRSF_WAIT_TYPE;
        } else {
            snprintf(t, sizeof(t), "Unknown packet type%d", byte_val);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT, t);
            s->state = CRSF_WAIT_SYNC;
        }
        break;

    case CRSF_WAIT_TYPE:
        s->frame_type = byte_val;
        s->payload_len = 0;
        {
            const char *name = crsf_get_frame_type_name(byte_val);
            if (name) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT, name);
            } else {
                snprintf(t, sizeof(t), "Unknown packet type%d", byte_val);
                c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT, t);
                s->state = CRSF_WAIT_SYNC;
                break;
            }
        }
        /* Payload length = len_byte - 2 (type + crc) */
        if (s->len_byte <= 2) {
            /* No payload, just CRC */
            s->state = CRSF_WAIT_SYNC;
            c_put(di, s->ss_block, end_sample, s->out_ann, ANN_TEXT,
                      "Checksum crc8 poly 0xD5");
        } else {
            s->state = CRSF_WAIT_PAYLOAD;
        }
        break;

    case CRSF_WAIT_PAYLOAD:
        /* Collect payload bytes (including CRC at end) */
        if (s->payload_len < (int)sizeof(s->payload))
            s->payload[s->payload_len++] = byte_val;

        /* Total payload bytes to collect = len_byte - 1 (type already received) */
        if (s->payload_len >= s->len_byte - 1) {
            /* Frame complete, process it */
            /* payload_len includes CRC byte at end */
            crsf_process_frame(di, s);
            s->state = CRSF_WAIT_SYNC;
        }
        break;
    }
}

static void crsf_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(crsf_state)));
    crsf_state *s = (crsf_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(crsf_state));
    s->state = CRSF_WAIT_SYNC;
}

static void crsf_start(struct srd_decoder_inst *di)
{
    crsf_state *s = (crsf_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "crsf");
}

static void crsf_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void crsf_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder crsf_c_decoder = {
    .id = "crsf_c",
    .name = "CRSF(C)",
    .longname = "Crossfire RC protocol (C)",
    .desc = "A protocol for radio control systems. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = crsf_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = crsf_ann_rows,
    .inputs = crsf_inputs,
    .num_inputs = 1,
    .outputs = crsf_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = crsf_tags,
    .num_tags = 1,
    .reset = crsf_reset,
    .start = crsf_start,
    .decode = crsf_decode,
    .destroy = crsf_destroy,
    .decode_upper = crsf_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &crsf_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}