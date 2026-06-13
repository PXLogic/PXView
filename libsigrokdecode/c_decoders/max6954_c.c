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
#include <ctype.h>
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
} max6954_state;

static const char *max6954_inputs[] = {"spi", NULL};
static const char *max6954_tags[] = {"Display", NULL};

static const char *max6954_ann_labels[][3] = {
    {"", "register", "Register write"},
    {"", "digit", "Digit displayed"},
    {"", "warning", "Warning"},
};

static const int max6954_row_commands_classes[] = {ANN_REG, ANN_DIGIT, -1};
static const int max6954_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row max6954_ann_rows[] = {
    {"commands", "Commands", max6954_row_commands_classes, 2},
    {"warnings", "Warnings", max6954_row_warnings_classes, 1},
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

static void max6954_decode_intensity(uint8_t val, char *buf, int bufsize)
{
    int intensity = val & 0x0F;
    if (intensity == 0)
        snprintf(buf, bufsize, "min");
    else if (intensity == 15)
        snprintf(buf, bufsize, "max");
    else
        snprintf(buf, bufsize, "%d", intensity);
}

static void max6954_decode_intensity_pair(uint8_t val, int dig0, int dig1, char *buf, int bufsize)
{
    char b0[16], b1[16];
    max6954_decode_intensity(val & 0x0F, b0, sizeof(b0));
    max6954_decode_intensity((val >> 4) & 0x0F, b1, sizeof(b1));
    snprintf(buf, bufsize, "%d: %s, %d: %s", dig0, b0, dig1, b1);
}

static void max6954_decode_configuration(uint8_t val, char *buf, int bufsize)
{
    const char *S = (val & 0x01) ? "false" : "true";
    const char *B = (val & 0x04) ? "fast" : "slow";
    const char *E = (val & 0x08) ? "enabled" : "disabled";
    const char *T = (val & 0x10) ? "true" : "false";
    const char *R = (val & 0x10) ? "true" : "false";
    const char *I = (val & 0x10) ? "per digit" : "global";
    snprintf(buf, bufsize,
        "Shutdown: %s, Blink rate: %s, Blink: %s, Reset blink: %s, Clear data: %s, Intensity control: %s",
        S, B, E, T, R, I);
}

static void max6954_decode_digit_type(uint8_t val, char *buf, int bufsize)
{
    if (val == 0xFF) {
        snprintf(buf, bufsize, "All 14-seg");
        return;
    }
    if (val == 0x00) {
        snprintf(buf, bufsize, "All 16/7-seg");
        return;
    }
    int pos = 0;
    for (int i = 0; i < 8 && pos < bufsize - 32; i++) {
        if (i > 0) pos += snprintf(buf + pos, bufsize - pos, ", ");
        pos += snprintf(buf + pos, bufsize - pos, "Dig %d: %s",
            i, (val & (1 << i)) ? "14-seg" : "16/7-seg");
    }
}

static void max6954_decode_digit(uint8_t val, char *buf, int bufsize)
{
    if (isalpha(val))
        snprintf(buf, bufsize, "'%c'", val);
    else
        snprintf(buf, bufsize, "0x%02X", val);
}

static void max6954_handle_register(struct srd_decoder_inst *di, max6954_state *s,
    uint8_t addr, uint8_t val, uint64_t ss, uint64_t es)
{
    char buf[512];
    const char *name = NULL;
    

    /* Digit registers: 0x20-0x2F (P0), 0x40-0x4F (P1), 0x60-0x6F (Both) */
    if ((addr >= 0x20 && addr <= 0x2F) ||
        (addr >= 0x40 && addr <= 0x4F) ||
        (addr >= 0x60 && addr <= 0x6F)) {
        /* Determine digit name */
        int digit_idx = addr & 0x07;
        int plane = (addr >> 5) & 0x03;
        const char *plane_str = "";
        const char *seg_str = "";
        if (plane == 1) plane_str = " Plane P0";
        else if (plane == 2) plane_str = " Plane P1";
        else if (plane == 3) plane_str = " Both Planes";
        if (addr & 0x08) seg_str = " (7 Segment Only)";

        char digit_buf[16];
        max6954_decode_digit(val, digit_buf, sizeof(digit_buf));
        snprintf(buf, sizeof(buf), "Digit %d%s%s: %s", digit_idx, plane_str, seg_str, digit_buf);
        c_put(di, ss, es, s->out_ann, ANN_DIGIT, buf);
        return;
    }

    switch (addr) {
    case 0x00: name = "No-op"; buf[0] = '\0'; break;
    case 0x01: name = "Decode Mode"; fmt_binary(val, buf, sizeof(buf)); break;
    case 0x02: name = "Global Intensity"; max6954_decode_intensity(val, buf, sizeof(buf)); break;
    case 0x03: name = "Scan limit"; snprintf(buf, sizeof(buf), "%d", 1 + val); break;
    case 0x04: name = "Configuration"; max6954_decode_configuration(val, buf, sizeof(buf)); break;
    case 0x05: {
        name = "GPIO Data";
        snprintf(buf, sizeof(buf), "P0: %d, P1: %d, P2: %d, P3: %d, P4: %d",
            val & 1, (val >> 1) & 1, (val >> 2) & 1, (val >> 3) & 1, (val >> 4) & 1);
        break;
    }
    case 0x06: name = "Port Configuration"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x07: name = "Display test"; snprintf(buf, sizeof(buf), "%s", val ? "on" : "off"); break;
    case 0x08: name = "KEY_A Mask"; fmt_binary(val, buf, sizeof(buf)); break;
    case 0x09: name = "KEY_B Mask"; fmt_binary(val, buf, sizeof(buf)); break;
    case 0x0A: name = "KEY_C Mask"; fmt_binary(val, buf, sizeof(buf)); break;
    case 0x0B: name = "KEY_D Mask"; fmt_binary(val, buf, sizeof(buf)); break;
    case 0x0C: name = "Digit Type"; max6954_decode_digit_type(val, buf, sizeof(buf)); break;
    case 0x0D: name = ""; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x0E: name = ""; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x0F: name = ""; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x10: name = "Intensity 10"; max6954_decode_intensity_pair(val, 0, 1, buf, sizeof(buf)); break;
    case 0x11: name = "Intensity 32"; max6954_decode_intensity_pair(val, 2, 3, buf, sizeof(buf)); break;
    case 0x12: name = "Intensity 54"; max6954_decode_intensity_pair(val, 4, 5, buf, sizeof(buf)); break;
    case 0x13: name = "Intensity 76"; max6954_decode_intensity_pair(val, 6, 7, buf, sizeof(buf)); break;
    case 0x14: name = "Intensity 10a (7 Segment Only)"; max6954_decode_intensity_pair(val, 0, 1, buf, sizeof(buf)); break;
    case 0x15: name = "Intensity 32a (7 Segment Only)"; max6954_decode_intensity_pair(val, 2, 3, buf, sizeof(buf)); break;
    case 0x16: name = "Intensity 54a (7 Segment Only)"; max6954_decode_intensity_pair(val, 4, 5, buf, sizeof(buf)); break;
    case 0x17: name = "Intensity 76a (7 Segment Only)"; max6954_decode_intensity_pair(val, 6, 7, buf, sizeof(buf)); break;
    case 0x88: name = "Key_A Debounced"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x89: name = "Key_B Debounced"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x8A: name = "Key_C Debounced"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x8B: name = "Key_D Debounced"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x8C: name = "Key_A Pressed"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x8D: name = "Key_B Pressed"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x8E: name = "Key_C Pressed"; snprintf(buf, sizeof(buf), "not done"); break;
    case 0x8F: name = "Key_D Pressed"; snprintf(buf, sizeof(buf), "not done"); break;
    default:
        snprintf(buf, sizeof(buf), "Unknown register %02X", addr);
        c_put(di, ss, es, s->out_ann, ANN_WARNING, buf);
        return;
    }

    char ann_text[600];
    if (buf[0])
        snprintf(ann_text, sizeof(ann_text), "%s: %s", name, buf);
    else
        snprintf(ann_text, sizeof(ann_text), "%s", name);
    c_put(di, ss, es, s->out_ann, ANN_REG, ann_text);
}

static void max6954_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    max6954_state *s = (max6954_state *)c_decoder_get_private(di);
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
            max6954_handle_register(di, s, s->addr, mosi_byte,
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

static void max6954_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(max6954_state)));
    }
    max6954_state *s = (max6954_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(max6954_state));
}

static void max6954_start(struct srd_decoder_inst *di)
{
    max6954_state *s = (max6954_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "max6954");
    s->pos = 0;
    s->cs_start = 0;
}

static void max6954_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void max6954_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder max6954_c_decoder = {
    .id = "max6954_c",
    .name = "MAX6954(C)",
    .longname = "Maxim MAX6954 (C)",
    .desc = "Maxim MAX6954 LED display driver. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = max6954_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = max6954_ann_rows,
    .inputs = max6954_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = max6954_tags,
    .num_tags = 1,
    .reset = max6954_reset,
    .start = max6954_start,
    .decode = max6954_decode,
    .destroy = max6954_destroy,
    .decode_upper = max6954_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &max6954_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}