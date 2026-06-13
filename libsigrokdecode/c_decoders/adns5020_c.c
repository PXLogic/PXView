/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Karl Palsson <karlp@tweak.net.au>
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
    ANN_READ = 0,
    ANN_WRITE,
    ANN_WARN,
    NUM_ANN,
};

typedef struct {
    uint8_t mosi_bytes[2];
    int byte_count;
    uint64_t ss_cmd, es_cmd;
    int out_ann;
} adns5020_state;

static const struct { int addr; const char *name; } adns5020_regs[] = {
    {0x00, "Product_ID"},
    {0x01, "Revision_ID"},
    {0x02, "Motion"},
    {0x03, "Delta_X"},
    {0x04, "Delta_Y"},
    {0x05, "SQUAL"},
    {0x06, "Shutter_Upper"},
    {0x07, "Shutter_Lower"},
    {0x08, "Maximum_Pixel"},
    {0x09, "Pixel_Sum"},
    {0x0A, "Minimum_Pixel"},
    {0x0B, "Pixel_Grab"},
    {0x0D, "Mouse_Control"},
    {0x3A, "Chip_Reset"},
    {0x3F, "Inv_Rev_ID"},
    {0x63, "Motion_Burst"},
    {-1, NULL}
};

static const char *adns5020_inputs[] = {"spi", NULL};
static const char *adns5020_tags[] = {"IC", "PC", "Sensor", NULL};

static const char *adns5020_ann_labels[][3] = {
    {"", "read", "Register read commands"},
    {"", "write", "Register write commands"},
    {"", "warning", "Warnings"},
};

static const int adns5020_row_read_classes[] = {ANN_READ, -1};
static const int adns5020_row_write_classes[] = {ANN_WRITE, -1};
static const int adns5020_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row adns5020_ann_rows[] = {
    {"read", "Read", adns5020_row_read_classes, 1},
    {"write", "Write", adns5020_row_write_classes, 1},
    {"warnings", "Warnings", adns5020_row_warnings_classes, 1},
};

static void parse_spi_data(const c_field *fields, int n_fields,
    int *have_mosi, int *have_miso, uint8_t *mosi_byte, uint8_t *miso_byte)
{
    if (n_fields < 1) return;
    *have_mosi = (fields[0].u8 & 1) ? 1 : 0;
    *have_miso = (fields[0].u8 & 2) ? 1 : 0;
    uint64_t mv = 0, sv = 0;
    if (n_fields >= 9) {
        for (int i = 0; i < 8; i++)
            mv |= ((uint64_t)fields[1 + i].u8) << (8 * i);
    }
    if (n_fields >= 17) {
        for (int i = 0; i < 8; i++)
            sv |= ((uint64_t)fields[9 + i].u8) << (8 * i);
    }
    *mosi_byte = (uint8_t)mv;
    *miso_byte = (uint8_t)sv;
}

static void parse_cs_change(const c_field *fields, int n_fields,
    int *cs_old, int *cs_new)
{
    *cs_old = (n_fields > 0) ? (int)fields[0].u8 : -1;
    *cs_new = (n_fields > 1) ? (int)fields[1].u8 : -1;
    if (*cs_old == 0xFF) *cs_old = -1;
}

static const char *adns5020_reg_name(int reg)
{
    for (int i = 0; adns5020_regs[i].name; i++) {
        if (adns5020_regs[i].addr == reg)
            return adns5020_regs[i].name;
    }
    if (reg > 0x63) return "Unknown";
    return "Reserved";
}

static void adns5020_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    adns5020_state *s = (adns5020_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        int cs_old = -1, cs_new = -1;
        parse_cs_change(fields, n_fields, &cs_old, &cs_new);
        if (cs_old == 0 && cs_new == 1) {
            if (s->byte_count != 0 && s->byte_count != 2) {
                c_put(di, s->ss_cmd, end_sample, s->out_ann, ANN_WARN, "Misplaced CS#!");
            }
            s->byte_count = 0;
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0) return;

    int have_mosi = 0, have_miso = 0;
    uint8_t mosi = 0, miso = 0;
    parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi, &miso);
    if (!have_mosi) return;

    if (s->byte_count == 0)
        s->ss_cmd = start_sample;
    s->mosi_bytes[s->byte_count++] = mosi;

    if (s->byte_count != 2) return;

    s->es_cmd = end_sample;
    uint8_t c = s->mosi_bytes[0], arg = s->mosi_bytes[1];
    int write = c & 0x80;
    int reg = c & 0x7f;
    const char *reg_desc = adns5020_reg_name(reg);

    char buf[256];
    if (write) {
        snprintf(buf, sizeof(buf), "%s: %02X", reg_desc, arg);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WRITE, buf);
    } else {
        snprintf(buf, sizeof(buf), "%s: %02X", reg_desc, arg);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_READ, buf);
    }
    s->byte_count = 0;
}

static void adns5020_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(adns5020_state)));
    }
    adns5020_state *s = (adns5020_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(adns5020_state));
}

static void adns5020_start(struct srd_decoder_inst *di)
{
    adns5020_state *s = (adns5020_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "adns5020");
}

static void adns5020_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void adns5020_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder adns5020_c_decoder = {
    .id = "adns5020_c",
    .name = "ADNS-5020(C)",
    .longname = "Avago ADNS-5020 (C)",
    .desc = "Bidirectional optical mouse sensor protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = adns5020_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = adns5020_ann_rows,
    .inputs = adns5020_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = adns5020_tags,
    .num_tags = 3,
    .reset = adns5020_reset,
    .start = adns5020_start,
    .decode = adns5020_decode,
    .destroy = adns5020_destroy,
    .decode_upper = adns5020_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &adns5020_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}