/*
 * This file is part of the libsigrokdecode project.
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
    ANN_COMMANDFRAME = 0,
    ANN_READDATAFRAME,
    ANN_WRITEDATAFRAME,
    ANN_REGISTERREAD,
    ANN_REGISTERWRITE,
    ANN_WARN,
    ANN_FIELD,
    NUM_ANN,
};

enum {
    AS5047_STATE_INIT = 0,
    AS5047_STATE_READ,
    AS5047_STATE_WRITE,
};

typedef struct {
    int state;
    uint64_t transaction_start;
    uint16_t current_reg;
    int out_ann;
    /* 16-bit frame assembly for 8-bit SPI mode */
    int byte_idx;
    uint16_t mosi_word;
    uint16_t miso_word;
    uint64_t frame_ss;
} as5047_state;

static const struct { uint16_t addr; const char *name; } as5047_regs[] = {
    {0x0000, "NOP"},
    {0x0001, "ERRFL"},
    {0x0003, "PROG"},
    {0x0016, "ZPOSM"},
    {0x0017, "ZPOSL"},
    {0x0018, "SETTINGS1"},
    {0x0019, "SETTINGS2"},
    {0x3FFC, "DIAAGC"},
    {0x3FFD, "MAG"},
    {0x3FFE, "ANGLEUNC"},
    {0x3FFF, "ANGLECOM"},
    {0xFFFF, NULL}
};

static const char *as5047_inputs[] = {"spi", NULL};
static const char *as5047_tags[] = {"Embedded/industrial", NULL};

static const char *as5047_ann_labels[][3] = {
    {"", "commandframe", "command frame"},
    {"", "readdataframe", "read data frame"},
    {"", "writedataframe", "write data frame"},
    {"", "registerread", "register read"},
    {"", "registerwrite", "register write"},
    {"", "warning", "warning"},
    {"", "field", "field"},
};

static const int as5047_row_fields_classes[] = {ANN_FIELD, -1};
static const int as5047_row_frames_classes[] = {ANN_COMMANDFRAME, ANN_READDATAFRAME, ANN_WRITEDATAFRAME, -1};
static const int as5047_row_transactions_classes[] = {ANN_REGISTERREAD, ANN_REGISTERWRITE, -1};
static const int as5047_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row as5047_ann_rows[] = {
    {"fields", "fields", as5047_row_fields_classes, 1},
    {"frames", "frames", as5047_row_frames_classes, 3},
    {"transactions", "transactions", as5047_row_transactions_classes, 2},
    {"warnings", "warnings", as5047_row_warnings_classes, 1},
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

static const char *as5047_reg_name(uint16_t addr)
{
    for (int i = 0; as5047_regs[i].name; i++) {
        if (as5047_regs[i].addr == addr)
            return as5047_regs[i].name;
    }
    return "unknown";
}

static int popcount_parity(uint16_t v)
{
    int count = 0;
    while (v) { count += v & 1; v >>= 1; }
    return count % 2;
}

static void as5047_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    as5047_state *s = (as5047_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        int cs_old = -1, cs_new = -1;
        parse_cs_change(fields, n_fields, &cs_old, &cs_new);
        if (cs_old == 0 && cs_new == 1) {
            s->state = AS5047_STATE_INIT;
            s->byte_idx = 0;
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0) return;

    int have_mosi = 0, have_miso = 0;
    uint8_t mosi_b = 0, miso_b = 0;
    parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi_b, &miso_b);

    /* Detect if SPI wordsize is 16 (mosi_byte > 0xFF won't happen, but
       if n_fields >= 9 the mosi value could be > 255 for 16-bit words).
       For 16-bit SPI, each DATA callback gives a full 16-bit word.
       For 8-bit SPI, we need to assemble two bytes.
       We use byte_idx to track: 0 = waiting for high byte, 1 = waiting for low byte.
       If we get a value > 0xFF in the uint64_t, it's 16-bit mode. */

    /* Check if this looks like a 16-bit word by examining the raw mosi value */
    uint64_t mosi_raw = 0, miso_raw = 0;
    if (n_fields >= 9) {
        for (int i = 0; i < 8; i++)
            mosi_raw |= ((uint64_t)fields[1 + i].u8) << (8 * i);
    }
    if (n_fields >= 17) {
        for (int i = 0; i < 8; i++)
            miso_raw |= ((uint64_t)fields[9 + i].u8) << (8 * i);
    }

    int is_16bit = (mosi_raw > 0xFF || miso_raw > 0xFF) ? 1 : 0;

    uint16_t mosi_word, miso_word;

    if (is_16bit) {
        mosi_word = (uint16_t)mosi_raw;
        miso_word = (uint16_t)miso_raw;
    } else {
        /* 8-bit SPI: assemble two bytes into 16-bit word (MSB first) */
        if (s->byte_idx == 0) {
            s->mosi_word = (uint16_t)mosi_b << 8;
            s->miso_word = (uint16_t)miso_b << 8;
            s->frame_ss = start_sample;
            s->byte_idx = 1;
            return;
        }
        s->mosi_word |= mosi_b;
        s->miso_word |= miso_b;
        mosi_word = s->mosi_word;
        miso_word = s->miso_word;
        s->byte_idx = 0;
    }

    uint64_t frame_ss = is_16bit ? start_sample : s->frame_ss;

    if (s->state == AS5047_STATE_INIT || s->state == AS5047_STATE_WRITE) {
        if (have_mosi && popcount_parity(mosi_word) != 0) {
            c_put(di, frame_ss, end_sample, s->out_ann, ANN_WARN, "mosi parity");
        }
    }

    if (s->state == AS5047_STATE_INIT) {
        s->transaction_start = frame_ss;
        uint16_t reg = mosi_word & 0x3FFF;
        s->current_reg = reg;
        const char *reg_desc = as5047_reg_name(reg);

        if (mosi_word & 0x4000) {
            s->state = AS5047_STATE_READ;
            char buf[256];
            snprintf(buf, sizeof(buf), "read from %s (0x%04x)", reg_desc, reg);
            c_put(di, frame_ss, end_sample, s->out_ann, ANN_COMMANDFRAME, buf);
        } else {
            s->state = AS5047_STATE_WRITE;
            char buf[256];
            snprintf(buf, sizeof(buf), "write to %s (0x%04x)", reg_desc, reg);
            c_put(di, frame_ss, end_sample, s->out_ann, ANN_COMMANDFRAME, buf);
        }
        s->transaction_start = frame_ss;
    } else {
        if (have_miso && popcount_parity(miso_word) != 0) {
            c_put(di, frame_ss, end_sample, s->out_ann, ANN_WARN, "miso parity");
        }
        const char *reg_desc = as5047_reg_name(s->current_reg);

        if (s->state == AS5047_STATE_READ) {
            if (miso_word & 0x4000) {
                c_put(di, frame_ss, end_sample, s->out_ann, ANN_WARN, "error flag set");
            }
            uint16_t rdata = miso_word & 0x3FFF;
            char buf[256];
            snprintf(buf, sizeof(buf), "read data frame: 0x%04x", rdata);
            c_put(di, frame_ss, end_sample, s->out_ann, ANN_READDATAFRAME, buf);
            snprintf(buf, sizeof(buf), "Read 0x%04x from %s", rdata, reg_desc);
            c_put(di, s->transaction_start, end_sample, s->out_ann, ANN_REGISTERREAD, buf);
        }
        if (s->state == AS5047_STATE_WRITE) {
            uint16_t wdata = mosi_word & 0x3FFF;
            char buf[256];
            snprintf(buf, sizeof(buf), "write data frame: 0x%04x", wdata);
            c_put(di, frame_ss, end_sample, s->out_ann, ANN_WRITEDATAFRAME, buf);
            snprintf(buf, sizeof(buf), "Write 0x%04x to %s", wdata, reg_desc);
            c_put(di, s->transaction_start, end_sample, s->out_ann, ANN_REGISTERWRITE, buf);
        }

        s->state = AS5047_STATE_INIT;
    }
}

static void as5047_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(as5047_state)));
    }
    as5047_state *s = (as5047_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(as5047_state));
    s->state = AS5047_STATE_INIT;
}

static void as5047_start(struct srd_decoder_inst *di)
{
    as5047_state *s = (as5047_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "as5047");
}

static void as5047_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void as5047_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder as5047_c_decoder = {
    .id = "as5047_c",
    .name = "AS5047(C)",
    .longname = "AS5047 (C)",
    .desc = "AS5047 magnetic rotary encoder. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = as5047_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = as5047_ann_rows,
    .inputs = as5047_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = as5047_tags,
    .num_tags = 1,
    .reset = as5047_reset,
    .start = as5047_start,
    .decode = as5047_decode,
    .destroy = as5047_destroy,
    .decode_upper = as5047_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &as5047_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}