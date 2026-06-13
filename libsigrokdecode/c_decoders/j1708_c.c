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

#define J1708_BAUD 9600
#define MIN_BUS_ACCESS_BIT_TIMES 12
#define MAX_MSG_LEN 256

enum {
    ANN_DATUM = 0,
    ANN_INFO,
    ANN_ERROR,
    ANN_INLINE_ERROR,
    ANN_DELAY,
    ANN_BUS_ACCESS,
    NUM_ANN,
};

enum {
    BIN_MID = 0,
    BIN_PAYLOAD,
    BIN_CRC,
    NUM_BIN,
};

enum j1708_fsm_state {
    J1708_FSM_WAIT_STARTBIT,
    J1708_FSM_WAIT_DATA,
    J1708_FSM_WAIT_STOPBIT,
};

typedef struct {
    enum j1708_fsm_state fsm_state;
    uint8_t data[MAX_MSG_LEN];
    int data_len;
    int n_fields;
    uint64_t first_startbit_ss;
    uint64_t prev_stopbit_es;
    uint64_t last_valid_msg_stopbit_es;
    double bit_width;
    int message_break;
    int out_ann;
    int out_bin;
} j1708_state;

static const char *j1708_inputs[] = {"uart", NULL};
static const char *j1708_tags[] = {"Automotive", NULL};

static struct srd_decoder_option j1708_options[] = {
    {"message_break", "dec_j1708_opt_message_break", "Delay (in bit times) for message break", NULL, NULL},
};

static const char *j1708_ann_labels[][3] = {
    {"", "datum", "A J1708 message"},
    {"", "info", "Protocol info"},
    {"", "error", "Protocol violation or error"},
    {"", "inline_error", "Protocol violation or error"},
    {"", "delay", "Inter-message Delay [bit times]"},
    {"", "bus_access", "Bus Access time violation [bit times]"},
};

static const int j1708_row_fields_classes[] = {ANN_INFO, -1};
static const int j1708_row_data_classes[] = {ANN_DATUM, ANN_INLINE_ERROR, -1};
static const int j1708_row_errors_classes[] = {ANN_ERROR, ANN_BUS_ACCESS, -1};
static const int j1708_row_delays_classes[] = {ANN_DELAY, -1};

static const struct srd_c_ann_row j1708_ann_rows[] = {
    {"fields", "RX Fields", j1708_row_fields_classes, 1},
    {"data", "RX Data", j1708_row_data_classes, 2},
    {"errors", "RX Errors", j1708_row_errors_classes, 2},
    {"delays", "RX Message Delays", j1708_row_delays_classes, 1},
};

static const struct srd_decoder_binary j1708_binary[] = {
    {BIN_MID, "mid", "J1708 MID"},
    {BIN_PAYLOAD, "payload", "J1708 Payload"},
    {BIN_CRC, "crc", "J1708 Checksum"},
};

static uint8_t j1708_checksum(uint8_t *msg, int len)
{
    uint16_t sum = 0;
    for (int i = 0; i < len; i++)
        sum = (sum + msg[i]) & 0xFF;
    return (~sum + 1) & 0xFF;
}

static void j1708_flush_message(struct srd_decoder_inst *di, j1708_state *s)
{
    if (s->data_len == 0)
        return;

    /* Arm message delay measurement */
    s->last_valid_msg_stopbit_es = s->prev_stopbit_es;

    if (s->data_len < 2) {
        /* Too short for checksum validation */
        char buf[512];
        int pos = 0;
        for (int i = 0; i < s->data_len && pos < 200; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", s->data[i]);
        c_put(di, s->first_startbit_ss, s->prev_stopbit_es, s->out_ann, ANN_INLINE_ERROR, buf);
        c_put(di, s->first_startbit_ss, s->prev_stopbit_es, s->out_ann, ANN_ERROR, "Message too short");
        s->data_len = 0;
        return;
    }

    /* Validate checksum */
    uint8_t calc_crc = j1708_checksum(s->data, s->data_len - 1);
    uint8_t recv_crc = s->data[s->data_len - 1];

    if (calc_crc != recv_crc) {
        /* Checksum error */
        char buf[512];
        int pos = 0;
        for (int i = 0; i < s->data_len - 1 && pos < 200; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", s->data[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "(%02x)", recv_crc);
        c_put(di, s->first_startbit_ss, s->prev_stopbit_es, s->out_ann, ANN_INLINE_ERROR, buf);

        uint64_t crc_ss = (uint64_t)(s->prev_stopbit_es - s->bit_width * 10);
        c_put(di, crc_ss, s->prev_stopbit_es, s->out_ann, ANN_ERROR, "Checksum", "CRC");
    } else {
        /* Valid message */
        char buf[512];
        int pos = 0;
        for (int i = 0; i < s->data_len - 1 && pos < 200; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x", s->data[i]);
        c_put(di, s->first_startbit_ss, s->prev_stopbit_es, s->out_ann, ANN_DATUM, buf);

        /* MID field - match Python: 'MID: ' + hex(fields[0].u8), hex(fields[0].u8), 'MID' */
        char mid_long[32], mid_mid[16];
        snprintf(mid_long, sizeof(mid_long), "MID: 0x%x", s->data[0]);
        snprintf(mid_mid, sizeof(mid_mid), "0x%x", s->data[0]);
        uint64_t mid_es = (uint64_t)(s->first_startbit_ss + s->bit_width * 10);
        c_put(di, s->first_startbit_ss, mid_es, s->out_ann, ANN_INFO, mid_long, mid_mid, "MID");
        c_put_bin(di, s->first_startbit_ss, mid_es, s->out_bin, BIN_MID, 1, &s->data[0]);

        /* Payload field - match Python: 'Payload: ' + hex, hex, 'Payload' */
        if (s->data_len > 2) {
            char payload_buf[256];
            int ppos = 0;
            for (int i = 1; i < s->data_len - 1 && ppos < 200; i++)
                ppos += snprintf(payload_buf + ppos, sizeof(payload_buf) - ppos, "%02x", s->data[i]);
            char payload_long[280];
            snprintf(payload_long, sizeof(payload_long), "Payload: %s", payload_buf);
            uint64_t payload_es = (uint64_t)(s->prev_stopbit_es - s->bit_width * 10);
            c_put(di, mid_es, payload_es, s->out_ann, ANN_INFO, payload_long, payload_buf, "Payload");
            c_put_bin(di, mid_es, payload_es, s->out_bin, BIN_PAYLOAD,
                                 s->data_len - 2, &s->data[1]);
        }

        /* CRC field - match Python: 'CRC: ' + hex, hex, 'CRC' */
        char crc_buf[32], crc_mid[16];
        snprintf(crc_buf, sizeof(crc_buf), "CRC: %02x", recv_crc);
        snprintf(crc_mid, sizeof(crc_mid), "%02x", recv_crc);
        uint64_t crc_ss = (uint64_t)(s->prev_stopbit_es - s->bit_width * 10);
        c_put(di, crc_ss, s->prev_stopbit_es, s->out_ann, ANN_INFO, crc_buf, crc_mid, "CRC");
        c_put_bin(di, crc_ss, s->prev_stopbit_es, s->out_bin, BIN_CRC, 1, &recv_crc);
    }

    s->data_len = 0;
}

static void j1708_message_ready(j1708_state *s)
{
    s->fsm_state = J1708_FSM_WAIT_STARTBIT;
    s->prev_stopbit_es = 0;
    s->first_startbit_ss = 0;
    s->data_len = 0;
}

static void j1708_flush_message_break_measurement(struct srd_decoder_inst *di, j1708_state *s,
                                                   uint64_t startbit_ss)
{
    if (s->last_valid_msg_stopbit_es == 0)
        return;

    double inter_delay = (double)(startbit_ss - s->last_valid_msg_stopbit_es) / s->bit_width;
    char buf[32];
    snprintf(buf, sizeof(buf), "%05.1f", inter_delay);
    c_put(di, s->last_valid_msg_stopbit_es, startbit_ss, s->out_ann, ANN_DELAY, buf);
    if (inter_delay < MIN_BUS_ACCESS_BIT_TIMES) {
        c_put(di, s->last_valid_msg_stopbit_es, startbit_ss, s->out_ann, ANN_BUS_ACCESS, buf);
    }
}

static void j1708_maybe_flush_message(struct srd_decoder_inst *di, j1708_state *s,
                                       uint64_t startbit_ss)
{
    if (s->prev_stopbit_es > 0 && s->bit_width > 0) {
        double delay_bits = (double)(startbit_ss - s->prev_stopbit_es) / s->bit_width;
        if ((int)delay_bits > s->message_break) {
            j1708_flush_message(di, s);
            j1708_message_ready(s);
        }
    }
}

static void j1708_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    j1708_state *s = (j1708_state *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Get bit_width from samplerate, like Python decoder does */
    if (s->bit_width == 0) {
        uint64_t samplerate = c_samplerate(di);
        if (samplerate > 0) {
            s->bit_width = (double)samplerate / (double)J1708_BAUD;
        } else {
            /* Fallback: estimate from STARTBIT/STOPBIT duration */
            if (strcmp(cmd, "STARTBIT") == 0 || strcmp(cmd, "STOPBIT") == 0) {
                s->bit_width = (double)(end_sample - start_sample);
                return;
            } else if (strcmp(cmd, "DATA") != 0) {
                return;
            }
        }
    }

    /* Only process RX */
    if (strcmp(cmd, "DATA") == 0) {
        if (n_fields < 2)
            return;
        uint8_t rxtx = fields[1].u8;
        if (rxtx != 0)
            return;
    }

    /* Ignore FRAME, BREAK */
    if (strcmp(cmd, "FRAME") == 0 || strcmp(cmd, "BREAK") == 0)
        return;

    /* FSM: WaitForStartBit */
    if (s->fsm_state == J1708_FSM_WAIT_STARTBIT) {
        if (strcmp(cmd, "STARTBIT") == 0) {
            if (s->first_startbit_ss == 0) {
                s->first_startbit_ss = start_sample;
            } else {
                j1708_maybe_flush_message(di, s, start_sample);
            }
            s->fsm_state = J1708_FSM_WAIT_DATA;

            /* Check message break measurement after state transition */
            if (s->last_valid_msg_stopbit_es > 0 && s->bit_width > 0) {
                double inter_delay = (double)(start_sample - s->last_valid_msg_stopbit_es) / s->bit_width;
                if ((int)inter_delay > s->message_break) {
                    j1708_flush_message_break_measurement(di, s, start_sample);
                    s->last_valid_msg_stopbit_es = 0;
                    s->first_startbit_ss = start_sample;
                }
            }
        } else if (strcmp(cmd, "IDLE") == 0) {
            j1708_maybe_flush_message(di, s, start_sample);
        }
        return;
    }

    /* FSM: WaitForData */
    if (s->fsm_state == J1708_FSM_WAIT_DATA) {
        if (strcmp(cmd, "DATA") == 0) {
            if (n_fields < 2)
                return;
            uint8_t byte_val = fields[0].u8;
            if (s->data_len < MAX_MSG_LEN)
                s->data[s->data_len++] = byte_val;
            s->fsm_state = J1708_FSM_WAIT_STOPBIT;
        }
        return;
    }

    /* FSM: WaitForStopBit */
    if (s->fsm_state == J1708_FSM_WAIT_STOPBIT) {
        /* The C UART decoder sends "INVALID STOPBIT" instead of "STOPBIT"
         * when the stop bit is invalid. The Python UART decoder sends
         * "STOPBIT" even for invalid stop bits. Handle both here to
         * keep the FSM moving. */
        if (strcmp(cmd, "STOPBIT") == 0 || strcmp(cmd, "INVALID STOPBIT") == 0) {
            s->prev_stopbit_es = end_sample;
            s->fsm_state = J1708_FSM_WAIT_STARTBIT;
        }
        return;
    }
}

static void j1708_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(j1708_state)));
    }
    j1708_state *s = (j1708_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(j1708_state));
    s->fsm_state = J1708_FSM_WAIT_STARTBIT;
    s->message_break = 2;
}

static void j1708_start(struct srd_decoder_inst *di)
{
    j1708_state *s = (j1708_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "j1708");
    s->out_bin = c_reg_out(di, SRD_OUTPUT_BINARY, "j1708");
    s->message_break = (int)c_opt_int(di, "message_break", 2);
}

static void j1708_end(struct srd_decoder_inst *di)
{
    j1708_state *s = (j1708_state *)c_decoder_get_private(di);
    if (!s)
        return;
    /* Flush any pending message when the session ends.
     * This matches the Python decoder's behavior where IDLE events
     * at the end of the stream trigger maybe_flush_message(). */
    if (s->data_len > 0) {
        j1708_flush_message(di, s);
        j1708_message_ready(s);
    }
}

static void j1708_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void j1708_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder j1708_c_decoder = {
    .id = "j1708_c",
    .name = "J1708(C)",
    .longname = "J1708 (C)",
    .desc = "J1708 truck/bus serial communication protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = j1708_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = j1708_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = j1708_ann_rows,
    .inputs = j1708_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = j1708_binary,
    .num_binary = 3,
    .tags = j1708_tags,
    .num_tags = 1,
    .reset = j1708_reset,
    .start = j1708_start,
    .decode = j1708_decode,
    .end = j1708_end,
    .destroy = j1708_destroy,
    .decode_upper = j1708_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    j1708_options[0].def = g_variant_new_int64(2);
    GSList *vals = NULL;
    vals = g_slist_append(vals, g_variant_new_int64(2));
    vals = g_slist_append(vals, g_variant_new_int64(10));
    vals = g_slist_append(vals, g_variant_new_int64(12));
    j1708_options[0].values = vals;

    return &j1708_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}