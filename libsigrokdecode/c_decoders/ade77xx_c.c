/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Karl Palsson <karlp@etactica.com>
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
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
    const char *name;
    int bits;
} ade77xx_reg_info;

/* Register table from lists.py - name and bit width */
static const ade77xx_reg_info ade77xx_regs[0x80] = {
    [0x01] = {"AWATTHR",  16},
    [0x02] = {"BWATTHR",  16},
    [0x03] = {"CWATTHR",  16},
    [0x04] = {"AVARHR",   16},
    [0x05] = {"BVARHR",   16},
    [0x06] = {"CVARHR",   16},
    [0x07] = {"AVAHR",    16},
    [0x08] = {"BVAHR",    16},
    [0x09] = {"CVAHR",    16},
    [0x0A] = {"AIRMS",    24},
    [0x0B] = {"BIRMS",    24},
    [0x0C] = {"CIRMS",    24},
    [0x0D] = {"AVRMS",    24},
    [0x0E] = {"BVRMS",    24},
    [0x0F] = {"CVRMS",    24},
    [0x10] = {"FREQ",     12},
    [0x11] = {"TEMP",      8},
    [0x12] = {"WFORM",    24},
    [0x13] = {"OPMODE",    8},
    [0x14] = {"MMODE",     8},
    [0x15] = {"WAVMODE",   8},
    [0x16] = {"COMPMODE",  8},
    [0x17] = {"LCYCMODE",  8},
    [0x18] = {"Mask",     24},
    [0x19] = {"Status",   24},
    [0x1A] = {"RSTATUS",  24},
    [0x1B] = {"ZXTOUT",   16},
    [0x1C] = {"LINECYC",  16},
    [0x1D] = {"SAGCYC",    8},
    [0x1E] = {"SAGLVL",    8},
    [0x1F] = {"VPINTLVL",  8},
    [0x20] = {"IPINTLVL",  8},
    [0x21] = {"VPEAK",     8},
    [0x22] = {"IPEAK",     8},
    [0x23] = {"Gain",      8},
    [0x24] = {"AVRMSGAIN", 12},
    [0x25] = {"BVRMSGAIN", 12},
    [0x26] = {"CVRMSGAIN", 12},
    [0x27] = {"AIGAIN",    12},
    [0x28] = {"BIGAIN",    12},
    [0x29] = {"CIGAIN",    12},
    [0x2A] = {"AWG",       12},
    [0x2B] = {"BWG",       12},
    [0x2C] = {"CWG",       12},
    [0x2D] = {"AVARG",     12},
    [0x2E] = {"BVARG",     12},
    [0x2F] = {"CVARG",     12},
    [0x30] = {"AVAG",      12},
    [0x31] = {"BVAG",      12},
    [0x32] = {"CVAG",      12},
    [0x33] = {"AVRMSOS",   12},
    [0x34] = {"BVRMSOS",   12},
    [0x35] = {"CVRMSOS",   12},
    [0x36] = {"AIRMSOS",   12},
    [0x37] = {"BIRMSOS",   12},
    [0x38] = {"CIRMSOS",   12},
    [0x39] = {"AWATTOS",   12},
    [0x3A] = {"BWATTOS",   12},
    [0x3B] = {"CWATTOS",   12},
    [0x3C] = {"AVAROS",    12},
    [0x3D] = {"BVAROS",    12},
    [0x3E] = {"CVAROS",    12},
    [0x3F] = {"APHCAL",     7},
    [0x40] = {"BPHCAL",     7},
    [0x41] = {"CPHCAL",     7},
    [0x42] = {"WDIV",       8},
    [0x43] = {"VARDIV",     8},
    [0x44] = {"VADIV",      8},
    [0x45] = {"APCFNUM",   16},
    [0x46] = {"APCFDEN",   12},
    [0x47] = {"VARCFNUM",  16},
    [0x48] = {"VARCFDEN",  12},
    [0x7E] = {"CHKSUM",     8},
    [0x7F] = {"Version",    8},
};

typedef struct {
    uint8_t mosi_bytes[4];
    uint8_t miso_bytes[4];
    int byte_count;
    int expected;
    uint64_t ss_cmd, es_cmd;
    int out_ann;
} ade77xx_state;

static const char *ade77xx_inputs[] = {"spi", NULL};
static const char *ade77xx_tags[] = {"Analog/digital", "IC", "Sensor", NULL};

static const char *ade77xx_ann_labels[][3] = {
    {"", "read", "Register read commands"},
    {"", "write", "Register write commands"},
    {"", "warning", "Warnings"},
};

static const int ade77xx_row_read_classes[] = {ANN_READ};
static const int ade77xx_row_write_classes[] = {ANN_WRITE};
static const int ade77xx_row_warnings_classes[] = {ANN_WARN};
static const struct srd_c_ann_row ade77xx_ann_rows[] = {
    {"read", "Read", ade77xx_row_read_classes, 1},
    {"write", "Write", ade77xx_row_write_classes, 1},
    {"warnings", "Warnings", ade77xx_row_warnings_classes, 1},
};

static void parse_spi_data(const c_field *fields, int n_fields,
    int *have_mosi, int *have_miso, uint64_t *mosi_val, uint64_t *miso_val)
{
    int pos = 0;
    uint8_t flags = fields[pos++].u8;
    *have_mosi = (flags & 1) ? 1 : 0;
    *have_miso = (flags & 2) ? 1 : 0;

    *mosi_val = 0;
    if (*have_mosi) {
        for (int i = 0; i < 8 && pos < (int)n_fields; i++)
            *mosi_val |= ((uint64_t)fields[pos++].u8) << (8 * i);
    }

    *miso_val = 0;
    if (*have_miso) {
        for (int i = 0; i < 8 && pos < (int)n_fields; i++)
            *miso_val |= ((uint64_t)fields[pos++].u8) << (8 * i);
    }
}

static void ade77xx_reset_data(ade77xx_state *s)
{
    s->expected = 0;
    s->byte_count = 0;
}

static void ade77xx_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ade77xx_state *s = (ade77xx_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        uint8_t cs_old = (n_fields > 0) ? fields[0].u8 : 0xFF;
        uint8_t cs_new = (n_fields > 1) ? fields[1].u8 : 0;

        if (cs_old == 0 && cs_new == 1) {
            if (s->byte_count > 1 && (s->byte_count - 1) < s->expected) {
                uint8_t cmd_byte = s->mosi_bytes[0];
                int write = cmd_byte & 0x80;
                int reg = cmd_byte & 0x7f;
                const ade77xx_reg_info *ri = &ade77xx_regs[reg];
                int idx = write ? ANN_WRITE : ANN_READ;
                char buf[256];
                snprintf(buf, sizeof(buf), "%s: SHORT", ri->name);
                c_put(di, s->ss_cmd, end_sample, s->out_ann, idx, buf);
                c_put(di, s->ss_cmd, end_sample, s->out_ann, ANN_WARN,
                          "Short transfer!");
            }
            ade77xx_reset_data(s);
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0) return;

    int have_mosi, have_miso;
    uint64_t mosi_val, miso_val;
    parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi_val, &miso_val);

    if (s->byte_count == 0) s->ss_cmd = start_sample;

    if (s->byte_count < 4) {
        s->mosi_bytes[s->byte_count] = (uint8_t)mosi_val;
        s->miso_bytes[s->byte_count] = (uint8_t)miso_val;
        s->byte_count++;
    }

    if (s->byte_count < 2) return;

    uint8_t cmd_byte = s->mosi_bytes[0];
    int write = cmd_byte & 0x80;
    int reg = cmd_byte & 0x7f;

    if (reg >= 0x80 || ade77xx_regs[reg].name == NULL) {
        c_put(di, s->ss_cmd, end_sample, s->out_ann, ANN_WARN,
                  "Unknown register!");
        ade77xx_reset_data(s);
        return;
    }

    const ade77xx_reg_info *ri = &ade77xx_regs[reg];
    s->expected = (ri->bits + 7) / 8;

    if ((s->byte_count - 1) != s->expected) return;

    s->es_cmd = end_sample;

    uint32_t valo = 0;
    uint32_t vali = 0;
    if (s->expected == 3) {
        valo = (uint32_t)s->mosi_bytes[1] << 16 | (uint32_t)s->mosi_bytes[2] << 8 | s->mosi_bytes[3];
        vali = (uint32_t)s->miso_bytes[1] << 16 | (uint32_t)s->miso_bytes[2] << 8 | s->miso_bytes[3];
    } else if (s->expected == 2) {
        valo = (uint32_t)s->mosi_bytes[1] << 8 | s->mosi_bytes[2];
        vali = (uint32_t)s->miso_bytes[1] << 8 | s->miso_bytes[2];
    } else {
        valo = s->mosi_bytes[1];
        vali = s->miso_bytes[1];
    }
    (void)vali;

    int idx = write ? ANN_WRITE : ANN_READ;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: {$}", ri->name);
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, idx, buf);

    char hex_buf[16];
    snprintf(hex_buf, sizeof(hex_buf), "@%02X", valo);
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, idx, hex_buf);

    ade77xx_reset_data(s);
}

static void ade77xx_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ade77xx_state)));
    }
    ade77xx_state *s = (ade77xx_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ade77xx_state));
}

static void ade77xx_start(struct srd_decoder_inst *di)
{
    ade77xx_state *s = (ade77xx_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ade77xx");
}

static void ade77xx_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ade77xx_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ade77xx_c_decoder = {
    .id = "ade77xx_c",
    .name = "ADE77xx(C)",
    .longname = "Analog Devices ADE77xx (C)",
    .desc = "Poly phase multifunction energy metering IC protocol. (C implementation)",
    .license = "mit",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ade77xx_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ade77xx_ann_rows,
    .inputs = ade77xx_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ade77xx_tags,
    .num_tags = 3,
    .reset = ade77xx_reset,
    .start = ade77xx_start,
    .decode = ade77xx_decode,
    .destroy = ade77xx_destroy,
    .decode_upper = ade77xx_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ade77xx_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}