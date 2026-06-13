/*
 * Copyright (C) 2023 DreamSourceLab <support@dreamsourcelab.com>
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

#define PAN1321_MAX_CMD 512

enum {
    ANN_TEXT_VERBOSE = 0,
    ANN_TEXT,
    ANN_WARNINGS,
    NUM_ANN,
};

typedef struct {
    char cmd[2][PAN1321_MAX_CMD]; /* [0]=RX, [1]=TX */
    int cmd_len[2];
    uint64_t ss_block[2];
    uint64_t es_block[2];
    int out_ann;
} pan1321_state;

static const char *pan1321_inputs[] = {"uart", NULL};
static const char *pan1321_tags[] = {"Wireless/RF", NULL};

static const char *pan1321_ann_labels[][3] = {
    {"", "text-verbose", "Human-readable text (verbose)"},
    {"", "text", "Human-readable text"},
    {"", "warnings", "Human-readable warnings"},
};

static const int pan1321_row_text_classes[] = {ANN_TEXT_VERBOSE, ANN_TEXT, ANN_WARNINGS, -1};

static const struct srd_c_ann_row pan1321_ann_rows[] = {
    {"text", "Text", pan1321_row_text_classes, 3},
};

static void pan1321_putx(struct srd_decoder_inst *di, pan1321_state *s, int rxtx, int cls, const char *text)
{
    c_put(di, s->ss_block[rxtx], s->es_block[rxtx], s->out_ann, cls, text);
}

static void pan1321_handle_host_command(struct srd_decoder_inst *di, pan1321_state *s, const char *cmd)
{
    if (strncmp(cmd, "AT+JAAC", 7) == 0) {
        /* AT+JAAC=<auto_accept> (0 or 1) */
        const char *p = cmd + 7;
        if (*p == '=')
            p++;
        if (strcmp(p, "0") != 0 && strcmp(p, "1") != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Warning: Invalid JAAC parameter \"%s\"", p);
            pan1321_putx(di, s, 1, ANN_WARNINGS, buf);
            return;
        }
        const char *x = (strcmp(p, "1") == 0) ? "Auto" : "Don't auto";
        char buf[256];
        snprintf(buf, sizeof(buf), "%s-accept new connections", x);
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "%s-accept connections", x);
        pan1321_putx(di, s, 1, ANN_TEXT, buf);
    } else if (strncmp(cmd, "AT+JPRO", 7) == 0) {
        /* AT+JPRO=<mode> (0 or 1) */
        const char *p = cmd + 7;
        if (*p == '=')
            p++;
        if (strcmp(p, "0") != 0 && strcmp(p, "1") != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Warning: Invalid JPRO parameter \"%s\"", p);
            pan1321_putx(di, s, 1, ANN_WARNINGS, buf);
            return;
        }
        const char *x = (strcmp(p, "0") == 0) ? "Leaving" : "Entering";
        const char *onoff = (strcmp(p, "0") == 0) ? "off" : "on";
        char buf[256];
        snprintf(buf, sizeof(buf), "%s production mode", x);
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "Production mode = %s", onoff);
        pan1321_putx(di, s, 1, ANN_TEXT, buf);
    } else if (strncmp(cmd, "AT+JRES", 7) == 0) {
        /* AT+JRES - no params */
        if (strcmp(cmd, "AT+JRES") != 0) {
            pan1321_putx(di, s, 1, ANN_WARNINGS, "Warning: Invalid JRES usage.");
            return;
        }
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, "Triggering a software reset");
        pan1321_putx(di, s, 1, ANN_TEXT, "Reset");
    } else if (strncmp(cmd, "AT+JSDA", 7) == 0) {
        /* AT+JSDA=<l>,<d> */
        const char *params = cmd + 7;
        if (*params == '=')
            params++;
        /* Find comma separator */
        const char *comma = strchr(params, ',');
        if (!comma) {
            pan1321_putx(di, s, 1, ANN_WARNINGS, "Warning: Invalid JSDA format.");
            return;
        }
        int len_val = atoi(params);
        const char *d = comma + 1;
        int d_len = (int)strlen(d);
        if (len_val != d_len) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Warning: Data length mismatch (%d != %d).", len_val, d_len);
            pan1321_putx(di, s, 1, ANN_WARNINGS, buf);
        }
        /* Format data as hex */
        char hex_buf[256];
        int pos = 0;
        for (int i = 0; d[i] && pos < 200; i++)
            pos += snprintf(hex_buf + pos, sizeof(hex_buf) - pos, "%02x ", (unsigned char)d[i]);
        if (pos > 0)
            hex_buf[pos - 1] = '\0'; /* Remove trailing space */
        char buf[512];
        snprintf(buf, sizeof(buf), "Sending %d data bytes: %s", len_val, hex_buf);
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "Send %d = %s", len_val, hex_buf);
        pan1321_putx(di, s, 1, ANN_TEXT, buf);
    } else if (strncmp(cmd, "AT+JSEC", 7) == 0) {
        /* AT+JSEC=<secmode>,<linkkey>,<pintype>,<pinlen>,<pin> */
        int cmd_len = (int)strlen(cmd);
        char pin[5] = "";
        if (cmd_len >= 4) {
            /* Last 4 chars are the PIN */
            strncpy(pin, cmd + cmd_len - 4, 4);
            pin[4] = '\0';
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "Host set the Bluetooth PIN to \"%s\"", pin);
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "PIN = %s", pin);
        pan1321_putx(di, s, 1, ANN_TEXT, buf);
    } else if (strncmp(cmd, "AT+JSLN", 7) == 0) {
        /* AT+JSLN=<namelen>,<name> */
        const char *params = cmd + 7;
        if (*params == '=')
            params++;
        const char *name = strchr(params, ',');
        if (name)
            name++;
        else
            name = params;
        char buf[512];
        snprintf(buf, sizeof(buf), "Host set the Bluetooth name to \"%s\"", name);
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "BT name = %s", name);
        pan1321_putx(di, s, 1, ANN_TEXT, buf);
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf), "Host sent unsupported command: %s", cmd);
        pan1321_putx(di, s, 1, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "Unsupported command: %s", cmd);
        pan1321_putx(di, s, 1, ANN_TEXT, buf);
    }
}

static void pan1321_handle_device_reply(struct srd_decoder_inst *di, pan1321_state *s, const char *cmd)
{
    if (strcmp(cmd, "ROK") == 0) {
        pan1321_putx(di, s, 0, ANN_TEXT_VERBOSE, "Device initialized correctly");
        pan1321_putx(di, s, 0, ANN_TEXT, "Init");
    } else if (strcmp(cmd, "OK") == 0) {
        pan1321_putx(di, s, 0, ANN_TEXT_VERBOSE, "Device acknowledged last command");
        pan1321_putx(di, s, 0, ANN_TEXT, "ACK");
    } else if (strncmp(cmd, "ERR", 3) == 0) {
        const char *error = cmd + 3;
        if (*error == '=')
            error++;
        char buf[256];
        snprintf(buf, sizeof(buf), "Device sent error code %s", error);
        pan1321_putx(di, s, 0, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "ERR = %s", error);
        pan1321_putx(di, s, 0, ANN_TEXT, buf);
    } else {
        char buf[512];
        snprintf(buf, sizeof(buf), "Device sent an unknown reply: %s", cmd);
        pan1321_putx(di, s, 0, ANN_TEXT_VERBOSE, buf);
        snprintf(buf, sizeof(buf), "Unknown reply: %s", cmd);
        pan1321_putx(di, s, 0, ANN_TEXT, buf);
    }
}

static void pan1321_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    pan1321_state *s = (pan1321_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;
    uint8_t rxtx = fields[1].u8;
    if (rxtx > 1)
        return;

    /* Record command start */
    if (s->cmd_len[rxtx] == 0)
        s->ss_block[rxtx] = start_sample;

    /* Buffer character */
    if (s->cmd_len[rxtx] < PAN1321_MAX_CMD - 2) {
        s->cmd[rxtx][s->cmd_len[rxtx]++] = (char)byte_val;
        s->cmd[rxtx][s->cmd_len[rxtx]] = '\0';
    }

    /* Detect \r\n */
    if (s->cmd_len[rxtx] >= 2 &&
        s->cmd[rxtx][s->cmd_len[rxtx] - 2] == '\r' &&
        s->cmd[rxtx][s->cmd_len[rxtx] - 1] == '\n') {
        /* Remove \r\n */
        s->cmd[rxtx][s->cmd_len[rxtx] - 2] = '\0';
        s->es_block[rxtx] = end_sample;
        if (rxtx == 0)
            pan1321_handle_device_reply(di, s, s->cmd[rxtx]);
        else
            pan1321_handle_host_command(di, s, s->cmd[rxtx]);
        s->cmd_len[rxtx] = 0;
        s->cmd[rxtx][0] = '\0';
    }
}

static void pan1321_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(pan1321_state)));
    }
    pan1321_state *s = (pan1321_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(pan1321_state));
}

static void pan1321_start(struct srd_decoder_inst *di)
{
    pan1321_state *s = (pan1321_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pan1321");
}

static void pan1321_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void pan1321_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder pan1321_c_decoder = {
    .id = "pan1321_c",
    .name = "PAN1321(C)",
    .longname = "Panasonic PAN1321 (C)",
    .desc = "Bluetooth RF module with Serial Port Profile (SPP). (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = pan1321_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = pan1321_ann_rows,
    .inputs = pan1321_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = pan1321_tags,
    .num_tags = 1,
    .reset = pan1321_reset,
    .start = pan1321_start,
    .decode = pan1321_decode,
    .destroy = pan1321_destroy,
    .decode_upper = pan1321_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &pan1321_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}