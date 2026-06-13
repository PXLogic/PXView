/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Petteri Aimonen <jpa@sigrok.mail.kapsi.fi>
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
    ANN_STREAM = 0,
    ANN_DATA,
    NUM_ANN,
};

typedef struct {
    int out_ann;
    int out_proto;
    int stream;
    uint64_t ss_stream;
    int bytenum;
    uint64_t prev_sample;
    uint64_t byte_len;
    /* Frame buffer: 16 entries, each with (ss, es, byte_val) */
    uint64_t frame_ss[16];
    uint64_t frame_es[16];
    uint8_t frame_data[16];
    int frame_len;
    /* Sync buffer */
    uint8_t syncbuf[4];
    int syncbuf_len;
    /* Options */
    int opt_stream;
    int opt_sync_offset;
} arm_tpiu_state;

static const char *arm_tpiu_inputs[] = {"uart", NULL};
static const char *arm_tpiu_outputs[] = {"arm_tpiu", NULL};
static const char *arm_tpiu_tags[] = {"Debug/trace", NULL};

static struct srd_decoder_option arm_tpiu_options[] = {
    {"stream", "dec_arm_tpiu_opt_stream", "Stream index", NULL, NULL},
    {"sync_offset", "dec_arm_tpiu_sync_offset", "Initial sync offset", NULL, NULL},
};

static const char *arm_tpiu_ann_labels[][3] = {
    {"", "stream", "Current stream"},
    {"", "data", "Stream data"},
};

static const int arm_tpiu_row_stream_classes[] = {ANN_STREAM, -1};
static const int arm_tpiu_row_data_classes[] = {ANN_DATA, -1};

static const struct srd_c_ann_row arm_tpiu_ann_rows[] = {
    {"stream", "Current stream", arm_tpiu_row_stream_classes, 1},
    {"data", "Stream data", arm_tpiu_row_data_classes, 1},
};

static void arm_tpiu_stream_changed(struct srd_decoder_inst *di, arm_tpiu_state *s,
                                     uint64_t ss, int stream)
{
    if (s->stream != stream) {
        if (s->stream != 0) {
            char t[32];
            snprintf(t, sizeof(t), "Stream %d", s->stream);
            c_put(di, s->ss_stream, ss, s->out_ann, ANN_STREAM, t);
        }
        s->stream = stream;
        s->ss_stream = ss;
    }
}

static void arm_tpiu_emit_byte(struct srd_decoder_inst *di, arm_tpiu_state *s,
                                uint64_t ss, uint64_t es, uint8_t byte_val)
{
    if (s->stream == s->opt_stream) {
        char t[16];
        snprintf(t, sizeof(t), "0x%02x", byte_val);
        c_put(di, ss, es, s->out_ann, ANN_DATA, t);
        /* Emit as protocol data for upper-layer decoders */
        
        
         /* rxtx = 0 (RX) */
        c_proto(di, ss, es, s->out_proto, "DATA", C_U8(byte_val), C_U8(0), C_END);
    }
}

static void arm_tpiu_process_frame(struct srd_decoder_inst *di, arm_tpiu_state *s)
{
    /* Byte 15 contains the lowest bits of bytes 0, 2, ... 14 */
    uint8_t lowbits = s->frame_data[15];

    for (int i = 0; i < 15; i += 2) {
        int delayed_stream_change = -1;
        uint8_t lowbit = (lowbits >> (i / 2)) & 0x01;

        /* Odd bytes can be stream ID or data */
        if (s->frame_data[i] & 0x01) {
            if (lowbit) {
                delayed_stream_change = s->frame_data[i] >> 1;
            } else {
                arm_tpiu_stream_changed(di, s, s->frame_ss[i], s->frame_data[i] >> 1);
            }
        } else {
            uint8_t byte_val = s->frame_data[i] | lowbit;
            arm_tpiu_emit_byte(di, s, s->frame_ss[i], s->frame_es[i], byte_val);
        }

        /* Even bytes are always data */
        if (i < 14) {
            arm_tpiu_emit_byte(di, s, s->frame_ss[i + 1], s->frame_es[i + 1],
                               s->frame_data[i + 1]);
        }

        /* The stream change can be delayed to occur after the data byte */
        if (delayed_stream_change >= 0) {
            arm_tpiu_stream_changed(di, s, s->frame_es[i + 1], delayed_stream_change);
        }
    }
}

static void arm_tpiu_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    arm_tpiu_state *s = (arm_tpiu_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (n_fields < 1)
        return;

    uint8_t byte_val = fields[0].u8;

    s->byte_len = end_sample - start_sample;

    /* Reset packet if there is a long pause between bytes */
    if (start_sample - s->prev_sample > s->byte_len)
        s->frame_len = 0;
    s->prev_sample = end_sample;

    /* Store in frame buffer */
    if (s->frame_len < 16) {
        s->frame_ss[s->frame_len] = start_sample;
        s->frame_es[s->frame_len] = end_sample;
        s->frame_data[s->frame_len] = byte_val;
        s->frame_len++;
    }
    s->bytenum++;

    /* Allow skipping N first bytes of the data */
    if (s->bytenum < s->opt_sync_offset) {
        s->frame_len = 0;
        return;
    }

    /* Keep separate buffer for detection of sync packets */
    if (s->syncbuf_len < 4) {
        s->syncbuf[s->syncbuf_len++] = byte_val;
    } else {
        memmove(s->syncbuf, s->syncbuf + 1, 3);
        s->syncbuf[3] = byte_val;
    }

    /* Sync pattern: 0xFF 0xFF 0xFF 0x7F */
    if (s->syncbuf_len >= 4 &&
        s->syncbuf[0] == 0xFF && s->syncbuf[1] == 0xFF &&
        s->syncbuf[2] == 0xFF && s->syncbuf[3] == 0x7F) {
        s->frame_len = 0;
        s->syncbuf_len = 0;
        return;
    }

    /* Process complete 16-byte frame */
    if (s->frame_len == 16) {
        arm_tpiu_process_frame(di, s);
        s->frame_len = 0;
    }
}

static void arm_tpiu_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(arm_tpiu_state)));
    arm_tpiu_state *s = (arm_tpiu_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(arm_tpiu_state));
    s->opt_stream = 1;
    s->opt_sync_offset = 0;
}

static void arm_tpiu_start(struct srd_decoder_inst *di)
{
    arm_tpiu_state *s = (arm_tpiu_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "arm_tpiu");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "arm_tpiu");
    s->opt_stream = (int)c_opt_int(di, "stream", 1);
    s->opt_sync_offset = (int)c_opt_int(di, "sync_offset", 0);
}

static void arm_tpiu_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void arm_tpiu_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder arm_tpiu_c_decoder = {
    .id = "arm_tpiu_c",
    .name = "ARM TPIU(C)",
    .longname = "ARM Trace Port Interface Unit (C)",
    .desc = "Filter TPIU formatted trace data into separate streams. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = arm_tpiu_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = arm_tpiu_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = arm_tpiu_ann_rows,
    .inputs = arm_tpiu_inputs,
    .num_inputs = 1,
    .outputs = arm_tpiu_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = arm_tpiu_tags,
    .num_tags = 1,
    .reset = arm_tpiu_reset,
    .start = arm_tpiu_start,
    .decode = arm_tpiu_decode,
    .destroy = arm_tpiu_destroy,
    .decode_upper = arm_tpiu_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    arm_tpiu_options[0].def = g_variant_new_int64(1);
    GSList *stream_vals = NULL;
    stream_vals = g_slist_append(stream_vals, g_variant_new_int64(0));
    stream_vals = g_slist_append(stream_vals, g_variant_new_int64(1));
    stream_vals = g_slist_append(stream_vals, g_variant_new_int64(2));
    arm_tpiu_options[0].values = stream_vals;

    arm_tpiu_options[1].def = g_variant_new_int64(0);

    return &arm_tpiu_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}