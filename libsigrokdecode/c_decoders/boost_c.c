/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012-2013 Uwe Hermann <uwe@hermann-uwe.de>
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
    ANN_MESSAGE = 0,
    ANN_ERROR,
    ANN_BYTES,
    NUM_ANN,
};

/* LEGO Boost color names */
static const char *lego_colors[] = {
    "Black", "Pink", "Purple", "Blue", "LightBlue",
    "Cyan", "Green", "Yellow", "Orange", "Red", "White"
};

/* LEGO Boost Color/Distance sensor modes */
static const char *lego_cd_sensor_modes[] = {
    "ColorOnly", "CoarseDist", NULL, NULL, "FineDist",
    NULL, "RGB", NULL, "Color+Dist", "Luminosity"
};

typedef struct {
    int out_ann;
    uint8_t message[2][256];
    int msg_len[2];
    uint64_t ss_block[2];
    uint64_t es_block[2];
    int show_errors;
    int show_bytes;
} boost_state;

static const char *boost_inputs[] = {"uart", NULL};
static const char *boost_outputs[] = {"boost", NULL};
static const char *boost_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option boost_options[] = {
    {"show_errors", "dec_boost_opt_show_errors", "Show errors?", NULL, NULL},
    {"show_bytes", "dec_boost_opt_show_bytes", "Show message bytes?", NULL, NULL},
};

static const char *boost_ann_labels[][3] = {
    {"", "message", "Valid messages that pass checksum"},
    {"", "error", "Invalid/malformed messages"},
    {"", "byte", "Each individual byte"},
};

static const int boost_row_messages_classes[] = {ANN_MESSAGE, -1};
static const int boost_row_errors_classes[] = {ANN_ERROR, -1};
static const int boost_row_bytes_classes[] = {ANN_BYTES, -1};

static const struct srd_c_ann_row boost_ann_rows[] = {
    {"messages", "Messages", boost_row_messages_classes, 1},
    {"errors", "Errors", boost_row_errors_classes, 1},
    {"bytes", "Bytes", boost_row_bytes_classes, 1},
};

static const char *lego_color(int code)
{
    if (code >= 0 && code <= 10)
        return lego_colors[code];
    return "None";
}

static const char *lego_cd_sensor_mode(int mode)
{
    if (mode >= 0 && mode <= 9 && lego_cd_sensor_modes[mode])
        return lego_cd_sensor_modes[mode];
    static char buf[32];
    snprintf(buf, sizeof(buf), "Unk %02X", mode);
    return buf;
}

static int valid_checksum(const uint8_t *msg, int len)
{
    uint8_t b = 0xFF;
    for (int i = 0; i < len; i++)
        b ^= msg[i];
    return b == 0;
}

static void boost_putx(struct srd_decoder_inst *di, boost_state *s,
                        int rxtx, int cls, const char *text,
                        uint64_t ss, uint64_t es)
{
    (void)rxtx;
    if (cls == ANN_ERROR && !s->show_errors)
        return;
    if (cls == ANN_BYTES && !s->show_bytes)
        return;
    c_put(di, ss, es, s->out_ann, cls, text);
}

static int boost_handle_message(struct srd_decoder_inst *di, boost_state *s, int rxtx)
{
    uint8_t *msg = s->message[rxtx];
    int len = s->msg_len[rxtx];
    char t[256];

    /* Message type 0x46: C/D Sensor mode indicator */
    if (msg[0] == 0x46) {
        if (len < 3)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        const char *mode = lego_cd_sensor_mode(msg[1]);
        snprintf(t, sizeof(t), "C/D Sensor: Mode=%s (%02X)", mode, msg[1]);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0xC1: CD sensor mode 01 response - distance */
    if (msg[0] == 0xC1) {
        if (len < 3)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        snprintf(t, sizeof(t), "Distance: %d inches", msg[1]);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0xC0: Color/Distance sensor - color */
    if (msg[0] == 0xC0) {
        if (len < 3)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        snprintf(t, sizeof(t), "C/D Sensor: Color=%s", lego_color(msg[1]));
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0xD0: Color/Distance sensor status */
    if (msg[0] == 0xD0) {
        if (len < 6)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        const char *color = lego_color(msg[1]);
        snprintf(t, sizeof(t), "C/D Sensor: Status=%02X CDist=%-3d FDist=%-3d Color=%s",
                 msg[3], msg[2], msg[4], color);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0xD8: Motor status */
    if (msg[0] == 0xD8) {
        if (len < 10)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        int8_t speed = (int8_t)msg[1];
        int32_t angle_dist = (int32_t)(msg[2] | (msg[3] << 8) | (msg[4] << 16) | (msg[5] << 24));
        int angle = angle_dist % 360;
        snprintf(t, sizeof(t), "Motor: Speed=%-3d Angle=%03d", speed, angle);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0x54: Motor initialization */
    if (msg[0] == 0x54) {
        if (len < 6)
            return 0;
        static const uint8_t expected[] = {0x54, 0x22, 0x00, 0x10, 0x20, 0xB9};
        if (memcmp(msg, expected, 6) != 0) {
            boost_putx(di, s, rxtx, ANN_ERROR, "Malformed Motor Initialization message",
                       s->ss_block[rxtx], s->es_block[rxtx]);
            return 1;
        }
        boost_putx(di, s, rxtx, ANN_MESSAGE, "Motor Initialization",
                   s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0x43: Sensor mode change order */
    if (msg[0] == 0x43) {
        if (len < 4)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        const char *mode = lego_cd_sensor_mode(msg[1]);
        snprintf(t, sizeof(t), "C/D Sensor Mode Change: Mode=%s (%02X)", mode, msg[1]);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0xDE: CD sensor RGB values */
    if (msg[0] == 0xDE) {
        if (len < 10)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        uint16_t red = msg[1] | (msg[2] << 8);
        uint16_t green = msg[3] | (msg[4] << 8);
        uint16_t blue = msg[5] | (msg[6] << 8);
        snprintf(t, sizeof(t), "R=%d G=%d B=%d", red, green, blue);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Message type 0xD1: Luminosity */
    if (msg[0] == 0xD1) {
        if (len < 6)
            return 0;
        if (!valid_checksum(msg, len))
            goto failed_checksum;
        uint16_t lum = msg[1] | (msg[2] << 8);
        snprintf(t, sizeof(t), "Luminosity=%d", lum);
        boost_putx(di, s, rxtx, ANN_MESSAGE, t, s->ss_block[rxtx], s->es_block[rxtx]);
        return 1;
    }

    /* Unknown/unhandled message type - consume it */
    return 1;

failed_checksum:
    snprintf(t, sizeof(t), "Failed checksum: msg type 0x%02X", msg[0]);
    boost_putx(di, s, rxtx, ANN_ERROR, t, s->ss_block[rxtx], s->es_block[rxtx]);
    return 1;
}

static void boost_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    boost_state *s = (boost_state *)c_decoder_get_private(di);
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

    /* If this is the start of a message, remember the start sample */
    if (s->msg_len[rxtx] == 0)
        s->ss_block[rxtx] = start_sample;

    /* Append byte to current message */
    if (s->msg_len[rxtx] < (int)sizeof(s->message[rxtx]))
        s->message[rxtx][s->msg_len[rxtx]++] = byte_val;

    /* Show individual byte */
    char t[8];
    snprintf(t, sizeof(t), "%02X", byte_val);
    boost_putx(di, s, rxtx, ANN_BYTES, t, start_sample, end_sample);

    /* Update end sample */
    s->es_block[rxtx] = end_sample;

    /* Try to handle the message */
    if (boost_handle_message(di, s, rxtx)) {
        s->msg_len[rxtx] = 0;
    }
}

static void boost_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(boost_state)));
    boost_state *s = (boost_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(boost_state));
    s->show_errors = 1;
    s->show_bytes = 0;
}

static void boost_start(struct srd_decoder_inst *di)
{
    boost_state *s = (boost_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "boost");

    const char *show_err = c_opt_str(di, "show_errors", "yes");
    s->show_errors = (show_err && strcmp(show_err, "yes") == 0) ? 1 : 0;

    const char *show_byt = c_opt_str(di, "show_bytes", "no");
    s->show_bytes = (show_byt && strcmp(show_byt, "yes") == 0) ? 1 : 0;
}

static void boost_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void boost_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder boost_c_decoder = {
    .id = "boost_c",
    .name = "Boost(C)",
    .longname = "LEGO Boost Hub and Peripherals (C)",
    .desc = "LEGO Boost Hub and Peripherals protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = boost_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = boost_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = boost_ann_rows,
    .inputs = boost_inputs,
    .num_inputs = 1,
    .outputs = boost_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = boost_tags,
    .num_tags = 1,
    .reset = boost_reset,
    .start = boost_start,
    .decode = boost_decode,
    .destroy = boost_destroy,
    .decode_upper = boost_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    boost_options[0].def = g_variant_new_string("yes");
    GSList *err_vals = NULL;
    err_vals = g_slist_append(err_vals, g_variant_new_string("yes"));
    err_vals = g_slist_append(err_vals, g_variant_new_string("no"));
    boost_options[0].values = err_vals;

    boost_options[1].def = g_variant_new_string("no");
    GSList *byt_vals = NULL;
    byt_vals = g_slist_append(byt_vals, g_variant_new_string("yes"));
    byt_vals = g_slist_append(byt_vals, g_variant_new_string("no"));
    boost_options[1].values = byt_vals;

    return &boost_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}