/*
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

enum {
    ANN_HEADER = 0,
    ANN_DATASIZE,
    ANN_CHECKSUM,
    ANN_ANSWER,
    ANN_COMMAND,
    ANN_DATA_RX,
    ANN_DATA_TX,
    ANN_PACKET_RX,
    ANN_PACKET_TX,
    ANN_WARN,
    NUM_ANN,
};

#define STRELETZ_MAX_PKT 64
#define STRELETZ_MIN_PKT 4

#define RX 0
#define TX 1

enum buf_pos {
    BUF_HEADER = 1,
    BUF_DATA_SIZE = 2,
    BUF_DATA_TYPE = 3,
    BUF_DATA_START = 4,
};

typedef struct {
    uint8_t accum_bytes[STRELETZ_MAX_PKT];
    int accum_count;
    int rxtx;
    int packet_size;
    uint64_t packet_ss;
    uint64_t packet_es;
    uint64_t data_ss;
    uint64_t data_es;
    int buf_pos;
    uint8_t checksum;
    uint8_t header_rx;
    uint8_t header_tx;
    int out_ann;
    int out_python;
} streletz_state;

static const char *streletz_inputs[] = {"uart", NULL};
static const char *streletz_outputs[] = {"streletz", NULL};
static const char *streletz_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option streletz_options[] = {
    {"header_tx", "dec_streletz_opt_header_tx", "Request header", NULL, NULL},
    {"header_rx", "dec_streletz_opt_header_rx", "Response header", NULL, NULL},
};

static const char *streletz_ann_labels[][3] = {
    {"", "head", "Header"},
    {"", "datasize", "Data Size"},
    {"", "checksum", "Checksum"},
    {"", "answer", "Answer"},
    {"", "command", "Command"},
    {"", "rx-data", "RX Data"},
    {"", "tx-data", "TX Data"},
    {"", "rx-packet", "RX packet"},
    {"", "tx-packet", "TX packet"},
    {"", "warning", "Warning"},
};

static const int streletz_row_framing_classes[] = {ANN_HEADER, ANN_DATASIZE, ANN_CHECKSUM};
static const int streletz_row_data_classes[] = {ANN_ANSWER, ANN_COMMAND, ANN_DATA_RX, ANN_DATA_TX};
static const int streletz_row_warnings_classes[] = {ANN_WARN};
static const int streletz_row_packets_classes[] = {ANN_PACKET_RX, ANN_PACKET_TX};
static const struct srd_c_ann_row streletz_ann_rows[] = {
    {"framing", "Framing", streletz_row_framing_classes, 3},
    {"data", "Data", streletz_row_data_classes, 4},
    {"warnings", "Warnings", streletz_row_warnings_classes, 1},
    {"packets", "Packets", streletz_row_packets_classes, 2},
};

static void streletz_reset_state(streletz_state *s)
{
    s->checksum = 0;
    s->accum_count = 0;
    s->rxtx = 0;
    s->packet_size = 0;
    s->packet_ss = 0;
    s->packet_es = 0;
    s->data_ss = 0;
    s->data_es = 0;
    s->buf_pos = 0;
}

static void streletz_handle_byte(struct srd_decoder_inst *di, streletz_state *s,
    uint64_t ss, uint64_t es, uint8_t byte_val, int rxtx)
{
    if (s->buf_pos > 0) {
        if (s->rxtx == rxtx) {
            s->buf_pos++;
            if (s->accum_count < STRELETZ_MAX_PKT)
                s->accum_bytes[s->accum_count++] = byte_val;
            s->checksum ^= byte_val;
        } else {
            streletz_reset_state(s);
        }
    }

    /* Wait for header */
    if (s->buf_pos < BUF_HEADER) {
        uint8_t expected = (rxtx == TX) ? s->header_tx : s->header_rx;
        if (byte_val == expected) {
            s->buf_pos = BUF_HEADER;
            s->rxtx = rxtx;
            s->accum_bytes[0] = byte_val;
            s->accum_count = 1;
            s->packet_ss = ss;
            s->checksum = byte_val;
            char hdr_str[32];
            snprintf(hdr_str, sizeof(hdr_str), "HEAD: 0x%02X", byte_val);
            c_put(di, ss, es, s->out_ann, ANN_HEADER, hdr_str, "HEAD", "H");
        }
        return;
    }

    /* Data size */
    if (s->buf_pos == BUF_DATA_SIZE) {
        s->packet_size = STRELETZ_MIN_PKT + byte_val;
        if (s->packet_size > STRELETZ_MAX_PKT) {
            char warn_str[32];
            snprintf(warn_str, sizeof(warn_str), "Wrong DS: 0x%02X", byte_val);
            c_put(di, ss, es, s->out_ann, ANN_WARN, warn_str, "WDS");
            streletz_reset_state(s);
            return;
        }
        char ds_str[32];
        snprintf(ds_str, sizeof(ds_str), "DS: 0x%02X", byte_val);
        c_put(di, ss, es, s->out_ann, ANN_DATASIZE, ds_str, "DS");
    }
    /* Data type */
    else if (s->buf_pos == BUF_DATA_TYPE) {
        if (rxtx == TX) {
            char cmd_str[32];
            snprintf(cmd_str, sizeof(cmd_str), "CMD: 0x%02X", byte_val);
            c_put(di, ss, es, s->out_ann, ANN_COMMAND, cmd_str, "CMD");
        } else {
            char ans_str[32];
            snprintf(ans_str, sizeof(ans_str), "ANS: 0x%02X", byte_val);
            c_put(di, ss, es, s->out_ann, ANN_ANSWER, ans_str, "ANS");
        }
    }
    /* Data start */
    else if (s->buf_pos == BUF_DATA_START) {
        s->data_ss = ss;
    }

    /* End of data block */
    if (s->buf_pos == s->packet_size - 1) {
        s->data_es = es;
    }

    /* Checksum byte: end of packet */
    if (s->buf_pos == s->packet_size) {
        s->packet_es = es;
        const char *rxtx_str = (rxtx == TX) ? "TX" : "RX";

        if (s->checksum == 0) {
            /* Correct checksum */
            char cs_str[32];
            snprintf(cs_str, sizeof(cs_str), "CS: 0x%02X", byte_val);
            c_put(di, ss, es, s->out_ann, ANN_CHECKSUM, cs_str, "CS");

            int packet_ann = (rxtx == TX) ? ANN_PACKET_TX : ANN_PACKET_RX;
            /* Build packet hex string */
            char pkt_str[256];
            int pos = 0;
            for (int i = 0; i < s->accum_count && pos < (int)sizeof(pkt_str) - 4; i++) {
                pos += snprintf(pkt_str + pos, sizeof(pkt_str) - pos, "%02X ", s->accum_bytes[i]);
            }
            char pkt_ann_str[512];
            snprintf(pkt_ann_str, sizeof(pkt_ann_str), "%s PACKET: %s", rxtx_str, pkt_str);
            c_put(di, s->packet_ss, s->packet_es, s->out_ann, packet_ann,
                      pkt_ann_str, rxtx_str, rxtx_str);

            /* Data section */
            if (s->packet_size > STRELETZ_MIN_PKT) {
                char data_str[256];
                int dpos = 0;
                for (int i = BUF_DATA_START - 1; i < s->packet_size - 1 && dpos < (int)sizeof(data_str) - 4; i++) {
                    dpos += snprintf(data_str + dpos, sizeof(data_str) - dpos, "%02X ", s->accum_bytes[i]);
                }
                int data_ann = (rxtx == TX) ? ANN_DATA_TX : ANN_DATA_RX;
                char data_ann_str[512];
                snprintf(data_ann_str, sizeof(data_ann_str), "%s DATA: %s", rxtx_str, data_str);
                c_put(di, s->data_ss, s->data_es, s->out_ann, data_ann,
                          data_ann_str, rxtx_str, "D");
            }

            /* Protocol output for upper-layer decoders */
            
            c_proto(di, s->packet_ss, s->packet_es, s->out_python,
                                "PACKET", C_BYTES(s->accum_bytes, s->accum_count), C_END);
        } else {
            /* Wrong checksum */
            char pkt_str[256];
            int pos = 0;
            for (int i = 0; i < s->accum_count && pos < (int)sizeof(pkt_str) - 4; i++) {
                pos += snprintf(pkt_str + pos, sizeof(pkt_str) - pos, "%02X ", s->accum_bytes[i]);
            }
            char err_str[512];
            snprintf(err_str, sizeof(err_str), "Err %s PACKET: %s", rxtx_str, pkt_str);
            c_put(di, s->packet_ss, s->packet_es, s->out_ann, ANN_WARN,
                      err_str, rxtx_str, "EP");
        }

        streletz_reset_state(s);
    }
}

static void streletz_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    streletz_state *s = (streletz_state *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Python version only processes FRAME events */
    if (strcmp(cmd, "FRAME") != 0)
        return;
    if (n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;
    int rxtx = fields[1].u8;

    streletz_handle_byte(di, s, start_sample, end_sample, byte_val, rxtx);
}

static void streletz_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(streletz_state)));
    }
    streletz_state *s = (streletz_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(streletz_state));
    s->header_rx = 0x9D;  /* 157 */
    s->header_tx = 0xD9;  /* 217 */
    streletz_reset_state(s);
}

static void streletz_start(struct srd_decoder_inst *di)
{
    streletz_state *s = (streletz_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "streletz");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "streletz");

    s->header_tx = (uint8_t)c_opt_int(di, "header_tx", 217);
    s->header_rx = (uint8_t)c_opt_int(di, "header_rx", 157);
}

static void streletz_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void streletz_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder streletz_c_decoder = {
    .id = "streletz_c",
    .name = "Streletz(C)",
    .longname = "Streletz RS232 Serial Bus (C)",
    .desc = "Serial bus for guard system Streletz (C implementation)",
    .license = "mit",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = streletz_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = streletz_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = streletz_ann_rows,
    .inputs = streletz_inputs,
    .num_inputs = 1,
    .outputs = streletz_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = streletz_tags,
    .num_tags = 1,
    .reset = streletz_reset,
    .start = streletz_start,
    .decode = streletz_decode,
    .destroy = streletz_destroy,
    .decode_upper = streletz_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    streletz_options[0].def = g_variant_new_int64(217);
    streletz_options[1].def = g_variant_new_int64(157);

    return &streletz_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}