/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
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
    ANN_PE = 0,
    ANN_RSB0,
    ANN_RSB1,
    ANN_RSB2,
    ANN_CE,
    ANN_RFB,
    ANN_RHFB,
    ANN_REFB,
    ANN_RLB,
    ANN_REEM,
    ANN_RP,
    ANN_LPMP,
    ANN_WP,
    ANN_WARN,
    ANN_DEV,
    NUM_ANN,
};

#define VENDOR_CODE_ATMEL 0x1e

typedef struct {
    uint8_t mosi_bytes[4];
    uint8_t miso_bytes[4];
    int byte_count;
    uint64_t ss_cmd, es_cmd;
    uint64_t ss_device;
    uint8_t xx, yy, zz, mm;
    uint8_t vendor_code;
    uint8_t part_fam_flash_size;
    uint8_t part_number;
    int out_ann;
} avr_isp_state;

static const struct { uint8_t fam; uint8_t part; const char *name; } avr_devices[] = {
    {0x90, 0x01, "AT90S1200"},
    {0x91, 0x01, "AT90S2313"},
    {0x92, 0x01, "AT90S4414"},
    {0x92, 0x05, "ATmega48"},
    {0x93, 0x01, "AT90S8515"},
    {0x93, 0x0A, "ATmega88"},
    {0x94, 0x06, "ATmega168"},
    {0xFF, 0xFF, "Device code erased, or target missing"},
    {0x01, 0x02, "Device locked"},
    {0, 0, NULL}
};

static const char *avr_isp_inputs[] = {"spi", NULL};
static const char *avr_isp_tags[] = {"Debug/trace", NULL};

static const char *avr_isp_ann_labels[][3] = {
    {"", "pe", "Programming enable"},
    {"", "rsb0", "Read signature byte 0"},
    {"", "rsb1", "Read signature byte 1"},
    {"", "rsb2", "Read signature byte 2"},
    {"", "ce", "Chip erase"},
    {"", "rfb", "Read fuse bits"},
    {"", "rhfb", "Read high fuse bits"},
    {"", "refb", "Read extended fuse bits"},
    {"", "rlb", "Read lock bits"},
    {"", "reem", "Read EEPROM memory"},
    {"", "rp", "Read program memory"},
    {"", "lpmp", "Load program memory page"},
    {"", "wp", "Write program memory"},
    {"", "warning", "Warning"},
    {"", "dev", "Device"},
};

static const int avr_isp_row_commands_classes[] = {
    ANN_PE, ANN_RSB0, ANN_RSB1, ANN_RSB2, ANN_CE,
    ANN_RFB, ANN_RHFB, ANN_REFB, ANN_RLB, ANN_REEM,
    ANN_RP, ANN_LPMP, ANN_WP, -1
};
static const int avr_isp_row_warnings_classes[] = {ANN_WARN, -1};
static const int avr_isp_row_devs_classes[] = {ANN_DEV, -1};

static const struct srd_c_ann_row avr_isp_ann_rows[] = {
    {"commands", "Commands", avr_isp_row_commands_classes, 13},
    {"warnings", "Warnings", avr_isp_row_warnings_classes, 1},
    {"devs", "Devices", avr_isp_row_devs_classes, 1},
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

static const char *vendor_code_name(uint8_t code)
{
    if (code == 0x1E) return "Atmel";
    if (code == 0x00) return "Device locked";
    return "Unknown";
}

static const char *part_name(uint8_t fam, uint8_t part)
{
    for (int i = 0; avr_devices[i].name; i++) {
        if (avr_devices[i].fam == fam && avr_devices[i].part == part)
            return avr_devices[i].name;
    }
    return "Unknown";
}

static void avr_isp_handle_command(struct srd_decoder_inst *di, avr_isp_state *s,
    const uint8_t *cmd, const uint8_t *ret)
{
    char buf[512];

    /* Programming Enable: [0xAC, 0x53, *, *] */
    if (cmd[0] == 0xAC && cmd[1] == 0x53) {
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_PE, "Programming enable");
        if (ret[1] != 0xAC || ret[2] != 0x53 || ret[3] != cmd[2]) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN,
                      "Warning: Unexpected bytes in reply!");
        }
        return;
    }

    /* Chip Erase: [0xAC, 0x80|*, *, *] */
    if (cmd[0] == 0xAC && (cmd[1] & 0x80)) {
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_CE, "Chip erase");
        int bit = (ret[2] & 0x80) >> 7;
        if (ret[1] != 0xAC || bit != 1 || ret[3] != cmd[2]) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN,
                      "Warning: Unexpected bytes in reply!");
        }
        return;
    }

    /* Read Fuse Bits: [0x50, 0x00, 0x00, *] */
    if (cmd[0] == 0x50 && cmd[1] == 0x00 && cmd[2] == 0x00) {
        snprintf(buf, sizeof(buf), "Read fuse bits: 0x%02x", ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RFB, buf);
        return;
    }

    /* Read Fuse High Bits: [0x58, 0x08, 0x00, *] */
    if (cmd[0] == 0x58 && cmd[1] == 0x08 && cmd[2] == 0x00) {
        snprintf(buf, sizeof(buf), "Read fuse high bits: 0x%02x", ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RHFB, buf);
        return;
    }

    /* Read Extended Fuse Bits: [0x50, 0x08, 0x00, *] */
    if (cmd[0] == 0x50 && cmd[1] == 0x08 && cmd[2] == 0x00) {
        snprintf(buf, sizeof(buf), "Read extended fuse bits: 0x%02x", ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_REFB, buf);
        return;
    }

    /* Read Signature Byte 0: [0x30, *, 0x00, *] */
    if (cmd[0] == 0x30 && cmd[2] == 0x00) {
        s->vendor_code = ret[3];
        const char *v = vendor_code_name(s->vendor_code);
        snprintf(buf, sizeof(buf), "Vendor code: 0x%02x (%s)", ret[3], v);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RSB0, buf);
        s->xx = cmd[1];
        s->yy = cmd[3];
        s->zz = ret[0];
        if (ret[1] != 0x30 || ret[2] != cmd[1]) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN,
                      "Warning: Unexpected bytes in reply!");
        }
        if (s->vendor_code != VENDOR_CODE_ATMEL) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN,
                      "Warning: Vendor code was not 0x1e (Atmel)!");
        }
        return;
    }

    /* Read Signature Byte 1: [0x30, *, 0x01, *] */
    if (cmd[0] == 0x30 && cmd[2] == 0x01) {
        s->part_fam_flash_size = ret[3];
        snprintf(buf, sizeof(buf), "Part family / memory size: 0x%02x", ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RSB1, buf);
        s->mm = cmd[3];
        s->ss_device = s->ss_cmd;
        if (ret[1] != 0x30 || ret[2] != cmd[1] || ret[0] != s->yy) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN,
                      "Warning: Unexpected bytes in reply!");
        }
        return;
    }

    /* Read Signature Byte 2: [0x30, *, 0x02, *] */
    if (cmd[0] == 0x30 && cmd[2] == 0x02) {
        s->part_number = ret[3];
        snprintf(buf, sizeof(buf), "Part number: 0x%02x", ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RSB2, buf);
        const char *p = part_name(s->part_fam_flash_size, s->part_number);
        snprintf(buf, sizeof(buf), "Device: Atmel %s", p);
        c_put(di, s->ss_device, s->es_cmd, s->out_ann, ANN_DEV, buf);
        if (ret[1] != 0x30 || ret[2] != s->xx || ret[0] != s->mm) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN,
                      "Warning: Unexpected bytes in reply!");
        }
        s->xx = s->yy = s->zz = s->mm = 0;
        return;
    }

    /* Read Lock Bits: [0x58, 0x00, *, *] */
    if (cmd[0] == 0x58 && cmd[1] == 0x00) {
        snprintf(buf, sizeof(buf), "Read lock bits: 0x%02x", ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RLB, buf);
        return;
    }

    /* Read EEPROM Memory: [0xA0, *, *, *] with cmd[1] & 0xC0 == 0x00 */
    if (cmd[0] == 0xA0 && (cmd[1] & 0xC0) == 0x00) {
        int addr = ((cmd[1] & 0x01) << 8) + cmd[2];
        snprintf(buf, sizeof(buf), "Read EEPROM Memory: [0x%03x]: 0x%02x", addr, ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_REEM, buf);
        return;
    }

    /* Read Program Memory: [0x20|0x28, *, *, *] with cmd[1] & 0xF0 == 0x00 */
    if ((cmd[0] == 0x20 || cmd[0] == 0x28) && (cmd[1] & 0xF0) == 0x00) {
        const char *hl = (cmd[0] & 0x08) ? "High" : "Low";
        int addr = ((cmd[1] & 0x0F) << 8) + cmd[2];
        snprintf(buf, sizeof(buf), "Read program memory %s: [0x%03x]: 0x%02x", hl, addr, ret[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RP, buf);
        return;
    }

    /* Load Program Memory Page: [0x40|0x48, *, *, *] with cmd[1] & 0xF0 == 0x00 */
    if ((cmd[0] == 0x40 || cmd[0] == 0x48) && (cmd[1] & 0xF0) == 0x00) {
        const char *hl = (cmd[0] & 0x08) ? "High" : "Low";
        int addr = cmd[2] & 0x1F;
        snprintf(buf, sizeof(buf), "Load program memory page %s: [0x%03x]: 0x%02x", hl, addr, cmd[3]);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_LPMP, buf);
        return;
    }

    /* Write Program Memory Page: [0x4C, *, *, *] with cmd[1] & 0xF0 == 0x00 */
    if (cmd[0] == 0x4C && (cmd[1] & 0xF0) == 0x00) {
        int addr = ((cmd[1] & 0x0F) << 3) + (cmd[2] >> 5);
        snprintf(buf, sizeof(buf), "Write program memory page: 0x%02x", addr);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WP, buf);
        return;
    }

    /* Unknown command */
    snprintf(buf, sizeof(buf),
             "Unknown command: %02x %02x %02x %02x (reply: %02x %02x %02x %02x)!",
             cmd[0], cmd[1], cmd[2], cmd[3],
             ret[0], ret[1], ret[2], ret[3]);
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN, buf);
}

static void avr_isp_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    avr_isp_state *s = (avr_isp_state *)c_decoder_get_private(di);
    if (!s) return;

    /* Ignore BITS and other non-DATA packets */
    if (strcmp(cmd, "DATA") != 0) return;

    int have_mosi, have_miso;
    uint8_t mosi = 0, miso = 0;
    parse_spi_data(fields, n_fields, &have_mosi, &have_miso, &mosi, &miso);

    if (s->byte_count == 0)
        s->ss_cmd = start_sample;

    s->mosi_bytes[s->byte_count] = mosi;
    s->miso_bytes[s->byte_count] = miso;
    s->byte_count++;

    if (s->byte_count < 4) return;

    s->es_cmd = end_sample;
    avr_isp_handle_command(di, s, s->mosi_bytes, s->miso_bytes);
    s->byte_count = 0;
}

static void avr_isp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(avr_isp_state)));
    }
    avr_isp_state *s = (avr_isp_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(avr_isp_state));
}

static void avr_isp_start(struct srd_decoder_inst *di)
{
    avr_isp_state *s = (avr_isp_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "avr_isp");
}

static void avr_isp_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void avr_isp_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder avr_isp_c_decoder = {
    .id = "avr_isp_c",
    .name = "AVR ISP(C)",
    .longname = "AVR In-System Programming (C)",
    .desc = "Atmel AVR In-System Programming (ISP) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = avr_isp_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = avr_isp_ann_rows,
    .inputs = avr_isp_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = avr_isp_tags,
    .num_tags = 1,
    .reset = avr_isp_reset,
    .start = avr_isp_start,
    .decode = avr_isp_decode,
    .destroy = avr_isp_destroy,
    .decode_upper = avr_isp_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &avr_isp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}