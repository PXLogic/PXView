/*
 * This file is part of the libsigrokdecode project.
 *
 * ST7789 TFT controller protocol decoder (C implementation).
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

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_BIT = 0,
    ANN_CMD,
    ANN_DATA,
    ANN_CMD_DATA,
    ANN_ASSERTED,
    NUM_ANN,
};

#define CH_CSX 0
#define CH_DCX 1
#define CH_SDO 2
#define CH_WRX 3

#define MAX_CMD_DATA 256

/* Command table entry */
typedef struct {
    uint8_t cmd;
    const char* name;
    const char* desc;
} st7789_cmd_entry;

static const st7789_cmd_entry st7789_cmd_table[] = {
    { 0x00, "NOP", "Empty command" },
    { 0x01, "SWRESET", "Software Reset" },
    { 0x04, "RDDID", "Read Display ID" },
    { 0x09, "RDDST", "Read Display Status" },
    { 0x0A, "RDDPM", "Read Display Power Mode" },
    { 0x0B, "RDDMADCTL", "Read Display MADCTL" },
    { 0x0C, "RDDCOLMOD", "Read Display Pixel Format" },
    { 0x0D, "RDDIM", "Read Display Image Mode" },
    { 0x0E, "RDDSM", "Read Display Signal Mode" },
    { 0x0F, "RDDSDR", "Read Display Self-Diagnostic Result" },
    { 0x10, "SLPIN", "Sleep in" },
    { 0x11, "SLPOUT", "Sleep Out" },
    { 0x12, "PTLON", "Partial Display Mode On" },
    { 0x13, "NORON", "Normal Display Mode On" },
    { 0x20, "INVOFF", "Display Inversion Off" },
    { 0x21, "INVON", "Display Inversion On" },
    { 0x26, "GAMSET", "Gamma Set" },
    { 0x28, "DISPOFF", "Display Off" },
    { 0x29, "DISPON", "Display On" },
    { 0x2A, "CASET", "Column Address Set" },
    { 0x2B, "RASET", "Row Address Set" },
    { 0x2C, "RAMWR", "Memory Write" },
    { 0x2E, "RAMRD", "Memory Read" },
    { 0x30, "PTLAR", "Partial Area" },
    { 0x33, "VSCRDEF", "Vertical Scrolling Definition" },
    { 0x34, "TEOFF", "Tearing Effect Line OFF" },
    { 0x35, "TEON", "Tearing Effect Line On" },
    { 0x36, "MADCTL", "Memory Data Access Control" },
    { 0x37, "VSCSAD", "Vertical Scroll Start Address of RAM" },
    { 0x38, "IDMOFF", "Idle Mode Off" },
    { 0x39, "IDMON", "Idle mode on" },
    { 0x3A, "COLMOD", "Interface Pixel Format" },
    { 0x3C, "WRMEMC", "Write Memory Continue" },
    { 0x3E, "RDMEMC", "Read Memory Continue" },
    { 0x44, "STE", "Set Tear Scanline" },
    { 0x45, "GSCAN", "Get Scanline" },
    { 0x51, "WRDISBV", "Write Display Brightness" },
    { 0x52, "RDDISBV", "Read Display Brightness Value" },
    { 0x53, "WRCTRLD", "Write CTRL Display" },
    { 0x54, "RDCTRLD", "Read CTRL Value Display" },
    { 0x55, "WRCACE", "Write Content Adaptive Brightness Control and Color Enhancement" },
    { 0x56, "RDCABC", "Read Content Adaptive Brightness Control" },
    { 0x5E, "WRCABCMB", "Write CABC Minimum Brightness" },
    { 0x5F, "RDCABCMB", "Read CABC Minimum Brightness" },
    { 0x68, "RDABCSDR", "Read Automatic Brightness Control Self-Diagnostic Result" },
    { 0xDA, "RDID1", "Read ID1" },
    { 0xDB, "RDID2", "Read ID2" },
    { 0xDC, "RDID3", "Read ID3" },
    { 0xB0, "RAMCTRL", "RAM Control" },
    { 0xB1, "RGBCTRL", "RGB Interface Control" },
    { 0xB2, "PORCTRL", "Porch Setting" },
    { 0xB3, "FRCTRL1", "Frame Rate Control 1 (In partial mode/ idle colors)" },
    { 0xB5, "PARCTRL", "Partial mode Control" },
    { 0xB7, "GCTRL", "Gate Control" },
    { 0xB8, "GTADJ", "Gate On Timing Adjustment" },
    { 0xBA, "DGMEN", "Digital Gamma Enable" },
    { 0xBB, "VCOMS", "VCOMS Setting" },
    { 0xC0, "LCMCTRL", "LCM Control" },
    { 0xC1, "IDSET", "ID Code Setting" },
    { 0xC2, "VDVVRHEN", "VDV and VRH Command Enable" },
    { 0xC3, "VRHS", "VRH Set" },
    { 0xC4, "VDVS", "VDV Set" },
    { 0xC5, "VCMOFSET", "VCOMS Offset Set" },
    { 0xC6, "FRCTRL2", "Frame Rate Control in Normal Mode" },
    { 0xC7, "CABCCTRL", "CABC Control" },
    { 0xC8, "REGSEL1", "Register Value Selection 1" },
    { 0xCA, "REGSEL2", "Register Value Selection 2" },
    { 0xCC, "PWMFRSEL", "PWM Frequency Selection" },
    { 0xD0, "PWCTRL1", "Power Control 1" },
    { 0xD2, "VAPVANEN", "Enable VAP/VAN signal output" },
    { 0xDF, "CMD2EN", "Command 2 Enable" },
    { 0xE0, "PVGAMCTRL", "Positive Voltage Gamma Control" },
    { 0xE1, "NVGAMCTRL", "Negative Voltage Gamma Control" },
    { 0xE2, "DGMLUTR", "Digital Gamma Look-up Table for Red" },
    { 0xE3, "DGMLUTB", "Digital Gamma Look-up Table for Blue" },
    { 0xE4, "GATECTRL", "Gate Control" },
    { 0xE7, "SPI2EN", "SPI2 Enable" },
    { 0xE8, "PWCTRL2", "Power Control 2" },
    { 0xE9, "EQCTRL", "Equalize time control" },
    { 0xEC, "PROMCTRL", "Program Mode Control" },
    { 0xFA, "PROMEN", "Program Mode Enable" },
    { 0xFC, "NVMSET", "NVM Setting" },
    { 0xFE, "PROMACT", "Program action" },
    { 0xFF, NULL, NULL }, /* sentinel */
};

static void st7789_get_cmd_str(uint8_t cmd, char* buf, int buf_size)
{
    for (int i = 0; st7789_cmd_table[i].name != NULL; i++) {
        if (st7789_cmd_table[i].cmd == cmd) {
            snprintf(buf, buf_size, "%s(%02X)", st7789_cmd_table[i].name, cmd);
            return;
        }
    }
    snprintf(buf, buf_size, "Unknown(%02X)", cmd);
}

typedef struct {
    int bit; /* current sampled bit value, -1 = not sampled */
    int bit_count; /* number of bits sampled for current byte */
    uint8_t byte_val; /* accumulated byte value */
    uint64_t bit_start_samplenum;
    uint64_t byte_sample_startnum;

    int last_cmd; /* last command byte value, -1 = none */
    uint64_t last_cmd_data_ss;
    uint64_t last_cmd_data_es;
    uint8_t last_cmd_data[MAX_CMD_DATA];
    int last_cmd_data_len;

    uint64_t csx_start_samplenum;

    int out_ann;
} st7789_state;

static struct srd_channel st7789_channels[] = {
    { "csx", "CSX", "Chip selection signal", 0, SRD_CHANNEL_COMMON, "dec_st7789_chan_csx" },
    { "dcx", "DCX", "Clock signal", 1, SRD_CHANNEL_COMMON, "dec_st7789_chan_dcx" },
    { "sdo", "SDO", "Serial output data", 2, SRD_CHANNEL_SDATA, "dec_st7789_chan_sdo" },
    { "wrx", "WRX", "Command / data", 3, SRD_CHANNEL_COMMON, "dec_st7789_chan_wrx" },
};

static const char* st7789_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "command", "Command" },
    { "", "data", "Data" },
    { "", "cmd_data", "Command + Data" },
    { "", "asserted", "Assertion" },
};

static const int st7789_row_bits_classes[] = { ANN_BIT, -1 };
static const int st7789_row_bytes_classes[] = { ANN_CMD, ANN_DATA, -1 };
static const int st7789_row_cmd_data_classes[] = { ANN_CMD_DATA, -1 };
static const int st7789_row_asserted_classes[] = { ANN_ASSERTED, -1 };

static const struct srd_c_ann_row st7789_ann_rows[] = {
    { "bits", "Bits", st7789_row_bits_classes, 1 },
    { "bytes", "Bytes", st7789_row_bytes_classes, 2 },
    { "cmd_data", "Command + Data", st7789_row_cmd_data_classes, 1 },
    { "asserted", "Assertion", st7789_row_asserted_classes, 1 },
};

static const char* st7789_inputs[] = { "logic" };
static const char* st7789_tags[] = { "Display", "SPI" };

static void st7789_output_cmd_data(struct srd_decoder_inst* di, st7789_state* s)
{
    if (s->last_cmd < 0)
        return;

    char cmd_str[64];
    st7789_get_cmd_str((uint8_t)s->last_cmd, cmd_str, sizeof(cmd_str));

    if (s->last_cmd_data_len > 0) {
        char buf[1024];
        int pos = snprintf(buf, sizeof(buf), "%s: ", cmd_str);
        for (int i = 0; i < s->last_cmd_data_len && pos < (int)sizeof(buf) - 8; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", s->last_cmd_data[i]);
        c_put(di, s->last_cmd_data_ss, s->last_cmd_data_es, s->out_ann, ANN_CMD_DATA, buf);
    } else {
        c_put(di, s->last_cmd_data_ss, s->last_cmd_data_es, s->out_ann, ANN_CMD_DATA, cmd_str);
    }

    s->last_cmd = -1;
    s->last_cmd_data_len = 0;
}

static void st7789_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(st7789_state)));
    st7789_state* s = (st7789_state*)c_decoder_get_private(di);
    memset(s, 0, sizeof(st7789_state));
    s->bit = -1;
    s->last_cmd = -1;
    s->out_ann = -1;
}

static void st7789_start(struct srd_decoder_inst* di)
{
    st7789_state* s = (st7789_state*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "st7789");
}

static void st7789_decode(struct srd_decoder_inst* di)
{
    st7789_state* s = (st7789_state*)c_decoder_get_private(di);
    while (1) {
        /* Outer loop: wait for CSX falling edge */
        int ret = c_wait(di, CW_F(CH_CSX), CW_END);
        if (ret != SRD_OK)
            return;

        s->csx_start_samplenum = di_samplenum(di);
        s->bit = -1;
        s->bit_count = 0;
        s->byte_val = 0;
        s->byte_sample_startnum = 0;

        while (1) {
            /* Inner loop: wait for CSX rising edge or DCX edge */
            ret = c_wait(di, CW_R(CH_CSX), CW_OR, CW_E(CH_DCX), CW_END);
            if (ret != SRD_OK)
                return;

            int csx = c_pin(di, CH_CSX);
            int dcx = c_pin(di, CH_DCX);
            int sdo = c_pin(di, CH_SDO);
            int wrx = c_pin(di, CH_WRX);

            if (csx == 1) {
                /* CSX released: output Asserted and cmd_data */
                c_put(di, s->csx_start_samplenum, di_samplenum(di), s->out_ann, ANN_ASSERTED, "Asserted");

                if (s->last_cmd >= 0) {
                    st7789_output_cmd_data(di, s);
                }
                break;
            }

            if (dcx == 1 && s->bit == -1) {
                /* Sample SDO bit (data bit start) */
                s->bit = sdo;
                s->bit_start_samplenum = di_samplenum(di);
                s->bit_count++;
                s->byte_val = (s->byte_val << 1) | s->bit;
                if (s->byte_sample_startnum == 0)
                    s->byte_sample_startnum = di_samplenum(di);
            }

            if (dcx == 0 && s->bit != -1) {
                /* Complete one bit */
                char bit_str[16];
                snprintf(bit_str, sizeof(bit_str), "%d", s->bit);
                c_put(di, s->bit_start_samplenum, di_samplenum(di), s->out_ann, ANN_BIT, bit_str);
                s->bit = -1;

                if (s->bit_count == 8) {
                    if (wrx) {
                        /* Data byte */
                        s->last_cmd_data_es = di_samplenum(di);
                        if (s->last_cmd_data_len < MAX_CMD_DATA)
                            s->last_cmd_data[s->last_cmd_data_len++] = s->byte_val;

                        char buf[32];
                        snprintf(buf, sizeof(buf), "Data(%02X)", s->byte_val);
                        c_put(di, s->byte_sample_startnum, di_samplenum(di), s->out_ann, ANN_DATA, buf);
                    } else {
                        /* Command byte */
                        char cmd_buf[64];
                        st7789_get_cmd_str(s->byte_val, cmd_buf, sizeof(cmd_buf));
                        c_put(di, s->byte_sample_startnum, di_samplenum(di), s->out_ann, ANN_CMD, cmd_buf);

                        /* Output previous cmd_data combination */
                        if (s->last_cmd >= 0) {
                            st7789_output_cmd_data(di, s);
                        }

                        s->last_cmd = s->byte_val;
                        s->last_cmd_data_ss = s->byte_sample_startnum;
                        s->last_cmd_data_es = di_samplenum(di);
                        s->last_cmd_data_len = 0;
                    }

                    s->byte_val = 0;
                    s->bit_count = 0;
                    s->byte_sample_startnum = 0;
                }
            }
        }
    }
}

static void st7789_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder st7789_c_decoder = {
    .id = "st7789_c",
    .name = "ST7789(C)",
    .longname = "Sitronix ST7789 (C)",
    .desc = "Sitronix ST7789 TFT controller protocol. (C implementation)",
    .license = "gplv2+",
    .channels = st7789_channels,
    .num_channels = 4,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = st7789_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = st7789_ann_rows,
    .inputs = st7789_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = st7789_tags,
    .num_tags = 2,
    .reset = st7789_reset,
    .start = st7789_start,
    .decode = st7789_decode,
    .destroy = st7789_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &st7789_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}