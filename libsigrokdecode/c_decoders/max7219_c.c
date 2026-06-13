/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Paul Evans <leonerd@leonerd.org.uk>
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
    ANN_REG = 0,
    ANN_DIGIT,
    ANN_WARNING,
    NUM_ANN,
};

typedef struct {
    int out_ann;
    int pos;
    int cs_asserted;
    uint8_t addr;
    uint64_t addr_start;
    uint64_t cs_start;
} max7219_state;

static const char *max7219_inputs[] = {"spi", NULL};
static const char *max7219_tags[] = {"Display", NULL};

static const char *max7219_ann_labels[][3] = {
    {"", "register", "Registers written to the device"},
    {"", "digit", "Digits displayed on the device"},
    {"", "warnings", "Human-readable warnings"},
};

static const int max7219_row_commands_classes[] = {ANN_REG, ANN_DIGIT, -1};
static const int max7219_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row max7219_ann_rows[] = {
    {"commands", "Commands", max7219_row_commands_classes, 2},
    {"warnings", "Warnings", max7219_row_warnings_classes, 1},
};

/* Custom binary formatter (no %b in C standard library) */
static void fmt_binary(uint8_t val, char *buf, int bufsize)
{
    if (bufsize < 11) { buf[0] = '\0'; return; }
    buf[0] = '0'; buf[1] = 'b';
    for (int i = 7; i >= 0; i--)
        buf[2 + (7 - i)] = (val & (1 << i)) ? '1' : '0';
    buf[10] = '\0';
}

static void max7219_decode_intensity(uint8_t val, char *buf, int bufsize)
{
    int intensity = val & 0x0F;
    if (intensity == 0)
        snprintf(buf, bufsize, "min");
    else if (intensity == 15)
        snprintf(buf, bufsize, "max");
    else
        snprintf(buf, bufsize, "%d", intensity);
}

static void max7219_handle_register(struct srd_decoder_inst *di, max7219_state *s,
    uint8_t addr, uint8_t val, uint64_t ss, uint64_t es)
{
    if (addr >= 1 && addr <= 8) {
        /* Digit display */
        char buf[64];
        snprintf(buf, sizeof(buf), "Digit %d: 0x%02X", addr, val);
        c_put_v(di, ss, es, s->out_ann, ANN_DIGIT, val, buf);
        return;
    }

    /* Control registers */
    char buf[512];
    switch (addr) {
    case 0x00:
        snprintf(buf, sizeof(buf), "No-op");
        break;
    case 0x09: {
        char bin[11];
        fmt_binary(val, bin, sizeof(bin));
        snprintf(buf, sizeof(buf), "Decode: %s", bin);
        break;
    }
    case 0x0A: {
        char intensity[16];
        max7219_decode_intensity(val, intensity, sizeof(intensity));
        snprintf(buf, sizeof(buf), "Intensity: %s", intensity);
        break;
    }
    case 0x0B:
        snprintf(buf, sizeof(buf), "Scan limit: %d", 1 + val);
        break;
    case 0x0C:
        snprintf(buf, sizeof(buf), "Shutdown: %s", val ? "off" : "on");
        break;
    case 0x0F:
        snprintf(buf, sizeof(buf), "Display test: %s", val ? "on" : "off");
        break;
    default:
        snprintf(buf, sizeof(buf), "Unknown register 0x%02X", addr);
        c_put(di, ss, es, s->out_ann, ANN_WARNING, buf);
        return;
    }

    c_put(di, ss, es, s->out_ann, ANN_REG, buf);
}

static void max7219_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    max7219_state *s = (max7219_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "DATA") == 0) {
        if (!s->cs_asserted) return;
        if (n_fields < 17) return;

        int have_mosi = (fields[0].u8 & 1) ? 1 : 0;
        uint8_t mosi_byte = have_mosi ? fields[1].u8 : 0;

        if (s->pos == 0) {
            s->addr = mosi_byte;
            s->addr_start = start_sample;
        } else if (s->pos == 1) {
            max7219_handle_register(di, s, s->addr, mosi_byte,
                                     s->addr_start, end_sample);
        }
        s->pos++;
    } else if (strcmp(cmd, "CS-CHANGE") == 0) {
        int new_cs = (fields && n_fields >= 2) ? fields[1].u8 : 0;
        s->cs_asserted = (new_cs == 0);
        if (s->cs_asserted) {
            s->pos = 0;
            s->cs_start = start_sample;
        } else {
            if (s->pos == 1) {
                c_put(di, s->cs_start, end_sample, s->out_ann, ANN_WARNING, "Short write");
            } else if (s->pos > 2) {
                c_put(di, s->cs_start, end_sample, s->out_ann, ANN_WARNING, "Overlong write");
            }
        }
    }
}

static void max7219_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(max7219_state)));
    }
    max7219_state *s = (max7219_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(max7219_state));
}

static void max7219_start(struct srd_decoder_inst *di)
{
    max7219_state *s = (max7219_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "max7219");
    s->pos = 0;
    s->cs_start = 0;
}

static void max7219_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void max7219_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder max7219_c_decoder = {
    .id = "max7219_c",
    .name = "MAX7219(C)",
    .longname = "Maxim MAX7219/MAX7221 (C)",
    .desc = "Maxim MAX72xx series 8-digit LED display driver. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = max7219_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = max7219_ann_rows,
    .inputs = max7219_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = max7219_tags,
    .num_tags = 1,
    .reset = max7219_reset,
    .start = max7219_start,
    .decode = max7219_decode,
    .destroy = max7219_destroy,
    .decode_upper = max7219_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &max7219_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}