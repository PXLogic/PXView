/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Bert Vermeulen <bert@biot.com>
 * Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#define I2CFILTER_MAX_PACKETS 1024

enum {
    DIR_BOTH = 0,
    DIR_READ = 1,
    DIR_WRITE = 2,
};

typedef struct {
    int out_python;
    int curslave;
    int curdirection;       /* 0=none, 1=read, 2=write */
    int filter_address;
    int filter_direction;   /* 0=both, 1=read, 2=write */

    struct {
        uint64_t ss;
        uint64_t es;
        char cmd[32];
        uint8_t data[8];
        uint64_t data_len;
    } packets[I2CFILTER_MAX_PACKETS];
    int num_packets;
} i2cfilter_state;

static const char *i2cfilter_inputs[] = {"i2c", NULL};
static const char *i2cfilter_outputs[] = {"i2c", NULL};
static const char *i2cfilter_tags[] = {"Util", NULL};

static struct srd_decoder_option i2cfilter_options[] = {
    {"address", "dec_i2cfilter_opt_address", "Address to filter out of the I2C stream", NULL, NULL},
    {"direction", "dec_i2cfilter_opt_direction", "Direction to filter", NULL, NULL},
};

static void i2cfilter_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    i2cfilter_state *s = (i2cfilter_state *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Cache the packet */
    if (s->num_packets < I2CFILTER_MAX_PACKETS) {
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
        s->curslave = (fields && n_fields > 0) ? fields[0].u8 : 0;
        s->curdirection = (strcmp(cmd, "ADDRESS READ") == 0) ? DIR_READ : DIR_WRITE;
    } else if (strcmp(cmd, "STOP") == 0 || strcmp(cmd, "START REPEAT") == 0) {
        /* Address filter: if address != 0, only pass matching address */
        if (s->filter_address != 0 && s->curslave != s->filter_address) {
            s->num_packets = 0;
            return;
        }
        /* Direction filter */
        if (s->filter_direction != DIR_BOTH && s->curdirection != s->filter_direction) {
            s->num_packets = 0;
            return;
        }
        /* Forward all cached packets */
        for (int i = 0; i < s->num_packets; i++) {
            c_proto(di, s->packets[i].ss, s->packets[i].es,
                s->out_python, s->packets[i].cmd,
                C_BYTES(s->packets[i].data, s->packets[i].data_len), C_END);
        }
        s->num_packets = 0;
    }
}

static void i2cfilter_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(i2cfilter_state)));
    }
    i2cfilter_state *s = (i2cfilter_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(i2cfilter_state));
    s->curslave = -1;
    s->filter_direction = DIR_BOTH;
}

static void i2cfilter_start(struct srd_decoder_inst *di)
{
    i2cfilter_state *s = (i2cfilter_state *)c_decoder_get_private(di);
    s->out_python = c_reg_out(di, SRD_OUTPUT_PYTHON, "i2c");

    s->filter_address = (int)c_opt_int(di, "address", 0);

    const char *direction = c_opt_str(di, "direction", "both");
    if (direction && strcmp(direction, "read") == 0)
        s->filter_direction = DIR_READ;
    else if (direction && strcmp(direction, "write") == 0)
        s->filter_direction = DIR_WRITE;
    else
        s->filter_direction = DIR_BOTH;
}

static void i2cfilter_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void i2cfilter_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder i2cfilter_c_decoder = {
    .id = "i2cfilter_c",
    .name = "I2C filter(C)",
    .longname = "I2C filter (C)",
    .desc = "Filter out addresses/directions in an I2C stream. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = i2cfilter_options,
    .num_options = 2,
    .num_annotations = 0,
    .ann_labels = NULL,
    .num_annotation_rows = 0,
    .annotation_rows = NULL,
    .inputs = i2cfilter_inputs,
    .num_inputs = 1,
    .outputs = i2cfilter_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = i2cfilter_tags,
    .num_tags = 1,
    .reset = i2cfilter_reset,
    .start = i2cfilter_start,
    .decode = i2cfilter_decode,
    .destroy = i2cfilter_destroy,
    .decode_upper = i2cfilter_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    i2cfilter_options[0].def = g_variant_new_int64(0);

    i2cfilter_options[1].def = g_variant_new_string("both");
    GSList *dir_vals = NULL;
    dir_vals = g_slist_append(dir_vals, g_variant_new_string("read"));
    dir_vals = g_slist_append(dir_vals, g_variant_new_string("write"));
    dir_vals = g_slist_append(dir_vals, g_variant_new_string("both"));
    i2cfilter_options[1].values = dir_vals;

    return &i2cfilter_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}