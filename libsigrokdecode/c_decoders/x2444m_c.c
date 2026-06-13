/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2018 Stefan Petersen <spe@ciellt.se>
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
    ANN_WRDS = 0,
    ANN_STO,
    ANN_SLEEP,
    ANN_WRITE,
    ANN_WREN,
    ANN_RCL,
    ANN_READ,
    ANN_READ2,
    NUM_ANN,
};

enum {
    STATE_IDLE,
    STATE_CMD,
    STATE_DATA,
};

typedef struct {
    int out_ann;
    int state;
    int cs_asserted;
    uint8_t cmd_byte;
    uint64_t cmd_start;
    uint64_t cs_start;
    int byte_count;
    uint64_t mosi_val;
    uint64_t miso_val;
} x2444m_state;

static const char *x2444m_inputs[] = {"spi", NULL};
static const char *x2444m_tags[] = {"IC", "Memory", NULL};

static const char *x2444m_ann_labels[][3] = {
    {"", "wrds", "Write disable"},
    {"", "sto", "Store RAM data in EEPROM"},
    {"", "sleep", "Enter sleep mode"},
    {"", "write", "Write data into RAM"},
    {"", "wren", "Write enable"},
    {"", "rcl", "Recall EEPROM data into RAM"},
    {"", "read", "Data read from RAM"},
    {"", "read", "Data read from RAM"},
};

static const int x2444m_row_commands_classes[] = {
    ANN_WRDS, ANN_STO, ANN_SLEEP, ANN_WREN, ANN_RCL, -1
};
static const int x2444m_row_data_classes[] = {
    ANN_WRITE, ANN_READ, ANN_READ2, -1
};

static const struct srd_c_ann_row x2444m_ann_rows[] = {
    {"commands", "Commands", x2444m_row_commands_classes, 5},
    {"data", "Data read/write", x2444m_row_data_classes, 3},
};

static const char *x2444m_cmd_name(uint8_t cmd)
{
    switch (cmd & 0x87) {
    case 0x80: return "WRDS";
    case 0x81: return "STO";
    case 0x82: return "SLEEP";
    case 0x83: return "WRITE";
    case 0x84: return "WREN";
    case 0x85: return "RCL";
    case 0x86: return "READ";
    case 0x87: return "READ";
    default: return "UNKNOWN";
    }
}

static int x2444m_cmd_index(uint8_t cmd)
{
    switch (cmd & 0x87) {
    case 0x80: return ANN_WRDS;
    case 0x81: return ANN_STO;
    case 0x82: return ANN_SLEEP;
    case 0x83: return ANN_WRITE;
    case 0x84: return ANN_WREN;
    case 0x85: return ANN_RCL;
    case 0x86: return ANN_READ;
    case 0x87: return ANN_READ2;
    default: return ANN_WRDS;
    }
}

static int __attribute__((unused)) x2444m_is_data_cmd(uint8_t cmd)
{
    uint8_t c = cmd & 0x87;
    return (c == 0x83 || c == 0x86 || c == 0x87);
}

static uint64_t x2444m_read_le64(const c_field *fields)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; i++)
        val |= ((uint64_t)fields[i].u8) << (8 * i);
    return val;
}

static void x2444m_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    x2444m_state *s = (x2444m_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "DATA") == 0) {
        if (!s->cs_asserted) return;
        if (n_fields < 17) return;

        
        
        int have_mosi = (fields[0].u8 & 1) ? 1 : 0;
        int have_miso = ((fields[0].u8 >> 1) & 1) ? 1 : 0;
        uint64_t mosi = have_mosi ? x2444m_read_le64(fields + 1) : 0;
        uint64_t miso = have_miso ? x2444m_read_le64(fields + 9) : 0;

        if (s->byte_count == 0) {
            s->cmd_byte = (uint8_t)(mosi & 0xFF);
            s->cmd_start = start_sample;
            s->mosi_val = 0;
            s->miso_val = 0;
        } else {
            s->mosi_val = (s->mosi_val << 8) | (uint8_t)(mosi & 0xFF);
            s->miso_val = (s->miso_val << 8) | (uint8_t)(miso & 0xFF);
        }
        s->byte_count++;
    } else if (strcmp(cmd, "CS-CHANGE") == 0) {
        int new_cs = (fields && n_fields >= 2) ? fields[1].u8 : 0;
        /* CS active low for SPI */
        s->cs_asserted = (new_cs == 0);

        if (s->cs_asserted) {
            s->byte_count = 0;
            s->cs_start = start_sample;
        } else {
            /* CS deasserted: process the complete transaction */
            if (s->byte_count == 1) {
                /* Simple command only (no data) */
                const char *name = x2444m_cmd_name(s->cmd_byte);
                int idx = x2444m_cmd_index(s->cmd_byte);
                c_put(di, s->cmd_start, end_sample, s->out_ann, idx, name);
            } else if (s->byte_count > 1) {
                /* Command with data (READ or WRITE) */
                const char *name = x2444m_cmd_name(s->cmd_byte);
                int idx = x2444m_cmd_index(s->cmd_byte);
                int addr = (s->cmd_byte >> 3) & 0x0f;

                uint64_t value;
                if (strcmp(name, "READ") == 0)
                    value = s->miso_val;
                else
                    value = s->mosi_val;

                char buf[256];
                snprintf(buf, sizeof(buf), "%s: 0x%x => 0x%llx", name, addr,
                         (unsigned long long)value);
                c_put_v(di, s->cmd_start, end_sample, s->out_ann, idx,
                              value, buf);
            }
            s->byte_count = 0;
        }
    }
}

static void x2444m_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(x2444m_state)));
    }
    x2444m_state *s = (x2444m_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(x2444m_state));
}

static void x2444m_start(struct srd_decoder_inst *di)
{
    x2444m_state *s = (x2444m_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "x2444m");
}

static void x2444m_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void x2444m_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder x2444m_c_decoder = {
    .id = "x2444m_c",
    .name = "X2444M(C)",
    .longname = "Xicor X2444M/P (C)",
    .desc = "Xicor X2444M/P nonvolatile static RAM protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = x2444m_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = x2444m_ann_rows,
    .inputs = x2444m_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = x2444m_tags,
    .num_tags = 2,
    .reset = x2444m_reset,
    .start = x2444m_start,
    .decode = x2444m_decode,
    .destroy = x2444m_destroy,
    .decode_upper = x2444m_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &x2444m_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}