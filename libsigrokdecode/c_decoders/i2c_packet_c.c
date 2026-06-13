/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2022 Sergey Spivak <sespivak@yandex.ru>
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

static void fmt_binary(uint8_t val, char *buf, int bufsize) {
    if (bufsize < 9) { buf[0] = '\0'; return; }
    for (int i = 7; i >= 0; i--) buf[7 - i] = (val & (1 << i)) ? '1' : '0';
    buf[8] = '\0';
}

enum {
    ANN_DATA = 0,
    NUM_ANN,
};

#define MAX_PACKET_DATA 4096
#define MAX_STR_LEN 8192

typedef struct {
    int out_ann;
    int out_py;
    uint8_t packet_data[MAX_PACKET_DATA];
    int packet_data_len;
    char packet_str[MAX_STR_LEN];
    char packet_str_short[MAX_STR_LEN / 2];
    uint64_t packet_ss;
    uint64_t packet_part_ss;
    uint64_t packet_es;
    int read_sign;      /* 0=write, 1=read */
    uint8_t address;
    int format;         /* 0=ascii, 1=dec, 2=hex, 3=oct, 4=bin */
} i2c_packet_state;

static const char *i2c_packet_inputs[] = {"i2c", NULL};
static const char *i2c_packet_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option i2c_packet_options[] = {
    {"format", "dec_i2c_packet_opt_format", "Data format", NULL, NULL},
};

static const char *i2c_packet_ann_labels[][3] = {
    {"", "data", "Data"},
};

static const int i2c_packet_row_packet_classes[] = {ANN_DATA, -1};

static const struct srd_c_ann_row i2c_packet_ann_rows[] = {
    {"packet", "Packet", i2c_packet_row_packet_classes, 1},
};

static void i2c_packet_reset_state(i2c_packet_state *s)
{
    s->packet_data_len = 0;
    s->packet_str[0] = '\0';
    s->packet_str_short[0] = '\0';
    s->packet_ss = 0;
    s->packet_part_ss = 0;
    s->packet_es = 0;
    s->address = 0;
    s->read_sign = 0;
}

static void i2c_packet_format_value(i2c_packet_state *s, uint8_t v, char *buf, int buflen)
{
    switch (s->format) {
    case 1: /* dec */
        snprintf(buf, buflen, "%d", v);
        break;
    case 2: /* hex */
        snprintf(buf, buflen, "%02X", v);
        break;
    case 3: /* oct */
        snprintf(buf, buflen, "%03o", v);
        break;
    case 4: /* bin */
        fmt_binary(v, buf, buflen);
        break;
    default: /* ascii */
        if (v >= 32 && v <= 126)
            snprintf(buf, buflen, "%c", v);
        else
            snprintf(buf, buflen, "[%02X]", v);
        break;
    }
}

static void i2c_packet_format_current(i2c_packet_state *s,
    char *cur_str, int cur_len,
    char *cur_short, int short_len)
{
    int pos = 0;
    int spos = 0;

    pos += snprintf(cur_str + pos, cur_len - pos, "0x%02X %s: ",
                    s->address, s->read_sign ? "RD" : "WR");
    spos += snprintf(cur_short + spos, short_len - spos, "%02X %s: ",
                     s->address, s->read_sign ? "RD" : "WR");

    for (int i = 0; i < s->packet_data_len; i++) {
        char vbuf[16];
        i2c_packet_format_value(s, s->packet_data[i], vbuf, sizeof(vbuf));
        if (s->format != 0 && i > 0) {
            pos += snprintf(cur_str + pos, cur_len - pos, " ");
            spos += snprintf(cur_short + spos, short_len - spos, " ");
        }
        pos += snprintf(cur_str + pos, cur_len - pos, "%s", vbuf);
        spos += snprintf(cur_short + spos, short_len - spos, "%s", vbuf);
    }
}

static void i2c_packet_handle_packet(struct srd_decoder_inst *di,
    i2c_packet_state *s, int start_repeat)
{
    if (s->packet_data_len == 0) {
        if (!start_repeat)
            i2c_packet_reset_state(s);
        return;
    }

    char cur_str[MAX_STR_LEN];
    char cur_short[MAX_STR_LEN / 2];
    i2c_packet_format_current(s, cur_str, sizeof(cur_str),
                              cur_short, sizeof(cur_short));

    /* Output Python protocol data: PACKET READ/WRITE */
    const char *ptype = s->read_sign ? "PACKET READ" : "PACKET WRITE";
    /* Pack data: fields[0].u8 = address, data[1..] = packet_data */
    int py_len = 1 + s->packet_data_len;
    uint8_t *py_data = (uint8_t *)g_malloc(py_len);
    py_data[0] = s->address;
    if (s->packet_data_len > 0)
        memcpy(py_data + 1, s->packet_data, s->packet_data_len);
    c_proto(di, s->packet_part_ss, s->packet_es,
                         s->out_py, ptype, C_U8(s->address), C_BYTES(s->packet_data, s->packet_data_len), C_END);
    g_free(py_data);

    if (!start_repeat) {
        /* Output TRANSACTION END */
        c_proto(di, s->packet_es, s->packet_es, s->out_py, "TRANSACTION END", C_END);
    }

    if (start_repeat) {
        /* Merge into packet_str */
        if (s->packet_str[0]) {
            int len = strlen(s->packet_str);
            snprintf(s->packet_str + len, sizeof(s->packet_str) - len,
                     " [SR] %s", cur_str);
            len = strlen(s->packet_str_short);
            snprintf(s->packet_str_short + len, sizeof(s->packet_str_short) - len,
                     " [SR] %s", cur_short);
        } else {
            snprintf(s->packet_str, sizeof(s->packet_str), "%s", cur_str);
            snprintf(s->packet_str_short, sizeof(s->packet_str_short), "%s", cur_short);
        }
        s->packet_data_len = 0;
    } else {
        /* Final output annotation */
        char final_str[MAX_STR_LEN * 4];
        char final_short[MAX_STR_LEN * 4];
        if (s->packet_str[0]) {
            snprintf(final_str, sizeof(final_str), "%s [SR] %s",
                     s->packet_str, cur_str);
            snprintf(final_short, sizeof(final_short), "%s [SR] %s",
                     s->packet_str_short, cur_short);
        } else {
            snprintf(final_str, sizeof(final_str), "%s", cur_str);
            snprintf(final_short, sizeof(final_short), "%s", cur_short);
        }
        c_put(di, s->packet_ss, s->packet_es, s->out_ann, ANN_DATA, final_str, final_short);
        i2c_packet_reset_state(s);
    }
}

static void i2c_packet_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    i2c_packet_state *s = (i2c_packet_state *)c_decoder_get_private(di);
    if (!s)
        return;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strncmp(cmd, "DATA", 4) == 0) {
        if (s->packet_data_len < MAX_PACKET_DATA)
            s->packet_data[s->packet_data_len++] = databyte;
        s->packet_es = end_sample;
    } else if (strncmp(cmd, "START", 5) == 0) {
        int start_repeat = (strcmp(cmd, "START REPEAT") == 0);
        i2c_packet_handle_packet(di, s, start_repeat);
        s->packet_part_ss = start_sample;
        if (!start_repeat)
            s->packet_ss = start_sample;
    } else if (strncmp(cmd, "ADDRESS", 7) == 0) {
        s->address = databyte;
        s->read_sign = (strstr(cmd, "READ") != NULL) ? 1 : 0;
        s->packet_es = end_sample;
    } else if (strstr(cmd, "ACK") != NULL) {
        s->packet_es = end_sample;
    } else if (strcmp(cmd, "STOP") == 0) {
        s->packet_es = end_sample;
        i2c_packet_handle_packet(di, s, 0);
    }
}

static void i2c_packet_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(i2c_packet_state)));
    }
    i2c_packet_state *s = (i2c_packet_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(i2c_packet_state));
    i2c_packet_reset_state(s);
}

static void i2c_packet_start(struct srd_decoder_inst *di)
{
    i2c_packet_state *s = (i2c_packet_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "i2c_packet");
    s->out_py = c_reg_out(di, SRD_OUTPUT_PYTHON, "i2c_packet");

    const char *fmt = c_opt_str(di, "format", "hex");
    if (fmt && strcmp(fmt, "ascii") == 0)
        s->format = 0;
    else if (fmt && strcmp(fmt, "dec") == 0)
        s->format = 1;
    else if (fmt && strcmp(fmt, "hex") == 0)
        s->format = 2;
    else if (fmt && strcmp(fmt, "oct") == 0)
        s->format = 3;
    else if (fmt && strcmp(fmt, "bin") == 0)
        s->format = 4;
    else
        s->format = 2;
}

static void i2c_packet_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void i2c_packet_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder i2c_packet_c_decoder = {
    .id = "i2c_packet_c",
    .name = "I2C packet(C)",
    .longname = "I2C packet builder (C)",
    .desc = "Concatenate I2C data to packets. (C implementation)",
    .license = "mit",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = i2c_packet_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = i2c_packet_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = i2c_packet_ann_rows,
    .inputs = i2c_packet_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = i2c_packet_tags,
    .num_tags = 1,
    .reset = i2c_packet_reset,
    .start = i2c_packet_start,
    .decode = i2c_packet_decode,
    .destroy = i2c_packet_destroy,
    .decode_upper = i2c_packet_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    i2c_packet_options[0].def = g_variant_new_string("hex");
    GSList *fmt_vals = NULL;
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("ascii"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("dec"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("hex"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("oct"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("bin"));
    i2c_packet_options[0].values = fmt_vals;

    return &i2c_packet_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}