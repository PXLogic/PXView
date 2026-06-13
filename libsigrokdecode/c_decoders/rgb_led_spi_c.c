/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2014 Matt Ranostay <mranostay@gmail.com>
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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
    ANN_RGB = 0,
    ANN_START_FRAME,
    ANN_LED_FRAME,
    ANN_WARNING,
    NUM_ANN,
};

enum {
    LED_TYPE_SIMPLE,
    LED_TYPE_APA102,
};

typedef struct {
    int out_ann;
    int cs_asserted;
    int byte_count;
    int led_index;
    uint8_t mosi_bytes[4];
    uint64_t frame_start;
    uint64_t cs_start;
    int led_type;
    int in_start_frame;
    int start_frame_count;
} rgb_led_spi_state;

static const char *rgb_led_spi_inputs[] = {"spi", NULL};
static const char *rgb_led_spi_tags[] = {"Display", NULL};

static struct srd_decoder_option rgb_led_spi_options[] = {
    {"led_type", "dec_rgb_led_spi_opt_led_type", "LED strip type (simple/apa102)", NULL, NULL},
};

static const char *rgb_led_spi_ann_labels[][3] = {
    {"", "rgb", "RGB values"},
    {"", "start_frame", "Start frame"},
    {"", "led_frame", "LED frame"},
    {"", "warnings", "Human-readable warnings"},
};

static const int rgb_led_spi_row_rgb_classes[] = {ANN_RGB, -1};
static const int rgb_led_spi_row_frames_classes[] = {ANN_START_FRAME, ANN_LED_FRAME, -1};
static const int rgb_led_spi_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row rgb_led_spi_ann_rows[] = {
    {"rgb", "RGB values", rgb_led_spi_row_rgb_classes, 1},
    {"frames", "Frames", rgb_led_spi_row_frames_classes, 2},
    {"warnings", "Warnings", rgb_led_spi_row_warnings_classes, 1},
};

static uint64_t rgb_led_spi_read_le64(const c_field *fields)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; i++)
        val |= ((uint64_t)fields[i].u8) << (8 * i);
    return val;
}

static void rgb_led_spi_output_simple(struct srd_decoder_inst *di,
    rgb_led_spi_state *s, uint64_t start_sample, uint64_t end_sample)
{
    uint8_t red = s->mosi_bytes[0];
    uint8_t green = s->mosi_bytes[1];
    uint8_t blue = s->mosi_bytes[2];
    int rgb_value = (red << 16) | (green << 8) | blue;

    char buf[32];
    snprintf(buf, sizeof(buf), "#%.6x", rgb_value);
    c_put_v(di, start_sample, end_sample, s->out_ann, ANN_RGB,
                  rgb_value, buf);
    c_put(di, start_sample, end_sample, s->out_ann, ANN_LED_FRAME, buf);
}

static void rgb_led_spi_output_apa102(struct srd_decoder_inst *di,
    rgb_led_spi_state *s, uint64_t start_sample, uint64_t end_sample)
{
    /* APA102 LED frame: 0xE0|brightness, blue, green, red */
    uint8_t brightness = s->mosi_bytes[0] & 0x1F;
    uint8_t blue = s->mosi_bytes[1];
    uint8_t green = s->mosi_bytes[2];
    uint8_t red = s->mosi_bytes[3];
    int rgb_value = (red << 16) | (green << 8) | blue;

    char buf[64];
    snprintf(buf, sizeof(buf), "LED %d: B=%d #%.6x", s->led_index, brightness, rgb_value);
    c_put_v(di, start_sample, end_sample, s->out_ann, ANN_RGB,
                  rgb_value, buf);
    c_put(di, start_sample, end_sample, s->out_ann, ANN_LED_FRAME, buf);
    s->led_index++;
}

static void rgb_led_spi_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    rgb_led_spi_state *s = (rgb_led_spi_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "DATA") == 0) {
        if (!s->cs_asserted) return;
        if (n_fields < 17) return;

        int have_mosi = (fields[0].u8 & 1) ? 1 : 0;
        if (!have_mosi) return;

        uint64_t mosi = rgb_led_spi_read_le64(fields + 1);
        uint8_t byte_val = (uint8_t)(mosi & 0xFF);

        if (s->byte_count == 0)
            s->frame_start = start_sample;

        if (s->led_type == LED_TYPE_APA102) {
            /* APA102: start frame is 4 zero bytes, then LED frames of 4 bytes each */
            if (s->in_start_frame) {
                s->start_frame_count++;
                if (s->start_frame_count >= 4) {
                    c_put(di, s->frame_start, end_sample,
                              s->out_ann, ANN_START_FRAME, "Start frame");
                    s->in_start_frame = 0;
                    s->byte_count = 0;
                }
                return;
            }

            /* Check for start frame pattern (4 zero bytes) */
            if (s->byte_count == 0 && byte_val == 0x00) {
                s->in_start_frame = 1;
                s->start_frame_count = 1;
                s->frame_start = start_sample;
                return;
            }

            s->mosi_bytes[s->byte_count] = byte_val;
            s->byte_count++;

            if (s->byte_count >= 4) {
                rgb_led_spi_output_apa102(di, s, s->frame_start, end_sample);
                s->byte_count = 0;
            }
        } else {
            /* Simple RGB: 3 bytes per LED (R, G, B) */
            s->mosi_bytes[s->byte_count] = byte_val;
            s->byte_count++;

            if (s->byte_count >= 3) {
                rgb_led_spi_output_simple(di, s, s->frame_start, end_sample);
                s->byte_count = 0;
                s->led_index++;
            }
        }
    } else if (strcmp(cmd, "CS-CHANGE") == 0) {
        int new_cs = (fields && n_fields >= 2) ? fields[1].u8 : 0;
        s->cs_asserted = (new_cs == 0);

        if (s->cs_asserted) {
            s->byte_count = 0;
            s->led_index = 0;
            s->in_start_frame = (s->led_type == LED_TYPE_APA102) ? 1 : 0;
            s->start_frame_count = 0;
            s->cs_start = start_sample;
        } else {
            if (s->byte_count > 0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Incomplete frame (%d byte%s)",
                         s->byte_count, s->byte_count > 1 ? "s" : "");
                c_put(di, s->frame_start, end_sample,
                          s->out_ann, ANN_WARNING, buf);
            }
            s->byte_count = 0;
            s->led_index = 0;
            s->in_start_frame = (s->led_type == LED_TYPE_APA102) ? 1 : 0;
            s->start_frame_count = 0;
        }
    }
}

static void rgb_led_spi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(rgb_led_spi_state)));
    }
    rgb_led_spi_state *s = (rgb_led_spi_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(rgb_led_spi_state));
    s->led_type = LED_TYPE_SIMPLE;
}

static void rgb_led_spi_start(struct srd_decoder_inst *di)
{
    rgb_led_spi_state *s = (rgb_led_spi_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rgb_led_spi");

    const char *led_type = c_opt_str(di, "led_type", "simple");
    if (led_type && strcmp(led_type, "apa102") == 0)
        s->led_type = LED_TYPE_APA102;
    else
        s->led_type = LED_TYPE_SIMPLE;
}

static void rgb_led_spi_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void rgb_led_spi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder rgb_led_spi_c_decoder = {
    .id = "rgb_led_spi_c",
    .name = "RGB LED SPI(C)",
    .longname = "RGB LED string decoder (SPI) (C)",
    .desc = "RGB LED string protocol (RGB values clocked over SPI). (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = rgb_led_spi_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = rgb_led_spi_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = rgb_led_spi_ann_rows,
    .inputs = rgb_led_spi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rgb_led_spi_tags,
    .num_tags = 1,
    .reset = rgb_led_spi_reset,
    .start = rgb_led_spi_start,
    .decode = rgb_led_spi_decode,
    .destroy = rgb_led_spi_destroy,
    .decode_upper = rgb_led_spi_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    rgb_led_spi_options[0].def = g_variant_new_string("simple");
    GSList *type_vals = NULL;
    type_vals = g_slist_append(type_vals, g_variant_new_string("simple"));
    type_vals = g_slist_append(type_vals, g_variant_new_string("apa102"));
    rgb_led_spi_options[0].values = type_vals;

    return &rgb_led_spi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}