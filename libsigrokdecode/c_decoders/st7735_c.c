/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2018 Aleksander Alekseev <afiskon@gmail.com>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BIT = 0,
    ANN_CMD,
    ANN_DATA,
    ANN_DESC,
    NUM_ANN,
};

#define ST7735_MAX_DATA_LEN 128

#define CH_CS  0
#define CH_CLK 1
#define CH_MOSI 2
#define CH_DC  3

typedef struct {
    int accum_byte;
    int accum_bits_num;
    uint64_t bit_ss;
    uint64_t byte_ss;
    int current_bit;

    int current_cmd;
    uint8_t current_data[ST7735_MAX_DATA_LEN];
    int current_data_len;
    uint64_t desc_ss;
    uint64_t desc_es;

    int out_ann;
} st7735_state;

/* Command table entry */
typedef struct {
    uint8_t cmd;
    const char *name;
    const char *desc;
} st7735_cmd_entry;

static const st7735_cmd_entry st7735_cmd_table[] = {
    {0x00, "NOP",     "No operation"},
    {0x01, "SWRESET", "Software reset"},
    {0x04, "RDDID",   "Read display ID"},
    {0x09, "RDDST",   "Read display status"},
    {0x10, "SLPIN",   "Sleep in & booster off"},
    {0x11, "SLPOUT",  "Sleep out & booster on"},
    {0x12, "PTLON",   "Partial mode on"},
    {0x13, "NORON",   "Partial off (normal)"},
    {0x20, "INVOFF",  "Display inversion off"},
    {0x21, "INVON",   "Display inversion on"},
    {0x28, "DISPOFF", "Display off"},
    {0x29, "DISPON",  "Display on"},
    {0x2A, "CASET",   "Column address set"},
    {0x2B, "RASET",   "Row address set"},
    {0x2C, "RAMWR",   "Memory write"},
    {0x2E, "RAMRD",   "Memory read"},
    {0x30, "PTLAR",   "Partial start/end address set"},
    {0x36, "MADCTL",  "Memory data address control"},
    {0x3A, "COLMOD",  "Interface pixel format"},
    {0xB1, "FRMCTR1", "Frame rate control (in normal mode / full colors)"},
    {0xB2, "FRMCTR2", "Frame rate control (in idle mode / 8-colors)"},
    {0xB3, "FRMCTR3", "Frame rate control (in partial mode / full colors)"},
    {0xB4, "INVCTR",  "Display inversion control"},
    {0xB6, "DISSET5", "Display function set 5"},
    {0xC0, "PWCTR1",  "Power control 1"},
    {0xC1, "PWCTR2",  "Power control 2"},
    {0xC2, "PWCTR3",  "Power control 3"},
    {0xC3, "PWCTR4",  "Power control 4"},
    {0xC4, "PWCTR5",  "Power control 5"},
    {0xC5, "VMCTR1",  "VCOM control 1"},
    {0xDA, "RDID1",   "Read ID1"},
    {0xDB, "RDID2",   "Read ID2"},
    {0xDC, "RDID3",   "Read ID3"},
    {0xDD, "RDID4",   "Read ID4"},
    {0xFC, "PWCTR6",  "Power control 6"},
    {0xE0, "GMCTRP1", "Gamma '+'polarity correction characteristics setting"},
    {0xE1, "GMCTRN1", "Gamma '-'polarity correction characteristics setting"},
    {0xFF, NULL,      NULL}, /* sentinel */
};

static const st7735_cmd_entry *st7735_find_cmd(uint8_t cmd)
{
    for (int i = 0; st7735_cmd_table[i].name != NULL; i++) {
        if (st7735_cmd_table[i].cmd == cmd)
            return &st7735_cmd_table[i];
    }
    return NULL;
}

static struct srd_channel st7735_channels[] = {
    {"cs",  "CS#",  "Chip-select",        0, SRD_CHANNEL_COMMON, "dec_st7735_chan_cs"},
    {"clk", "CLK",  "Clock",              1, SRD_CHANNEL_SCLK,   "dec_st7735_chan_clk"},
    {"mosi","MOSI", "Master out, slave in",2, SRD_CHANNEL_SDATA,  "dec_st7735_chan_mosi"},
    {"dc",  "DC",   "Data or command",    3, SRD_CHANNEL_COMMON, "dec_st7735_chan_dc"},
};

static const char *st7735_ann_labels[][3] = {
    {"", "bit", "Bit"},
    {"", "command", "Command"},
    {"", "data", "Data"},
    {"", "description", "Description"},
};

static const int st7735_row_bits_classes[] = {ANN_BIT, -1};
static const int st7735_row_fields_classes[] = {ANN_CMD, ANN_DATA, -1};
static const int st7735_row_description_classes[] = {ANN_DESC, -1};

static const struct srd_c_ann_row st7735_ann_rows[] = {
    {"bits", "Bits", st7735_row_bits_classes, 1},
    {"fields", "Fields", st7735_row_fields_classes, 2},
    {"description", "Description", st7735_row_description_classes, 1},
};

static const char *st7735_inputs[] = {"logic"};
static const char *st7735_tags[] = {"Display", "IC"};

static void st7735_put_desc(struct srd_decoder_inst *di, st7735_state *s)
{
    if (s->current_cmd == -1)
        return;

    const st7735_cmd_entry *entry = st7735_find_cmd((uint8_t)s->current_cmd);
    if (entry) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s: %s", entry->name, entry->desc);
        c_put(di, s->desc_ss, s->desc_es, s->out_ann, ANN_DESC, buf);
    } else {
        /* Unknown command */
        char data_str[512];
        int pos = 0;
        int n_fields = s->current_data_len;
        int truncated = 0;
        if (n_fields == ST7735_MAX_DATA_LEN) {
            n_fields = ST7735_MAX_DATA_LEN - 1;
            truncated = 1;
        }
        if (n_fields > 0) {
            for (int i = 0; i < n_fields && pos < (int)sizeof(data_str) - 8; i++)
                pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%s%02X", (i > 0) ? " " : "", s->current_data[i]);
        } else {
            snprintf(data_str, sizeof(data_str), "(none)");
        }
        char buf[1024];
        snprintf(buf, sizeof(buf), "Unknown command: %02X. Data: %s%s",
                 s->current_cmd, data_str, truncated ? "..." : "");
        c_put(di, s->desc_ss, s->desc_es, s->out_ann, ANN_DESC, buf);
    }
}

static void st7735_reset_state(st7735_state *s)
{
    s->accum_byte = 0;
    s->accum_bits_num = 0;
    s->bit_ss = (uint64_t)-1;
    s->byte_ss = (uint64_t)-1;
    s->current_bit = -1;
}

static void st7735_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(st7735_state)));
    st7735_state *s = (st7735_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(st7735_state));
    s->current_cmd = -1;
    s->bit_ss = (uint64_t)-1;
    s->byte_ss = (uint64_t)-1;
    s->current_bit = -1;
    s->out_ann = -1;
}

static void st7735_start(struct srd_decoder_inst *di)
{
    st7735_state *s = (st7735_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "st7735");
}

static void st7735_decode(struct srd_decoder_inst *di)
{
    st7735_state *s = (st7735_state *)c_decoder_get_private(di);
    while (1) {
        /* Check data on both CLK edges */
        int ret = c_wait(di, CW_E(CH_CLK), CW_END);
        if (ret != SRD_OK)
            return;

        int cs  = c_pin(di, CH_CS);
        int clk = c_pin(di, CH_CLK);
        int mosi = c_pin(di, CH_MOSI);
        int dc  = c_pin(di, CH_DC);

        if (cs == 1) {
            /* Wait for CS = low, ignore the rest */
            st7735_reset_state(s);
            continue;
        }

        if (clk == 1) {
            /* Rising edge: read one bit */
            s->bit_ss = di_samplenum(di);
            if (s->accum_bits_num == 0)
                s->byte_ss = di_samplenum(di);
            s->current_bit = mosi;
        }

        if (clk == 0 && s->current_bit >= 0) {
            /* Falling edge: process one bit */
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", s->current_bit);
            c_put(di, s->bit_ss, di_samplenum(di), s->out_ann, ANN_BIT, bit_str);

            s->accum_byte = (s->accum_byte << 1) | s->current_bit; /* MSB-first */
            s->accum_bits_num++;

            if (s->accum_bits_num == 8) {
                /* Process one byte */
                int ann = dc ? ANN_DATA : ANN_CMD; /* DC = low for commands */
                char byte_str[8];
                snprintf(byte_str, sizeof(byte_str), "%02X", s->accum_byte);
                c_put(di, s->byte_ss, di_samplenum(di), s->out_ann, ann, byte_str);

                if (ann == ANN_CMD) {
                    /* Output description of previous command */
                    st7735_put_desc(di, s);
                    s->desc_ss = s->byte_ss;
                    s->desc_es = di_samplenum(di); /* For cmds without data */
                    s->current_cmd = s->accum_byte;
                    s->current_data_len = 0;
                } else {
                    /* Data byte */
                    if (s->current_data_len < ST7735_MAX_DATA_LEN)
                        s->current_data[s->current_data_len++] = (uint8_t)s->accum_byte;
                    s->desc_es = di_samplenum(di);
                }

                s->accum_bits_num = 0;
                s->accum_byte = 0;
                s->byte_ss = (uint64_t)-1;
            }
            s->current_bit = -1;
            s->bit_ss = (uint64_t)-1;
        }
    }
}

static void st7735_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder st7735_c_decoder = {
    .id = "st7735_c",
    .name = "ST7735(C)",
    .longname = "Sitronix ST7735 (C)",
    .desc = "Sitronix ST7735 TFT controller protocol. (C implementation)",
    .license = "gplv2+",
    .channels = st7735_channels,
    .num_channels = 4,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = st7735_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = st7735_ann_rows,
    .inputs = st7735_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = st7735_tags,
    .num_tags = 2,
    .reset = st7735_reset,
    .start = st7735_start,
    .decode = st7735_decode,
    .destroy = st7735_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &st7735_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}