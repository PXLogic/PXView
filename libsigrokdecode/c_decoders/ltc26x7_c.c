/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Analog Devices Inc.
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
    ANN_SLAVE_ADDR = 0,
    ANN_COMMAND = 1,
    ANN_ADDRESS = 2,
    ANN_DAC_A_VOLTAGE = 3,
    ANN_DAC_B_VOLTAGE = 4,
    NUM_ANN,
};

enum {
    LTC_IDLE,
    LTC_GET_SLAVE_ADDR,
    LTC_GET_CMD_ADDR,
    LTC_WRITE_DATA,
};

static const char *slave_address_str[][4] = {
    {"GND", "GND", "GND", "G"},
    {"FLOAT", "FLOAT", "FLOAT", "F"},
    {"VCC", "VCC", "VCC", "V"},
};

static const char *ltc_commands[][4] = {
    {"Write Input Register", "Write In Reg", "Wr In Reg", "WIR"},
    {"Update DAC", "Update", "U", "U"},
    {"Write and Power Up DAC", "Write & Power Up", "W&PU", "W&PU"},
    {"Power Down DAC", "Power Down", "PD", "PD"},
    {"No Operation", "No Op", "NO", "NO"},
};

static const int ltc_cmd_keys[] = {0x00, 0x01, 0x03, 0x04, 0x0F};
#define NUM_LTC_COMMANDS 5

static const char *ltc_addresses[][2] = {
    {"DAC A", "A"},
    {"DAC B", "B"},
    {"All DACs", "All"},
};

static const int ltc_addr_keys[] = {0x00, 0x01, 0x0F};
#define NUM_LTC_ADDRESSES 3

typedef struct {
    int out_ann;
    int state;
    uint64_t ss;
    uint64_t es;
    int data;
    int dac_val;
    int chip;       /* 0=ltc2607, 1=ltc2617, 2=ltc2627 */
    double vref;
    int first_data_byte;
    uint64_t data_ss;
} ltc26x7_state;

static const char *ltc26x7_inputs[] = {"i2c", NULL};
static const char *ltc26x7_tags[] = {"IC", "Analog/digital", NULL};

static struct srd_decoder_option ltc26x7_options[] = {
    {"chip", "dec_ltc26x7_opt_chip", "Chip", NULL, NULL},
    {"vref", "dec_ltc26x7_opt_vref", "Reference voltage (V)", NULL, NULL},
};

static const char *ltc26x7_ann_labels[][3] = {
    {"", "slave_addr", "Slave address"},
    {"", "command", "Command"},
    {"", "address", "Address"},
    {"", "dac_a_voltage", "DAC A voltage"},
    {"", "dac_b_voltage", "DAC B voltage"},
};

static const int ltc26x7_row_addr_cmd_classes[] = {ANN_SLAVE_ADDR, ANN_COMMAND, ANN_ADDRESS, -1};
static const int ltc26x7_row_dac_a_classes[] = {ANN_DAC_A_VOLTAGE, -1};
static const int ltc26x7_row_dac_b_classes[] = {ANN_DAC_B_VOLTAGE, -1};

static const struct srd_c_ann_row ltc26x7_ann_rows[] = {
    {"addr_cmd", "Address/command", ltc26x7_row_addr_cmd_classes, 3},
    {"dac_a_voltages", "DAC A voltages", ltc26x7_row_dac_a_classes, 1},
    {"dac_b_voltages", "DAC B voltages", ltc26x7_row_dac_b_classes, 1},
};

static void convert_ternary(int n, int result[3])
{
    result[0] = result[1] = result[2] = 0;
    if (n < 0) return;
    for (int i = 2; i >= 0; i--) {
        result[i] = n % 3;
        n /= 3;
    }
}

static void ltc26x7_handle_slave_addr(struct srd_decoder_inst *di,
    ltc26x7_state *s, uint8_t data)
{
    if (data == 0x73) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_SLAVE_ADDR,
                  "Global address", "Global addr", "Glob addr", "GA");
        return;
    }

    /* Extract CA2/CA1/CA0 bits */
    int addr = 0;
    for (int i = 0; i < 7; i++) {
        if (i == 2 || i == 3)
            continue;
        int offset = i;
        if (i > 3)
            offset -= 2;
        if (data & (1 << i))
            addr |= (1 << offset);
    }
    addr -= 0x04;

    int ternary[3];
    convert_ternary(addr, ternary);

    char buf[256];
    snprintf(buf, sizeof(buf), "CA2=%s CA1=%s CA0=%s",
             slave_address_str[ternary[0]][0],
             slave_address_str[ternary[1]][0],
             slave_address_str[ternary[2]][0]);
    char buf2[64];
    snprintf(buf2, sizeof(buf2), "2=%s 1=%s 0=%s",
             slave_address_str[ternary[0]][1],
             slave_address_str[ternary[1]][1],
             slave_address_str[ternary[2]][1]);
    char buf3[32];
    snprintf(buf3, sizeof(buf3), "%s %s %s",
             slave_address_str[ternary[0]][3],
             slave_address_str[ternary[1]][3],
             slave_address_str[ternary[2]][3]);
    c_put(di, s->ss, s->es, s->out_ann, ANN_SLAVE_ADDR, buf, buf2, buf3);
}

static void ltc26x7_handle_cmd_addr(struct srd_decoder_inst *di,
    ltc26x7_state *s, uint8_t databyte)
{
    int cmd_val = (databyte >> 4) & 0x0F;
    s->dac_val = databyte & 0x0F;
    uint64_t sm = (s->ss + s->es) / 2;

    /* Find command string */
    const char *cmd_str[4] = {"Unknown", "Unk", "U", "?"};
    for (int i = 0; i < NUM_LTC_COMMANDS; i++) {
        if (ltc_cmd_keys[i] == cmd_val) {
            for (int j = 0; j < 4; j++)
                cmd_str[j] = ltc_commands[i][j];
            break;
        }
    }
    c_put(di, s->ss, sm, s->out_ann, ANN_COMMAND,
              cmd_str[0], cmd_str[1], cmd_str[2], cmd_str[3]);

    /* Find address string */
    const char *addr_str[2] = {"Unknown", "?"};
    for (int i = 0; i < NUM_LTC_ADDRESSES; i++) {
        if (ltc_addr_keys[i] == s->dac_val) {
            addr_str[0] = ltc_addresses[i][0];
            addr_str[1] = ltc_addresses[i][1];
            break;
        }
    }
    c_put(di, sm, s->es, s->out_ann, ANN_ADDRESS,
              addr_str[0], addr_str[1]);
}

static void ltc26x7_handle_data(struct srd_decoder_inst *di,
    ltc26x7_state *s, uint8_t databyte)
{
    s->data = (s->data << 8) & 0xFF00;
    s->data += databyte;

    double voltage;
    int data_val = s->data;
    switch (s->chip) {
    case 1: /* ltc2617 (14-bit) */
        data_val >>= 2;
        voltage = s->vref * data_val / 0x3FFF;
        break;
    case 2: /* ltc2627 (12-bit) */
        data_val >>= 4;
        voltage = s->vref * data_val / 0x0FFF;
        break;
    default: /* ltc2607 (16-bit) */
        voltage = s->vref * data_val / 0xFFFF;
        break;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "%.6fV", voltage);
    char buf2[32];
    snprintf(buf2, sizeof(buf2), "%.2fV", voltage);

    s->data = 0;
    if (s->dac_val == 0x0F) {
        /* All DACs */
        c_put(di, s->data_ss, s->es, s->out_ann, ANN_DAC_A_VOLTAGE, buf, buf2);
        c_put(di, s->data_ss, s->es, s->out_ann, ANN_DAC_B_VOLTAGE, buf, buf2);
    } else {
        c_put(di, s->data_ss, s->es, s->out_ann, ANN_DAC_A_VOLTAGE + s->dac_val, buf, buf2);
    }
}

static void ltc26x7_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ltc26x7_state *s = (ltc26x7_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->es = end_sample;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    /* State machine */
    if (s->state == LTC_IDLE) {
        if (strcmp(cmd, "START") != 0)
            return;
        s->state = LTC_GET_SLAVE_ADDR;
    } else if (s->state == LTC_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS WRITE") != 0)
            return;
        s->ss = start_sample;
        ltc26x7_handle_slave_addr(di, s, databyte);
        s->ss = (uint64_t)-1;
        s->state = LTC_GET_CMD_ADDR;
    } else if (s->state == LTC_GET_CMD_ADDR) {
        if (strcmp(cmd, "DATA WRITE") != 0)
            return;
        s->ss = start_sample;
        ltc26x7_handle_cmd_addr(di, s, databyte);
        s->ss = (uint64_t)-1;
        s->state = LTC_WRITE_DATA;
    } else if (s->state == LTC_WRITE_DATA) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            if (s->ss == (uint64_t)-1) {
                s->ss = start_sample;
                s->data_ss = start_sample;
                s->data = databyte;
                return;
            }
            ltc26x7_handle_data(di, s, databyte);
            s->ss = (uint64_t)-1;
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = LTC_IDLE;
        } else {
            return;
        }
    }
}

static void ltc26x7_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ltc26x7_state)));
    }
    ltc26x7_state *s = (ltc26x7_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ltc26x7_state));
    s->state = LTC_IDLE;
    s->ss = (uint64_t)-1;
    s->vref = 1.5;
}

static void ltc26x7_start(struct srd_decoder_inst *di)
{
    ltc26x7_state *s = (ltc26x7_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ltc26x7");

    const char *chip = c_opt_str(di, "chip", "ltc2607");
    if (chip && strcmp(chip, "ltc2617") == 0)
        s->chip = 1;
    else if (chip && strcmp(chip, "ltc2627") == 0)
        s->chip = 2;
    else
        s->chip = 0;

    s->vref = c_opt_dbl(di, "vref", 1.5);
}

static void ltc26x7_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ltc26x7_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ltc26x7_c_decoder = {
    .id = "ltc26x7_c",
    .name = "LTC26x7(C)",
    .longname = "Linear Technology LTC26x7 (C)",
    .desc = "Linear Technology LTC26x7 16-/14-/12-bit rail-to-rail DACs. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ltc26x7_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = ltc26x7_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ltc26x7_ann_rows,
    .inputs = ltc26x7_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ltc26x7_tags,
    .num_tags = 2,
    .reset = ltc26x7_reset,
    .start = ltc26x7_start,
    .decode = ltc26x7_decode,
    .destroy = ltc26x7_destroy,
    .decode_upper = ltc26x7_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ltc26x7_options[0].def = g_variant_new_string("ltc2607");
    GSList *chip_vals = NULL;
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("ltc2607"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("ltc2617"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("ltc2627"));
    ltc26x7_options[0].values = chip_vals;

    ltc26x7_options[1].def = g_variant_new_double(1.5);

    return &ltc26x7_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}