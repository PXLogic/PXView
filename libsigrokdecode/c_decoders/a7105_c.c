/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Richard Li <richard.li@ces.hk>
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
    ANN_CMD = 0,
    ANN_TX_DATA,
    ANN_RX_DATA,
    ANN_WARN,
    NUM_ANN,
};

enum a7105_state_val {
    A7105_IDLE,
    A7105_CMD_RECEIVED,
};

static const struct {
    const char *name;
    int size;
} a7105_regs[] = {
    [0x00] = {"MODE",           1},
    [0x01] = {"MODE_CTRL",      1},
    [0x02] = {"CALC",           1},
    [0x03] = {"FIFO_I",         1},
    [0x04] = {"FIFO_II",        1},
    [0x05] = {"FIFO_DATA",      1},
    [0x06] = {"ID_DATA",        1},
    [0x07] = {"RC_OSC_I",       1},
    [0x08] = {"RC_OSC_II",      1},
    [0x09] = {"RC_OSC_III",     1},
    [0x0A] = {"CKO_PIN",        1},
    [0x0B] = {"GPIO1_PIN_I",    1},
    [0x0C] = {"GPIO2_PIN_II",   1},
    [0x0D] = {"CLOCK",          1},
    [0x0E] = {"DATA_RATE",      1},
    [0x0F] = {"PLL_I",          1},
    [0x10] = {"PLL_II",         1},
    [0x11] = {"PLL_III",        1},
    [0x12] = {"PLL_IV",         1},
    [0x13] = {"PLL_V",          1},
    [0x14] = {"TX_I",           1},
    [0x15] = {"TX_II",          1},
    [0x16] = {"DELAY_I",        1},
    [0x17] = {"DELAY_II",       1},
    [0x18] = {"RX",             1},
    [0x19] = {"RX_GAIN_I",      1},
    [0x1A] = {"RX_GAIN_II",     1},
    [0x1B] = {"RX_GAIN_III",    1},
    [0x1C] = {"RX_GAIN_IV",     1},
    [0x1D] = {"RSSI_THRES",     1},
    [0x1E] = {"ADC",            1},
    [0x1F] = {"CODE_I",         1},
    [0x20] = {"CODE_II",        1},
    [0x21] = {"CODE_III",       1},
    [0x22] = {"IF_CAL_I",       1},
    [0x23] = {"IF_CAL_II",      1},
    [0x24] = {"VCO_CURR_CAL",   1},
    [0x25] = {"VCO_SB_CALC_I",  1},
    [0x26] = {"VCO_SB_CALC_II", 1},
    [0x27] = {"BATT_DETECT",    1},
    [0x28] = {"TX_TEST",        1},
    [0x29] = {"RX_DEM_TEST_I",  1},
    [0x2A] = {"RX_DEM_TEST_II", 1},
    [0x2B] = {"CPC",            1},
    [0x2C] = {"CRYSTAL_TEST",   1},
    [0x2D] = {"PLL_TEST",       1},
    [0x2E] = {"VCO_TEST_I",     1},
    [0x2F] = {"VCO_TEST_II",    1},
    [0x30] = {"IFAT",           1},
    [0x31] = {"RSCALE",         1},
    [0x32] = {"FILTER_TEST",    1},
    [0x33] = {"UNKNOWN",        1},
};

#define A7105_NUM_REGS 0x34

typedef struct {
    enum a7105_state_val state;
    int first;
    int cs_was_released;
    int requirements_met;

    char cmd_name[32];
    int cmd_reg;
    int cmd_min;
    int cmd_max;

    uint8_t mosi_bytes[32];
    uint8_t miso_bytes[32];
    int mb_count;
    uint64_t mb_ss;
    uint64_t mb_es;

    
    int out_ann;
    uint64_t ss;
    uint64_t es;
} a7105_state;

static const char *a7105_inputs[] = {"spi", NULL};
static const char *a7105_tags[] = {"IC", "Wireless/RF", NULL};

static const char *a7105_ann_labels[][3] = {
    {"", "cmd", "Commands sent to the device"},
    {"", "tx-data", "Payload sent to the device"},
    {"", "rx-data", "Payload read from the device"},
    {"", "warning", "Warnings"},
};

static const int a7105_row_commands_classes[] = {ANN_CMD, ANN_TX_DATA, ANN_RX_DATA};
static const int a7105_row_warnings_classes[] = {ANN_WARN};
static const struct srd_c_ann_row a7105_ann_rows[] = {
    {"commands", "Commands", a7105_row_commands_classes, 3},
    {"warnings", "Warnings", a7105_row_warnings_classes, 1},
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

static void a7105_reset_cmd(a7105_state *s)
{
    s->first = 1;
    s->cmd_name[0] = '\0';
    s->cmd_reg = -1;
    s->cmd_min = 0;
    s->cmd_max = 0;
    s->mb_count = 0;
    s->mb_ss = (uint64_t)-1;
    s->mb_es = (uint64_t)-1;
}

static int a7105_parse_command(uint8_t b, char *cmd_name, int *cmd_reg,
    int *cmd_min, int *cmd_max)
{
    if (b == 0x05) {
        strcpy(cmd_name, "W_TX_FIFO");
        *cmd_reg = -1;
        *cmd_min = 1; *cmd_max = 32;
        return 1;
    } else if (b == 0x45) {
        strcpy(cmd_name, "R_RX_FIFO");
        *cmd_reg = -1;
        *cmd_min = 1; *cmd_max = 32;
        return 1;
    } else if (b == 0x06) {
        strcpy(cmd_name, "W_ID");
        *cmd_reg = -1;
        *cmd_min = 1; *cmd_max = 4;
        return 1;
    } else if (b == 0x46) {
        strcpy(cmd_name, "R_ID");
        *cmd_reg = -1;
        *cmd_min = 1; *cmd_max = 4;
        return 1;
    } else if ((b & 0x80) == 0) {
        if ((b & 0x40) == 0) {
            strcpy(cmd_name, "W_REGISTER");
        } else {
            strcpy(cmd_name, "R_REGISTER");
        }
        *cmd_reg = b & 0x3F;
        *cmd_min = 1; *cmd_max = 1;
        return 1;
    } else {
        int cmd = b & 0xF0;
        if (cmd == 0x80) {
            strcpy(cmd_name, "SLEEP_MODE");
        } else if (cmd == 0x90) {
            strcpy(cmd_name, "IDLE_MODE");
        } else if (cmd == 0xA0) {
            strcpy(cmd_name, "STANDBY_MODE");
        } else if (cmd == 0xB0) {
            strcpy(cmd_name, "PLL_MODE");
        } else if (cmd == 0xC0) {
            strcpy(cmd_name, "RX_MODE");
        } else if (cmd == 0xD0) {
            strcpy(cmd_name, "TX_MODE");
        } else if (cmd == 0xE0) {
            strcpy(cmd_name, "FIFO_WRITE_PTR_RESET");
        } else if (cmd == 0xF0) {
            strcpy(cmd_name, "FIFO_READ_PTR_RESET");
        } else {
            return 0;
        }
        *cmd_reg = -1;
        *cmd_min = 0; *cmd_max = 0;
        return 1;
    }
}

static void a7105_decode_mb_data(struct srd_decoder_inst *di, a7105_state *s,
    int ann, const uint8_t *data_bytes, int count, const char *label)
{
    char hex_buf[96];
    int hpos = 0;
    /* Reversed: multi byte register come LSByte first in Python,
       but we collected in order, so reverse for display */
    for (int i = count - 1; i >= 0 && hpos < (int)sizeof(hex_buf) - 4; i--)
        hpos += snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%02X", data_bytes[i]);

    char buf[160];
    snprintf(buf, sizeof(buf), "%s = \"$\"", label);
    c_put(di, s->mb_ss, s->mb_es, s->out_ann, ann, buf);

    char at_buf[100];
    snprintf(at_buf, sizeof(at_buf), "@%s", hex_buf);
    c_put(di, s->mb_ss, s->mb_es, s->out_ann, ann, at_buf);
}

static void a7105_finish_command(struct srd_decoder_inst *di, a7105_state *s)
{
    if (strcmp(s->cmd_name, "R_REGISTER") == 0) {
        if (s->cmd_reg >= 0 && s->cmd_reg < A7105_NUM_REGS) {
            char label[64];
            snprintf(label, sizeof(label), "Cmd %s: %s", s->cmd_name,
                     a7105_regs[s->cmd_reg].name);
            a7105_decode_mb_data(di, s, ANN_CMD, s->miso_bytes, s->mb_count, label);
        }
    } else if (strcmp(s->cmd_name, "W_REGISTER") == 0) {
        if (s->cmd_reg >= 0 && s->cmd_reg < A7105_NUM_REGS) {
            char label[64];
            snprintf(label, sizeof(label), "Cmd %s: %s", s->cmd_name,
                     a7105_regs[s->cmd_reg].name);
            a7105_decode_mb_data(di, s, ANN_CMD, s->mosi_bytes, s->mb_count, label);
        }
    } else if (strcmp(s->cmd_name, "R_RX_FIFO") == 0) {
        a7105_decode_mb_data(di, s, ANN_RX_DATA, s->miso_bytes, s->mb_count, "RX FIFO");
    } else if (strcmp(s->cmd_name, "W_TX_FIFO") == 0) {
        a7105_decode_mb_data(di, s, ANN_TX_DATA, s->mosi_bytes, s->mb_count, "TX FIFO");
    } else if (strcmp(s->cmd_name, "R_ID") == 0) {
        a7105_decode_mb_data(di, s, ANN_RX_DATA, s->miso_bytes, s->mb_count, "R ID");
    } else if (strcmp(s->cmd_name, "W_ID") == 0) {
        a7105_decode_mb_data(di, s, ANN_TX_DATA, s->mosi_bytes, s->mb_count, "W ID");
    }
}

static void a7105_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    a7105_state *s = (a7105_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "TRANSFER") == 0) {
        if (s->state == A7105_CMD_RECEIVED && s->mb_count >= s->cmd_min) {
            a7105_finish_command(di, s);
        } else if (s->state == A7105_CMD_RECEIVED && s->mb_count < s->cmd_min && s->mb_count > 0) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "missing data bytes");
        }
        a7105_reset_cmd(s);
        s->cs_was_released = 1;
    } else if (strcmp(cmd, "CS-CHANGE") == 0) {
        uint8_t cs_old = (n_fields > 0) ? fields[0].u8 : 0xFF;
        uint8_t cs_new = (n_fields > 1) ? fields[1].u8 : 0;

        if (cs_old == 0 && cs_new == 1) {
            if (s->state == A7105_CMD_RECEIVED && s->mb_count >= s->cmd_min) {
                a7105_finish_command(di, s);
            } else if (s->state == A7105_CMD_RECEIVED && s->mb_count < s->cmd_min && s->mb_count > 0) {
                c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "missing data bytes");
            }
            a7105_reset_cmd(s);
            s->cs_was_released = 1;
        }
    } else if (strcmp(cmd, "DATA") == 0 && s->cs_was_released) {
        int have_mosi, have_miso;
        uint64_t mosi_val, miso_val;
        parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi_val, &miso_val);

        if (!have_mosi && !have_miso) {
            s->requirements_met = 0;
            return;
        }

        if (s->first) {
            s->first = 0;
            uint8_t cmd_byte = (uint8_t)mosi_val;
            if (!a7105_parse_command(cmd_byte, s->cmd_name, &s->cmd_reg,
                    &s->cmd_min, &s->cmd_max)) {
                c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "unknown command");
                return;
            }
            s->state = A7105_CMD_RECEIVED;

            if (strcmp(s->cmd_name, "W_REGISTER") == 0 ||
                strcmp(s->cmd_name, "R_REGISTER") == 0) {
                s->mb_ss = start_sample;
            } else {
                char buf[64];
                snprintf(buf, sizeof(buf), "Cmd %s", s->cmd_name);
                c_put(di, s->ss, s->es, s->out_ann, ANN_CMD, buf);
            }
        } else {
            if (s->mb_count < s->cmd_max) {
                if (s->mb_count == 0) s->mb_ss = start_sample;
                s->mb_es = end_sample;
                s->mosi_bytes[s->mb_count] = (uint8_t)mosi_val;
                s->miso_bytes[s->mb_count] = (uint8_t)miso_val;
                s->mb_count++;
            } else {
                c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "excess byte");
            }
        }
    }
}

static void a7105_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(a7105_state)));
    }
    a7105_state *s = (a7105_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(a7105_state));
    a7105_reset_cmd(s);
    s->requirements_met = 1;
    s->cs_was_released = 0;
}

static void a7105_start(struct srd_decoder_inst *di)
{
    a7105_state *s = (a7105_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "a7105");
}

static void a7105_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void a7105_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder a7105_c_decoder = {
    .id = "a7105_c",
    .name = "A7105(C)",
    .longname = "AMICCOM A7105 (C)",
    .desc = "2.4GHz FSK/GFSK Transceiver with 2K ~ 500Kbps data rate. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = a7105_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = a7105_ann_rows,
    .inputs = a7105_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = a7105_tags,
    .num_tags = 2,
    .reset = a7105_reset,
    .start = a7105_start,
    .decode = a7105_decode,
    .destroy = a7105_destroy,
    .decode_upper = a7105_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &a7105_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}