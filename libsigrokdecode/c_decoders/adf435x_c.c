/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Joel Holdsworth <joel@airwebreathe.org.uk>
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

enum {
    ANN_REG = 0,
    ANN_WARN,
    NUM_ANN,
};

/* Field description: offset (LSB), width, name, parser, checker */
typedef struct {
    int offset;
    int width;
    const char *name;
    const char *(*parser)(int v);
    const char *(*checker)(int v);
} adf435x_field_desc;

/* Parser/checker static string storage - single entry per call */
static char adf435x_fmt_buf[128];

static const char *adf435x_parse_disabled_enabled(int v)
{
    return v ? "Enabled" : "Disabled";
}

static const char *adf435x_parse_prescalar(int v)
{
    return v ? "8/9" : "4/5";
}

static const char *adf435x_parse_phase_adjust(int v)
{
    return v ? "On" : "Off";
}

static const char *adf435x_parse_pd_polarity(int v)
{
    return v ? "Positive" : "Negative";
}

static const char *adf435x_parse_ldp(int v)
{
    return v ? "6ns" : "10ns";
}

static const char *adf435x_parse_ldf(int v)
{
    return v ? "INT-N" : "FRAC-N";
}

static const char *adf435x_parse_cp_current(int v)
{
    static const double cp_currents[] = {
        0.31, 0.63, 0.94, 1.25, 1.56, 1.88, 2.19, 2.50,
        2.81, 3.13, 3.44, 3.75, 4.06, 4.38, 4.69, 5.00,
    };
    if (v >= 0 && v < 16)
        snprintf(adf435x_fmt_buf, sizeof(adf435x_fmt_buf),
                 "%.2fmA @ 5.1kΩ", cp_currents[v]);
    else
        snprintf(adf435x_fmt_buf, sizeof(adf435x_fmt_buf), "?");
    return adf435x_fmt_buf;
}

static const char *adf435x_parse_muxout(int v)
{
    static const char *texts[] = {
        "Three-State Output", "DVdd", "DGND",
        "R Counter Output", "N Divider Output",
        "Analog Lock Detect", "Digital Lock Detect",
        "Reserved",
    };
    if (v >= 0 && v < 8) return texts[v];
    return "?";
}

static const char *adf435x_parse_low_noise_spur(int v)
{
    static const char *texts[] = {
        "Low Noise Mode", "Reserved", "Reserved", "Low Spur Mode",
    };
    if (v >= 0 && v < 4) return texts[v];
    return "?";
}

static const char *adf435x_parse_clk_div_mode(int v)
{
    static const char *texts[] = {
        "Clock Divider Off", "Fast Lock Enable",
        "Resync Enable", "Reserved",
    };
    if (v >= 0 && v < 4) return texts[v];
    return "?";
}

static const char *adf435x_parse_abp(int v)
{
    return v ? "3ns (INT-N)" : "6ns (FRAC-N)";
}

static const char *adf435x_parse_band_sel_clk_mode(int v)
{
    return v ? "High" : "Low";
}

static const char *adf435x_parse_output_power(int v)
{
    static const int powers[] = {-4, -1, 2, 5};
    if (v >= 0 && v < 4) {
        snprintf(adf435x_fmt_buf, sizeof(adf435x_fmt_buf), "%+ddBm", powers[v]);
        return adf435x_fmt_buf;
    }
    return "?";
}

static const char *adf435x_parse_aux_output_select(int v)
{
    return v ? "Fundamental" : "Divided Output";
}

static const char *adf435x_parse_vco_powerdown(int v)
{
    return v ? "VCO Powered Down" : "VCO Powered Up";
}

static const char *adf435x_parse_rf_divider(int v)
{
    snprintf(adf435x_fmt_buf, sizeof(adf435x_fmt_buf), "÷%d", 1 << v);
    return adf435x_fmt_buf;
}

static const char *adf435x_parse_feedback_select(int v)
{
    return v ? "Fundamental" : "Divided";
}

static const char *adf435x_parse_ld_pin_mode(int v)
{
    static const char *texts[] = {"Low", "Digital Lock Detect", "Low", "High"};
    if (v >= 0 && v < 4) return texts[v];
    return "?";
}

static const char *adf435x_check_int(int v)
{
    return (v < 23) ? "Not Allowed" : NULL;
}

/* Register 0 fields */
static const adf435x_field_desc reg0_fields[] = {
    { 3, 12, "FRAC", NULL, NULL},
    {15, 16, "INT", NULL, adf435x_check_int},
    {-1,  0, NULL, NULL, NULL},
};

/* Register 1 fields */
static const adf435x_field_desc reg1_fields[] = {
    { 3, 12, "MOD", NULL, NULL},
    {15, 12, "Phase", NULL, NULL},
    {27,  1, "Prescalar", adf435x_parse_prescalar, NULL},
    {28,  1, "Phase Adjust", adf435x_parse_phase_adjust, NULL},
    {-1,  0, NULL, NULL, NULL},
};

/* Register 2 fields */
static const adf435x_field_desc reg2_fields[] = {
    { 3,  1, "Counter Reset", adf435x_parse_disabled_enabled, NULL},
    { 4,  1, "Charge Pump Three-State", adf435x_parse_disabled_enabled, NULL},
    { 5,  1, "Power-Down", adf435x_parse_disabled_enabled, NULL},
    { 6,  1, "PD Polarity", adf435x_parse_pd_polarity, NULL},
    { 7,  1, "LDP", adf435x_parse_ldp, NULL},
    { 8,  1, "LDF", adf435x_parse_ldf, NULL},
    { 9,  4, "Charge Pump Current Setting", adf435x_parse_cp_current, NULL},
    {13,  1, "Double Buffer", adf435x_parse_disabled_enabled, NULL},
    {14, 10, "R Counter", NULL, NULL},
    {24,  1, "RDIV2", adf435x_parse_disabled_enabled, NULL},
    {25,  1, "Reference Doubler", adf435x_parse_disabled_enabled, NULL},
    {26,  3, "MUXOUT", adf435x_parse_muxout, NULL},
    {29,  2, "Low Noise and Low Spur Modes", adf435x_parse_low_noise_spur, NULL},
    {-1,  0, NULL, NULL, NULL},
};

/* Register 3 fields */
static const adf435x_field_desc reg3_fields[] = {
    { 3, 12, "Clock Divider", NULL, NULL},
    {15,  2, "Clock Divider Mode", adf435x_parse_clk_div_mode, NULL},
    {18,  1, "CSR Enable", adf435x_parse_disabled_enabled, NULL},
    {21,  1, "Charge Cancellation", adf435x_parse_disabled_enabled, NULL},
    {22,  1, "ABP", adf435x_parse_abp, NULL},
    {23,  1, "Band Select Clock Mode", adf435x_parse_band_sel_clk_mode, NULL},
    {-1,  0, NULL, NULL, NULL},
};

/* Register 4 fields */
static const adf435x_field_desc reg4_fields[] = {
    { 3,  2, "Output Power", adf435x_parse_output_power, NULL},
    { 5,  1, "Output Enable", adf435x_parse_disabled_enabled, NULL},
    { 6,  2, "AUX Output Power", adf435x_parse_output_power, NULL},
    { 8,  1, "AUX Output Select", adf435x_parse_aux_output_select, NULL},
    { 9,  1, "AUX Output Enable", adf435x_parse_disabled_enabled, NULL},
    {10,  1, "MTLD", adf435x_parse_disabled_enabled, NULL},
    {11,  1, "VCO Power-Down", adf435x_parse_vco_powerdown, NULL},
    {12,  8, "Band Select Clock Divider", NULL, NULL},
    {20,  3, "RF Divider Select", adf435x_parse_rf_divider, NULL},
    {23,  1, "Feedback Select", adf435x_parse_feedback_select, NULL},
    {-1,  0, NULL, NULL, NULL},
};

/* Register 5 fields */
static const adf435x_field_desc reg5_fields[] = {
    {22,  2, "LD Pin Mode", adf435x_parse_ld_pin_mode, NULL},
    {-1,  0, NULL, NULL, NULL},
};

static const adf435x_field_desc *reg_fields[] = {
    reg0_fields, reg1_fields, reg2_fields,
    reg3_fields, reg4_fields, reg5_fields,
};

typedef struct {
    uint8_t bits[32];
    int bit_count;
    uint64_t bit_ss[32];
    uint64_t bit_es[32];
    int out_ann;
} adf435x_state;

static const char *adf435x_inputs[] = {"spi", NULL};
static const char *adf435x_tags[] = {"Clock/timing", "IC", "Wireless/RF", NULL};

static const char *adf435x_ann_labels[][3] = {
    {"", "write", "Register write"},
    {"", "warning", "Warnings"},
};

static const int adf435x_row_writes_classes[] = {ANN_REG};
static const int adf435x_row_warnings_classes[] = {ANN_WARN};
static const struct srd_c_ann_row adf435x_ann_rows[] = {
    {"writes", "Register writes", adf435x_row_writes_classes, 1},
    {"warnings", "Warnings", adf435x_row_warnings_classes, 1},
};

static uint32_t adf435x_extract_field(uint32_t word, int offset, int width)
{
    uint32_t mask = (1U << width) - 1;
    return (word >> offset) & mask;
}

static void adf435x_decode_word(struct srd_decoder_inst *di, adf435x_state *s)
{
    if (s->bit_count != 32) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "Frame error: Bit count: want 32, got %d", s->bit_count);
        c_put(di, s->bit_ss[0], s->bit_es[s->bit_count - 1],
                  s->out_ann, ANN_WARN, buf);
        return;
    }

    /* Pack MSB-ordered bits into 32-bit word */
    uint32_t word = 0;
    for (int i = 0; i < 32; i++)
        word = (word << 1) | s->bits[i];

    /* Extract register address (low 3 bits) */
    int reg_addr = word & 0x7;

    /* Output register address annotation */
    /* reg_addr bits are at positions [0..2] in LSB order,
       which maps to MSB positions [29..31] in our bits[] array */
    uint64_t reg_ss = s->bit_ss[31];
    uint64_t reg_es = s->bit_es[31];
    char buf[32];
    snprintf(buf, sizeof(buf), "Register: %d", reg_addr);
    c_put(di, reg_ss, reg_es, s->out_ann, ANN_REG, buf);

    /* Parse fields */
    if (reg_addr < 0 || reg_addr > 5) return;
    const adf435x_field_desc *fields = reg_fields[reg_addr];
    if (!fields) return;

    for (int i = 0; fields[i].name != NULL; i++) {
        uint32_t val = adf435x_extract_field(word, fields[i].offset, fields[i].width);
        const char *formatted = NULL;
        char auto_buf[32];

        if (fields[i].parser) {
            formatted = fields[i].parser(val);
        } else {
            snprintf(auto_buf, sizeof(auto_buf), "%u", val);
            formatted = auto_buf;
        }

        if (formatted) {
            char text[160];
            snprintf(text, sizeof(text), "%s: %s", fields[i].name, formatted);
            /* Calculate field ss/es from bit timestamps
               field offset is LSB order, bits[] is MSB order
               LSB bit N maps to MSB index (31 - N)
               Field spans [offset, offset+width-1] in LSB
               In MSB order: [31-(offset+width-1), 31-offset] */
            int msb_start = 31 - (fields[i].offset + fields[i].width - 1);
            int msb_end = 31 - fields[i].offset;
            if (msb_start >= 0 && msb_start < 32 && msb_end >= 0 && msb_end < 32) {
                c_put(di, s->bit_ss[msb_start], s->bit_es[msb_end],
                          s->out_ann, ANN_REG, text);
            }
        }

        if (fields[i].checker) {
            const char *warn = fields[i].checker(val);
            if (warn) {
                int msb_start = 31 - (fields[i].offset + fields[i].width - 1);
                int msb_end = 31 - fields[i].offset;
                if (msb_start >= 0 && msb_start < 32 && msb_end >= 0 && msb_end < 32) {
                    c_put(di, s->bit_ss[msb_start], s->bit_es[msb_end],
                              s->out_ann, ANN_WARN, warn);
                }
            }
        }
    }
}

static void adf435x_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    adf435x_state *s = (adf435x_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "TRANSFER") == 0) {
        if (s->bit_count > 0)
            adf435x_decode_word(di, s);
        s->bit_count = 0;
    } else if (strcmp(cmd, "BITS") == 0) {
        if (n_fields < 2) return;
        int pos = 0;
        uint8_t flags = fields[pos++].u8;
        int have_mosi = (flags & 1) ? 1 : 0;
        
        if (have_mosi) {
            int mosi_count = (int)fields[pos++].u8;
            for (int i = 0; i < mosi_count && s->bit_count < 32 && pos + 17 <= (int)n_fields; i++) {
                s->bits[s->bit_count] = fields[pos++].u8;
                s->bit_ss[s->bit_count] = 0;
                for (int b = 0; b < 8; b++)
                    s->bit_ss[s->bit_count] |= ((uint64_t)fields[pos++].u8) << (8 * b);
                s->bit_es[s->bit_count] = 0;
                for (int b = 0; b < 8; b++)
                    s->bit_es[s->bit_count] |= ((uint64_t)fields[pos++].u8) << (8 * b);
                s->bit_count++;
            }
        }
    }
}

static void adf435x_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(adf435x_state)));
    }
    adf435x_state *s = (adf435x_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(adf435x_state));
}

static void adf435x_start(struct srd_decoder_inst *di)
{
    adf435x_state *s = (adf435x_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "adf435x");
}

static void adf435x_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void adf435x_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder adf435x_c_decoder = {
    .id = "adf435x_c",
    .name = "ADF435x(C)",
    .longname = "Analog Devices ADF4350/1 (C)",
    .desc = "Wideband synthesizer with integrated VCO. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = adf435x_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = adf435x_ann_rows,
    .inputs = adf435x_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = adf435x_tags,
    .num_tags = 3,
    .reset = adf435x_reset,
    .start = adf435x_start,
    .decode = adf435x_decode,
    .destroy = adf435x_destroy,
    .decode_upper = adf435x_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &adf435x_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}