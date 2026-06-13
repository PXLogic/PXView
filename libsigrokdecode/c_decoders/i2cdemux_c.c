/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
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

#define MAX_SLAVES 128
#define MAX_PACKETS 1024

typedef struct {
    int num_slaves;
    uint8_t slaves[MAX_SLAVES];
    int out_python[MAX_SLAVES];
    int stream;
    int streamcount;

    struct {
        uint64_t ss;
        uint64_t es;
        char cmd[32];
        uint8_t data[8];
        uint64_t data_len;
    } packets[MAX_PACKETS];
    int num_packets;
} i2cdemux_state;

static const char *i2cdemux_inputs[] = {"i2c", NULL};
static const char *i2cdemux_tags[] = {"Util", NULL};

static void i2cdemux_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    i2cdemux_state *s = (i2cdemux_state *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Cache the packet */
    if (s->num_packets < MAX_PACKETS) {
        int i = s->num_packets++;
        s->packets[i].ss = start_sample;
        s->packets[i].es = end_sample;
        strncpy(s->packets[i].cmd, cmd, sizeof(s->packets[i].cmd) - 1);
        s->packets[i].cmd[sizeof(s->packets[i].cmd) - 1] = '\0';
        uint64_t copy_len = (uint64_t)n_fields < sizeof(s->packets[i].data) ? (uint64_t)n_fields : sizeof(s->packets[i].data);
        if (fields && copy_len > 0)
            for (uint64_t j = 0; j < copy_len; j++) s->packets[i].data[j] = fields[j].u8;
        s->packets[i].data_len = copy_len;
    }

    if (strcmp(cmd, "ADDRESS READ") == 0 || strcmp(cmd, "ADDRESS WRITE") == 0) {
        uint8_t addr = (fields && n_fields > 0) ? fields[0].u8 : 0;
        /* Find or create output stream for this slave */
        int found = -1;
        for (int j = 0; j < s->num_slaves; j++) {
            if (s->slaves[j] == addr) {
                found = j;
                break;
            }
        }
        if (found < 0 && s->num_slaves < MAX_SLAVES) {
            char proto_id[32];
            snprintf(proto_id, sizeof(proto_id), "i2c-0x%02x", addr);
            s->slaves[s->num_slaves] = addr;
            s->out_python[s->num_slaves] = c_reg_out(
                di, SRD_OUTPUT_PYTHON, proto_id);
            found = s->num_slaves;
            s->num_slaves++;
        }
        s->stream = found;
    } else if (strcmp(cmd, "STOP") == 0) {
        /* Send all cached packets to the target stream */
        if (s->stream >= 0 && s->stream < s->num_slaves) {
            for (int i = 0; i < s->num_packets; i++) {
                c_proto(di, s->packets[i].ss, s->packets[i].es,
                    s->out_python[s->stream],
                    s->packets[i].cmd, C_BYTES(s->packets[i].data, s->packets[i].data_len), C_END);
            }
        }
        s->num_packets = 0;
        s->stream = -1;
    }
}

static void i2cdemux_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(i2cdemux_state)));
    }
    i2cdemux_state *s = (i2cdemux_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(i2cdemux_state));
    s->stream = -1;
}

static void i2cdemux_start(struct srd_decoder_inst *di)
{
    (void)di;
}

static void i2cdemux_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void i2cdemux_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder i2cdemux_c_decoder = {
    .id = "i2cdemux_c",
    .name = "I2C demux(C)",
    .longname = "I2C demultiplexer (C)",
    .desc = "Demux I2C packets into per-slave-address streams. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = 0,
    .ann_labels = NULL,
    .num_annotation_rows = 0,
    .annotation_rows = NULL,
    .inputs = i2cdemux_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = i2cdemux_tags,
    .num_tags = 1,
    .reset = i2cdemux_reset,
    .start = i2cdemux_start,
    .decode = i2cdemux_decode,
    .destroy = i2cdemux_destroy,
    .decode_upper = i2cdemux_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &i2cdemux_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}