/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2011-2014 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2025 C port (v4 API)
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

#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel indices — match Python optional_channels */
#define CH_RX 0
#define CH_TX 1

/* rxtx direction indices */
#define RX 0
#define TX 1

/* Used for protocols stackable with the uart and which require
 * several uniform idle periods, such as lin PD */
#define IDLE_NUM_WITHOUT_GROWTH 2

/* Annotation class indices — match Python Ann class */
enum uart_ann {
    RX_WARN = 0,
    TX_WARN,
    RX_DATA,
    TX_DATA,
    RX_START,
    TX_START,
    RX_PARITY_OK,
    TX_PARITY_OK,
    RX_PARITY_ERR,
    TX_PARITY_ERR,
    RX_STOP,
    TX_STOP,
    RX_DATA_BIT,
    TX_DATA_BIT,
    RX_BREAK,
    TX_BREAK,
    RX_PACKET,
    TX_PACKET,
    RX_SAMPLES,
    TX_SAMPLES,
    ATK_POINT,
    NUM_ANN,
};

/* Binary class indices — match Python Bin class */
enum uart_bin {
    BIN_RX = 0,
    BIN_TX,
    BIN_RXTX,
    BIN_RX_OK,
    BIN_TX_OK,
    BIN_RXTX_OK,
    NUM_BIN,
};

/* State machine states — match Python State class */
enum uart_state {
    WAIT_FOR_START_BIT,
    GET_START_BIT,
    GET_DATA_BITS,
    GET_PARITY_BIT,
    GET_STOP_BITS,
};

/* Parity types — match Python parity option values */
enum uart_parity {
    PARITY_NONE,
    PARITY_ODD,
    PARITY_EVEN,
    PARITY_ZERO,
    PARITY_ONE,
    PARITY_IGNORE,
};

/* State machine step: (state, rel_ss, rel_samplenum, rel_es) */
typedef struct {
    enum uart_state state;
    int rel_ss;
    int rel_samplenum;
    int rel_es;
} sm_step;

/* Per-data-bit entry for DATA protocol output */
typedef struct {
    int bit_val;
    uint64_t ss;
    uint64_t es;
} data_bit_entry;

/* Decoder state struct — C_DECODER_STATE auto-generates uart_s typedef.
 * We provide custom reset/destroy that properly handle the GArrays,
 * so the auto-generated ones are unused.
 * Suppress the unused-function warnings for the auto-generated versions. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
C_DECODER_STATE(uart, {
    enum uart_state state[2];
    int state_num[2];

    uint64_t samplerate;
    double baudrate;
    int data_bits;
    double stop_bits;
    enum uart_parity parity_type;
    int msb_first;        /* bit_order: 0=lsb-first, 1=msb-first */
    int format;           /* 0=hex, 1=ascii, 2=dec, 3=oct, 4=bin */
    int invert_rx;
    int invert_tx;
    int put_sample_points;
    int show_start_stop;
    int show_data_point;

    double bit_width;
    double half_bit_width;
    double bit_samplenum;  /* sample point offset within a bit */
    double sample_point_pct;

    /* State machine */
    sm_step *state_machine;
    int sm_len;
    int data_bounds[2]; /* [0]=rel start of data bits, [1]=rel end of data bits */

    /* Per-frame state [RX], [TX] */
    uint64_t frame_start[2];
    int frame_valid[2];
    int datavalue[2];
    int paritybit[2];
    GArray *databits[2];  /* GArray of data_bit_entry */
    GArray *stopbits[2];  /* GArray of int (signal values) */
    int databit_count[2];
    int stopbit_count[2];

    /* IDLE/BREAK detection */
    uint64_t break_start[2];
    int break_start_valid[2];
    uint64_t idle_start[2];
    int idle_start_valid[2];
    int idle_num[2];
    int idle_num_max;
    uint64_t break_min_samples;
    uint64_t frame_len_samples_int;
    double frame_len_samples;

    /* PACKET detection */
    GArray *packet_data[2];   /* GArray of int (data values) */
    int packet_valid[2];      /* whether all frames in packet are valid */
    uint64_t ss_packet[2];
    uint64_t es_packet[2];
    double packet_idle_us;
    uint64_t packet_idle_samples;
    int delim[2];
    int plen[2];
    int packet_enabled;

    /* Byte width for binary output */
    int bw;

    /* Output IDs */
    int out_ann;
    int out_python;
    int out_binary;

    /* Channel presence */
    int has_rx;
    int has_tx;
});
#pragma GCC diagnostic pop

/* ---- Custom reset: frees old GArrays, then uses calloc ---- */
static void uart_reset_impl(struct srd_decoder_inst *di)
{
    uart_s *old = (uart_s *)c_decoder_get_private(di);
    if (old) {
        for (int i = 0; i < 2; i++) {
            if (old->databits[i])
                g_array_free(old->databits[i], TRUE);
            if (old->stopbits[i])
                g_array_free(old->stopbits[i], TRUE);
            if (old->packet_data[i])
                g_array_free(old->packet_data[i], TRUE);
        }
        if (old->state_machine)
            g_free(old->state_machine);
        free(old);
        c_decoder_set_private(di, NULL);
    }

    uart_s *s = (uart_s *)calloc(1, sizeof(uart_s));
    c_decoder_set_private(di, s);
}

/* ---- Custom destroy: frees GArrays and struct ---- */
static void uart_destroy_impl(struct srd_decoder_inst *di)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    if (s) {
        for (int i = 0; i < 2; i++) {
            if (s->databits[i])
                g_array_free(s->databits[i], TRUE);
            if (s->stopbits[i])
                g_array_free(s->stopbits[i], TRUE);
            if (s->packet_data[i])
                g_array_free(s->packet_data[i], TRUE);
        }
        if (s->state_machine)
            g_free(s->state_machine);
        free(s);
        c_decoder_set_private(di, NULL);
    }
}

/* ---- Channel definitions — match Python optional_channels ---- */
static struct srd_channel uart_optional_channels[] = {
    {"rx", "RX", "UART receive line", 0, SRD_CHANNEL_SDATA, NULL},
    {"tx", "TX", "UART transmit line", 1, SRD_CHANNEL_SDATA, NULL},
};

/* ---- Annotation labels — match Python annotations tuple ---- */
static const char *uart_ann_labels[][3] = {
    {"", "rx-warning",       "RX warning"},
    {"", "tx-warning",       "TX warning"},
    {"", "rx-data",          "RX data"},
    {"", "tx-data",          "TX data"},
    {"", "rx-start",         "RX start bit"},
    {"", "tx-start",         "TX start bit"},
    {"", "rx-parity-ok",     "RX parity OK bit"},
    {"", "tx-parity-ok",     "TX parity OK bit"},
    {"", "rx-parity-err",    "RX parity error bit"},
    {"", "tx-parity-err",    "TX parity error bit"},
    {"", "rx-stop",          "RX stop bit"},
    {"", "tx-stop",          "TX stop bit"},
    {"", "rx-data-bit",      "RX data bit"},
    {"", "tx-data-bit",      "TX data bit"},
    {"", "rx-break",         "RX break"},
    {"", "tx-break",         "TX break"},
    {"", "rx-packet",        "RX packet"},
    {"", "tx-packet",        "TX packet"},
    {"", "rx-sample",        "RX sample"},
    {"", "tx-sample",        "TX sample"},
    {"", "atk-data-point",   "ATK Data point"},
};

/* ---- Annotation row class lists ---- */
static const int uart_row_rx_bits_classes[] = {RX_DATA_BIT, -1};
static const int uart_row_rx_samples_classes[] = {RX_SAMPLES, -1};
static const int uart_row_rx_data_classes[] = {RX_DATA, RX_START, RX_PARITY_OK, RX_PARITY_ERR, RX_STOP, -1};
static const int uart_row_rx_warn_classes[] = {RX_WARN, -1};
static const int uart_row_rx_break_classes[] = {RX_BREAK, -1};
static const int uart_row_rx_packet_classes[] = {RX_PACKET, -1};
static const int uart_row_tx_bits_classes[] = {TX_DATA_BIT, -1};
static const int uart_row_tx_samples_classes[] = {TX_SAMPLES, -1};
static const int uart_row_tx_data_classes[] = {TX_DATA, TX_START, TX_PARITY_OK, TX_PARITY_ERR, TX_STOP, -1};
static const int uart_row_tx_warn_classes[] = {TX_WARN, -1};
static const int uart_row_tx_break_classes[] = {TX_BREAK, -1};
static const int uart_row_tx_packet_classes[] = {TX_PACKET, -1};
static const int uart_row_atk_classes[] = {ATK_POINT, -1};

static const struct srd_c_ann_row uart_ann_rows[] = {
    {"rx-data-bits",  "RX bits",     uart_row_rx_bits_classes,     1},
    {"rx-samples",    "RX samples",  uart_row_rx_samples_classes,  1},
    {"rx-data-vals",  "RX data",     uart_row_rx_data_classes,     5},
    {"rx-warnings",   "RX warnings", uart_row_rx_warn_classes,     1},
    {"rx-breaks",     "RX breaks",   uart_row_rx_break_classes,    1},
    {"rx-packets",    "RX packets",  uart_row_rx_packet_classes,   1},
    {"tx-data-bits",  "TX bits",     uart_row_tx_bits_classes,     1},
    {"tx-samples",    "TX samples",  uart_row_tx_samples_classes,  1},
    {"tx-data-vals",  "TX data",     uart_row_tx_data_classes,     5},
    {"tx-warnings",   "TX warnings", uart_row_tx_warn_classes,     1},
    {"tx-breaks",     "TX breaks",   uart_row_tx_break_classes,    1},
    {"tx-packets",    "TX packets",  uart_row_tx_packet_classes,   1},
    {"atk-signs",     "ATK signs",   uart_row_atk_classes,         1},
};

/* ---- Binary output classes — match Python binary tuple ---- */
static const struct srd_decoder_binary uart_binary[] = {
    {BIN_RX,      "rx",      "RX dump"},
    {BIN_TX,      "tx",      "TX dump"},
    {BIN_RXTX,    "rxtx",    "RX/TX dump"},
    {BIN_RX_OK,   "rx-ok",   "RX dump (no error)"},
    {BIN_TX_OK,   "tx-ok",   "TX dump (no error)"},
    {BIN_RXTX_OK, "rxtx-ok", "RX/TX dump (no error)"},
};

/* ---- Options — match Python options tuple ---- */
static struct srd_decoder_option uart_options[] = {
    {"baudrate",          NULL, "Baud rate(\xe6\xb3\xa2\xe7\x89\xb9\xe7\x8e\x87)",          NULL, NULL},
    {"data_bits",         NULL, "Data bits(\xe6\x95\xb0\xe6\x8d\xae\xe4\xbd\x8d\xe6\x95\xb0)",   NULL, NULL},
    {"stop_bits",         NULL, "Stop bits(\xe5\x81\x9c\xe6\xad\xa2\xe4\xbd\x8d)",          NULL, NULL},
    {"parity",            NULL, "Parity(\xe6\xa0\xa1\xe9\xaa\x8c\xe4\xbd\x8d)",             NULL, NULL},
    {"bit_order",         NULL, "Bit order(\xe4\xbd\x8d\xe5\xba\x8f)",                    NULL, NULL},
    {"format",            NULL, "Data format(\xe6\x95\xb0\xe6\x8d\xae\xe6\xa0\xbc\xe5\xbc\x8f)",   NULL, NULL},
    {"invert_rx",         NULL, "Invert RX(\xe5\x8f\x8d\xe8\xbd\xacRX)",                 NULL, NULL},
    {"invert_tx",         NULL, "Invert TX(\xe5\x8f\x8d\xe8\xbd\xacTX)",                 NULL, NULL},
    {"show_data_point",   NULL, "Show data point(\xe6\x95\xb0\xe6\x8d\xae\xe7\x82\xb9\xe6\x98\xbe\xe7\xa4\xba)", NULL, NULL},
    {"sample_point",      NULL, "Sample point(\xe9\x87\x87\xe6\xa0\xb7\xe7\x82\xb9%)",       NULL, NULL},
    {"show_start_stop",   NULL, "Show start/stop bits(\xe6\x98\xbe\xe7\xa4\xba\xe8\xb5\xb7\xe5\xa7\x8b/\xe5\x81\x9c\xe6\xad\xa2\xe4\xbd\x8d)", NULL, NULL},
    {"packet_idle_us",    NULL, "Packet idle time (us)",                                     NULL, NULL},
    {"rx_packet_delim",   NULL, "RX packet delimiter (decimal)",                             NULL, NULL},
    {"tx_packet_delim",   NULL, "TX packet delimiter (decimal)",                             NULL, NULL},
    {"rx_packet_len",     NULL, "RX packet length",                                          NULL, NULL},
    {"tx_packet_len",     NULL, "TX packet length",                                          NULL, NULL},
};

static const char *uart_inputs[] = {"logic", NULL};
static const char *uart_outputs[] = {"uart", NULL};
static const char *uart_tags[] = {"Embedded/industrial", NULL};

/* ---- parity_ok — match Python parity_ok() ---- */
static int parity_ok(enum uart_parity ptype, int parity_bit, int data, int data_bits)
{
    if (ptype == PARITY_IGNORE)
        return 1;
    if (ptype == PARITY_ZERO)
        return (parity_bit == 0);
    if (ptype == PARITY_ONE)
        return (parity_bit == 1);

    /* Count number of 1 (high) bits in the data (and the parity bit itself!) */
    int ones = 0;
    for (int i = 0; i < data_bits; i++) {
        if (data & (1 << i))
            ones++;
    }
    ones += parity_bit;

    if (ptype == PARITY_ODD)
        return (ones % 2) == 1;
    if (ptype == PARITY_EVEN)
        return (ones % 2) == 0;

    return 1;
}

/* ---- get_bit_bounds — match Python get_bit_bounds() ---- */
static void get_bit_bounds(double bit_width, double bit_samplenum, int bit_num,
                           int half_bit, int *rel_ss, int *rel_samplenum, int *rel_es)
{
    double ss = bit_num * bit_width;
    if (!half_bit) {
        *rel_ss = (int)round(ss);
        *rel_samplenum = (int)round(ss + bit_samplenum);
        *rel_es = (int)round(ss + bit_width);
    } else {
        *rel_ss = (int)round(ss);
        *rel_samplenum = (int)round(ss + bit_samplenum * 0.5);
        *rel_es = (int)round(ss + bit_width * 0.5);
    }
}

/* ---- init_state_machine — match Python init_state_machine() ---- */
static void init_state_machine(uart_s *s)
{
    GArray *sm = g_array_new(FALSE, FALSE, sizeof(sm_step));

    /* Get START bit */
    {
        sm_step step = {WAIT_FOR_START_BIT, 0, 0, 0};
        g_array_append_val(sm, step);
    }
    {
        sm_step step;
        step.state = GET_START_BIT;
        get_bit_bounds(s->bit_width, s->bit_samplenum, 0, 0,
                       &step.rel_ss, &step.rel_samplenum, &step.rel_es);
        g_array_append_val(sm, step);
    }

    /* Get DATA bits */
    s->data_bounds[0] = g_array_index(sm, sm_step, sm->len - 1).rel_es;
    for (int data_bit_num = 0; data_bit_num < s->data_bits; data_bit_num++) {
        sm_step step;
        step.state = GET_DATA_BITS;
        get_bit_bounds(s->bit_width, s->bit_samplenum, data_bit_num + 1, 0,
                       &step.rel_ss, &step.rel_samplenum, &step.rel_es);
        g_array_append_val(sm, step);
    }
    s->data_bounds[1] = g_array_index(sm, sm_step, sm->len - 1).rel_es;

    /* Get PARITY bit */
    int frame_bit_num = 1 + s->data_bits;
    if (s->parity_type != PARITY_NONE) {
        sm_step step;
        step.state = GET_PARITY_BIT;
        get_bit_bounds(s->bit_width, s->bit_samplenum, frame_bit_num, 0,
                       &step.rel_ss, &step.rel_samplenum, &step.rel_es);
        g_array_append_val(sm, step);
        frame_bit_num++;
    }

    /* Get STOP bit(s) */
    double stop_bits_remaining = s->stop_bits;
    while (stop_bits_remaining > 0.4) {
        if (stop_bits_remaining > 0.9) {
            sm_step step;
            step.state = GET_STOP_BITS;
            get_bit_bounds(s->bit_width, s->bit_samplenum, frame_bit_num, 0,
                           &step.rel_ss, &step.rel_samplenum, &step.rel_es);
            g_array_append_val(sm, step);
            stop_bits_remaining -= 1;
            frame_bit_num++;
        } else if (stop_bits_remaining > 0.4) {
            sm_step step;
            step.state = GET_STOP_BITS;
            get_bit_bounds(s->bit_width, s->bit_samplenum, frame_bit_num, 1,
                           &step.rel_ss, &step.rel_samplenum, &step.rel_es);
            g_array_append_val(sm, step);
            stop_bits_remaining = 0;
        }
    }

    /* Looping state machine: append copy of first entry */
    sm_step first = g_array_index(sm, sm_step, 0);
    g_array_append_val(sm, first);

    /* Store in state */
    if (s->state_machine)
        g_free(s->state_machine);
    s->state_machine = (sm_step *)g_array_free(sm, FALSE);

    /* Init state machine positions */
    for (int rxtx = 0; rxtx < 2; rxtx++) {
        s->state[rxtx] = WAIT_FOR_START_BIT;
        s->state_num[rxtx] = 0;
    }
}

/* ---- format_value — match Python format_value() ---- */
static void format_value(uart_s *s, int v, char *out, int out_size)
{
    if (s->format == 1) { /* ascii */
        if (v >= 32 && v <= 126) {
            snprintf(out, out_size, "%c", v);
        } else {
            if (s->data_bits <= 8)
                snprintf(out, out_size, "[%02X]", v);
            else
                snprintf(out, out_size, "[%03X]", v);
        }
    } else if (s->format == 2) { /* dec */
        snprintf(out, out_size, "%d", v);
    } else if (s->format == 3) { /* oct */
        int digits = (s->data_bits + 3 - 1) / 3;
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%0%do", digits);
        snprintf(out, out_size, fmt, v);
    } else if (s->format == 4) { /* bin */
        int digits = s->data_bits;
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%0%db", digits);
        snprintf(out, out_size, fmt, v);
    } else { /* hex */
        int digits = (s->data_bits + 4 - 1) / 4;
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%%0%dX", digits);
        snprintf(out, out_size, fmt, v);
    }
}

/* ---- Forward declarations ---- */
static void advance_state_machine(struct srd_decoder_inst *di, int rxtx,
                                   int startbit_error, int stopbit_error);
static void handle_packet(struct srd_decoder_inst *di, int rxtx);
static void handle_packet_idle(struct srd_decoder_inst *di, int rxtx, uint64_t idle_end_sample);

/* ---- frame_bit_bounds — match Python frame_bit_bounds() ---- */
static void frame_bit_bounds(uart_s *s, int rxtx, uint64_t *ss, uint64_t *es)
{
    int state_num = s->state_num[rxtx];
    *ss = s->frame_start[rxtx] + s->state_machine[state_num].rel_ss;
    *es = s->frame_start[rxtx] + s->state_machine[state_num].rel_es;
}

/* ---- frame_data_bounds — match Python frame_data_bounds() ---- */
static void frame_data_bounds(uart_s *s, int rxtx, uint64_t *ss, uint64_t *es)
{
    *ss = s->frame_start[rxtx] + s->data_bounds[0];
    *es = s->frame_start[rxtx] + s->data_bounds[1];
}

/* ---- get_sample_point — match Python get_sample_point() ---- */
static uint64_t get_sample_point(uart_s *s, int rxtx)
{
    int state_num = s->state_num[rxtx];
    return s->frame_start[rxtx] + s->state_machine[state_num].rel_samplenum;
}

/* ---- handle_frame — match Python handle_frame() ---- */
static void handle_frame(struct srd_decoder_inst *di, int rxtx, uint64_t ss, uint64_t es)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    /* Protocol output: FRAME — match Python:
     *   self.putpse(ss, es, ['FRAME', rxtx, (self.datavalue[rxtx], self.frame_valid[rxtx])]) */
    c_proto(di, ss, es, s->out_python, "FRAME",
            C_I32(rxtx), C_U8(s->datavalue[rxtx]), C_I32(s->frame_valid[rxtx] ? 1 : 0), C_END);

    /* Binary output */
    int bw = s->bw;
    unsigned char bdata[4];
    for (int i = 0; i < bw; i++)
        bdata[i] = (unsigned char)((s->datavalue[rxtx] >> (8 * (bw - 1 - i))) & 0xFF);

    /* All data (regardless of errors) → rx/tx/rxtx */
    c_put_bin(di, ss, es, s->out_binary, BIN_RX + rxtx, bw, bdata);
    c_put_bin(di, ss, es, s->out_binary, BIN_RXTX, bw, bdata);

    /* Only valid data → rx-ok/tx-ok/rxtx-ok */
    if (s->frame_valid[rxtx]) {
        c_put_bin(di, ss, es, s->out_binary, BIN_RX_OK + rxtx, bw, bdata);
        c_put_bin(di, ss, es, s->out_binary, BIN_RXTX_OK, bw, bdata);
    }
}

/* ---- handle_packet — match Python handle_packet() ---- */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void handle_packet(struct srd_decoder_inst *di, int rxtx)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    if (s->packet_data[rxtx]->len == 0)
        return;

    /* Format packet data for annotation */
    char pkt_str[8192];
    int pos = 0;
    for (guint i = 0; i < s->packet_data[rxtx]->len && pos < (int)sizeof(pkt_str) - 16; i++) {
        int data = g_array_index(s->packet_data[rxtx], int, i);
        if (i > 0 && s->format != 1) /* not ascii */
            pkt_str[pos++] = ' ';

        char val_str[32];
        format_value(s, data, val_str, sizeof(val_str));
        int len = (int)strlen(val_str);
        if (pos + len >= (int)sizeof(pkt_str))
            break;
        memcpy(pkt_str + pos, val_str, len);
        pos += len;
    }
    pkt_str[pos] = '\0';

    uint64_t ss = s->ss_packet[rxtx];
    uint64_t es = s->es_packet[rxtx];
    c_put(di, ss, es, s->out_ann, RX_PACKET + rxtx, pkt_str);

    /* Protocol output: PACKET — match Python:
     *   self.putpse(ss, es, ['PACKET', rxtx, (self.packet_data[rxtx], self.packet_valid[rxtx])]) */
    c_proto(di, ss, es, s->out_python, "PACKET",
            C_I32(rxtx),
            C_U8(s->packet_valid[rxtx] ? 1 : 0), C_END);

    g_array_set_size(s->packet_data[rxtx], 0);
}
#pragma GCC diagnostic pop

/* ---- handle_packet_idle — match Python handle_packet_idle() ---- */
static void handle_packet_idle(struct srd_decoder_inst *di, int rxtx, uint64_t idle_end_sample)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    if (s->packet_data[rxtx]->len == 0 || s->packet_idle_samples == 0)
        return;
    if (idle_end_sample >= s->es_packet[rxtx] + s->packet_idle_samples)
        handle_packet(di, rxtx);
}

/* ---- get_packet_data — match Python get_packet_data() ---- */
static void get_packet_data(struct srd_decoder_inst *di, int rxtx, uint64_t frame_end_sample)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    if (!s->packet_enabled)
        return;
    if (s->delim[rxtx] < 0 && s->plen[rxtx] < 0 && s->packet_idle_samples == 0)
        return;

    if (s->packet_data[rxtx]->len == 0) {
        s->ss_packet[rxtx] = s->frame_start[rxtx];
        s->packet_valid[rxtx] = s->frame_valid[rxtx];
    } else {
        if (!s->frame_valid[rxtx])
            s->packet_valid[rxtx] = 0;
    }

    g_array_append_val(s->packet_data[rxtx], s->datavalue[rxtx]);
    s->es_packet[rxtx] = frame_end_sample;

    if (s->delim[rxtx] >= 0 && s->datavalue[rxtx] == s->delim[rxtx]) {
        handle_packet(di, rxtx);
    } else if (s->plen[rxtx] > 0 && (int)s->packet_data[rxtx]->len >= s->plen[rxtx]) {
        handle_packet(di, rxtx);
    }
}

/* ---- reset_data_receive — match Python reset_data_receive() ---- */
static void reset_data_receive(uart_s *s, int rxtx)
{
    g_array_set_size(s->databits[rxtx], 0);
    s->datavalue[rxtx] = 0;
    s->paritybit[rxtx] = -1;
    g_array_set_size(s->stopbits[rxtx], 0);
}

/* ---- handle_data — match Python handle_data() ---- */
static void handle_data(struct srd_decoder_inst *di, int rxtx)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    /* Convert accumulated data bits to a data value */
    int b = 0;
    for (guint i = 0; i < s->databits[rxtx]->len; i++) {
        data_bit_entry *e = &g_array_index(s->databits[rxtx], data_bit_entry, i);
        if (s->msb_first)
            b |= (e->bit_val << (s->data_bits - 1 - (int)i));
        else
            b |= (e->bit_val << (int)i);
    }
    s->datavalue[rxtx] = b;

    uint64_t ss_data, es_data;
    frame_data_bounds(s, rxtx, &ss_data, &es_data);

    /* Protocol output: DATA — match Python:
     *   self.putpse(ss_data, es_data, ['DATA', rxtx, (self.datavalue[rxtx], self.databits[rxtx])])
     * C convention: data byte first, rxtx second (matches upper-layer decoder expectations) */
    c_proto(di, ss_data, es_data, s->out_python, "DATA",
            C_U8(b), C_I32(rxtx), C_END);

    /* Annotation */
    char val_str[32];
    format_value(s, b, val_str, sizeof(val_str));
    c_put(di, ss_data, es_data, s->out_ann, RX_DATA + rxtx, val_str);

    g_array_set_size(s->databits[rxtx], 0);
}

/* ---- handle_idle — match Python handle_idle() ---- */
static void handle_idle(struct srd_decoder_inst *di, int rxtx, uint64_t ss, uint64_t es)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    /* Match Python: self.putpse(ss, es, ['IDLE', rxtx, 0]) */
    c_proto(di, ss, es, s->out_python, "IDLE", C_I32(rxtx), C_U8(0), C_END);
    s->idle_num[rxtx]++;
    handle_packet_idle(di, rxtx, es);
}

/* ---- handle_break — match Python handle_break() ---- */
static void handle_break(struct srd_decoder_inst *di, int rxtx, uint64_t ss, uint64_t es)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    /* Match Python: self.putpse(ss, es, ['BREAK', rxtx, 0]) */
    c_proto(di, ss, es, s->out_python, "BREAK", C_I32(rxtx), C_U8(0), C_END);
    c_put(di, ss, es, s->out_ann, RX_BREAK + rxtx,
          "Break condition", "Break", "Brk", "B");
}

/* ---- wait_for_start_bit — match Python wait_for_start_bit() ---- */
static void wait_for_start_bit(struct srd_decoder_inst *di, int rxtx, int signal)
{
    (void)signal;
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    s->frame_start[rxtx] = di_samplenum(di);
    s->frame_valid[rxtx] = 1;
    advance_state_machine(di, rxtx, 0, 0);
}

/* ---- get_start_bit — match Python get_start_bit() ---- */
static void get_start_bit(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    uint64_t frame_ss, frame_es;
    frame_bit_bounds(s, rxtx, &frame_ss, &frame_es);

    if (signal != 0) {
        /* Invalid start bit */
        frame_es = di_samplenum(di);
        c_proto(di, frame_ss, frame_es, s->out_python, "INVALID STARTBIT",
                C_I32(rxtx), C_U8(signal), C_END);
        c_put(di, frame_ss, frame_es, s->out_ann, RX_WARN + rxtx,
              "Start bit error", "Start err", "SE");
        s->frame_valid[rxtx] = 0;
        handle_frame(di, rxtx, frame_ss, frame_es);
        advance_state_machine(di, rxtx, 1, 0);
        return;
    }

    c_proto(di, frame_ss, frame_es, s->out_python, "STARTBIT",
            C_I32(rxtx), C_U8(signal), C_END);
    if (s->show_start_stop)
        c_put(di, frame_ss, frame_es, s->out_ann, RX_START + rxtx,
              "Start bit", "Start", "S");

    advance_state_machine(di, rxtx, 0, 0);
    reset_data_receive(s, rxtx);
}

/* ---- get_data_bits — match Python get_data_bits() ---- */
static void get_data_bits(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    uint64_t ss, es;
    
    frame_bit_bounds(s, rxtx, &ss, &es);

    char bit_str[16];
    snprintf(bit_str, sizeof(bit_str), "%d", signal);
    c_put(di, ss, es, s->out_ann, RX_DATA_BIT + rxtx, bit_str);

    if (s->show_data_point) {
        uint64_t center = ss + (uint64_t)(s->bit_width / 2);
        char rxtx_str[4];
        snprintf(rxtx_str, sizeof(rxtx_str), "%d", rxtx);
        c_put(di, center, center, s->out_ann, ATK_POINT, rxtx_str);
    }

    data_bit_entry entry;
    entry.bit_val = signal;
    entry.ss = ss;
    entry.es = es;
    g_array_append_val(s->databits[rxtx], entry);

    if ((int)s->databits[rxtx]->len == s->data_bits)
        handle_data(di, rxtx);

    advance_state_machine(di, rxtx, 0, 0);
}

/* ---- get_parity_bit — match Python get_parity_bit() ---- */
static void get_parity_bit(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    s->paritybit[rxtx] = signal;
    uint64_t ss, es;
    
    frame_bit_bounds(s, rxtx, &ss, &es);

    if (parity_ok(s->parity_type, s->paritybit[rxtx], s->datavalue[rxtx], s->data_bits)) {
        c_proto(di, ss, es, s->out_python, "PARITYBIT",
                C_I32(rxtx), C_U8(s->paritybit[rxtx]), C_END);
        c_put(di, ss, es, s->out_ann, RX_PARITY_OK + rxtx,
              "Parity bit", "Parity", "P");
    } else {
        c_proto(di, ss, es, s->out_python, "PARITY ERROR",
                C_I32(rxtx),
                C_U8(!signal ? 1 : 0),
                C_U8(signal ? 1 : 0), C_END);
        c_put(di, ss, es, s->out_ann, RX_PARITY_ERR + rxtx,
              "Parity error", "Parity err", "PE");
        s->frame_valid[rxtx] = 0;
    }

    advance_state_machine(di, rxtx, 0, 0);
}

/* ---- get_stop_bits — match Python get_stop_bits() ---- */
static void get_stop_bits(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    g_array_append_val(s->stopbits[rxtx], signal);
    uint64_t ss, es;
    
    frame_bit_bounds(s, rxtx, &ss, &es);

    int stopbit_error = (signal != 1);
    if (stopbit_error) {
        es = di_samplenum(di);
        c_proto(di, ss, es, s->out_python, "INVALID STOPBIT",
                C_I32(rxtx), C_U8(signal), C_END);
        c_put(di, ss, es, s->out_ann, RX_WARN + rxtx,
              "Stop bit error", "Stop err", "TE");
        s->frame_valid[rxtx] = 0;
    } else if (s->show_start_stop) {
        c_put(di, ss, es, s->out_ann, RX_STOP + rxtx,
              "Stop bit", "Stop", "T");
    }
    c_proto(di, ss, es, s->out_python, "STOPBIT",
            C_I32(rxtx), C_U8(signal), C_END);

    advance_state_machine(di, rxtx, 0, stopbit_error);
}

/* ---- advance_state_machine — match Python advance_state_machine() ---- */
static void advance_state_machine(struct srd_decoder_inst *di, int rxtx,
                                   int startbit_error, int stopbit_error)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    if (startbit_error || stopbit_error) {
        s->state_num[rxtx] = 0;
        s->state[rxtx] = WAIT_FOR_START_BIT;

        if (startbit_error) {
            s->idle_start[rxtx] = di_samplenum(di);
            s->idle_start_valid[rxtx] = 1;
            return;
        }
    } else {
        s->state_num[rxtx] += 1;
        s->state[rxtx] = s->state_machine[s->state_num[rxtx]].state;
    }

    if (s->state[rxtx] == WAIT_FOR_START_BIT) {
        s->state_num[rxtx] = 0;
        uint64_t frame_ss = s->frame_start[rxtx];
        uint64_t frame_es;
        if (!stopbit_error) {
            frame_es = frame_ss + s->frame_len_samples_int;
            s->idle_start[rxtx] = frame_es;
            s->idle_start_valid[rxtx] = 1;
        } else {
            frame_es = di_samplenum(di);
            s->idle_start_valid[rxtx] = 0;
        }
        handle_frame(di, rxtx, frame_ss, frame_es);
        get_packet_data(di, rxtx, frame_es);
    }
}

/* ---- inspect_sample — match Python inspect_sample() ---- */
static void inspect_sample(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    switch (s->state[rxtx]) {
    case WAIT_FOR_START_BIT:
        handle_packet_idle(di, rxtx, di_samplenum(di));
        wait_for_start_bit(di, rxtx, signal);
        break;
    case GET_START_BIT:
        get_start_bit(di, rxtx, signal);
        break;
    case GET_DATA_BITS:
        get_data_bits(di, rxtx, signal);
        break;
    case GET_PARITY_BIT:
        get_parity_bit(di, rxtx, signal);
        break;
    case GET_STOP_BITS:
        get_stop_bits(di, rxtx, signal);
        break;
    }

    if (s->put_sample_points && s->state[rxtx] != WAIT_FOR_START_BIT) {
        char sig_str[4];
        snprintf(sig_str, sizeof(sig_str), "%d", signal);
        c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann,
              RX_SAMPLES + rxtx, sig_str);
    }
}

/* ---- inspect_edge — match Python inspect_edge() ---- */
static void inspect_edge(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    if (!signal) {
        /* Signal went low. Start another interval. */
        s->break_start[rxtx] = di_samplenum(di);
        s->break_start_valid[rxtx] = 1;
        return;
    }
    /* Signal went high. Was there an extended period with low signal? */
    if (!s->break_start_valid[rxtx])
        return;
    uint64_t diff = di_samplenum(di) - s->break_start[rxtx];
    if (diff >= s->break_min_samples) {
        uint64_t ss = s->frame_start[rxtx];
        uint64_t es = di_samplenum(di);
        handle_break(di, rxtx, ss, es);
    }
    s->break_start_valid[rxtx] = 0;
}

/* ---- inspect_idle — match Python inspect_idle() ---- */
static void inspect_idle(struct srd_decoder_inst *di, int rxtx, int signal)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    if (!signal) {
        /* Low input, cease inspection. */
        s->idle_start_valid[rxtx] = 0;
        s->idle_num[rxtx] = 0;
        return;
    }
    /* High input, either just reached, or still stable. */
    if (!s->idle_start_valid[rxtx]) {
        s->idle_start[rxtx] = di_samplenum(di);
        s->idle_start_valid[rxtx] = 1;
    }
    uint64_t diff = di_samplenum(di) - s->idle_start[rxtx];
    if (diff < s->frame_len_samples_int)
        return;
    uint64_t ss = s->idle_start[rxtx];
    uint64_t es = di_samplenum(di);
    handle_idle(di, rxtx, ss, es);
    handle_packet_idle(di, rxtx, es);
    s->idle_start[rxtx] = es;
}

/* ---- start callback — match Python start() ---- */
static void uart_start(struct srd_decoder_inst *di)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    s->out_ann    = c_reg_out(di, SRD_OUTPUT_ANN, "uart");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "uart");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "uart");

    s->baudrate = (double)c_opt_int(di, "baudrate", 115200);
    s->data_bits = (int)c_opt_int(di, "data_bits", 8);
    s->stop_bits = c_opt_dbl(di, "stop_bits", 1.0);

    const char *parity_str = c_opt_str(di, "parity", "none");
    if (parity_str && strcmp(parity_str, "odd") == 0)
        s->parity_type = PARITY_ODD;
    else if (parity_str && strcmp(parity_str, "even") == 0)
        s->parity_type = PARITY_EVEN;
    else if (parity_str && strcmp(parity_str, "zero") == 0)
        s->parity_type = PARITY_ZERO;
    else if (parity_str && strcmp(parity_str, "one") == 0)
        s->parity_type = PARITY_ONE;
    else if (parity_str && strcmp(parity_str, "ignore") == 0)
        s->parity_type = PARITY_IGNORE;
    else
        s->parity_type = PARITY_NONE;

    s->msb_first = (strcmp(c_opt_str(di, "bit_order", "lsb-first"), "msb-first") == 0);

    const char *format_str = c_opt_str(di, "format", "hex");
    if (format_str && strcmp(format_str, "ascii") == 0)
        s->format = 1;
    else if (format_str && strcmp(format_str, "dec") == 0)
        s->format = 2;
    else if (format_str && strcmp(format_str, "oct") == 0)
        s->format = 3;
    else if (format_str && strcmp(format_str, "bin") == 0)
        s->format = 4;
    else
        s->format = 0; /* hex */

    s->invert_rx = c_opt_bool(di, "invert_rx", 0);
    s->invert_tx = c_opt_bool(di, "invert_tx", 0);
    s->put_sample_points = c_opt_bool(di, "put_sample_points", 0);
    s->show_start_stop = c_opt_bool(di, "show_start_stop", 1);
    s->show_data_point = c_opt_bool(di, "show_data_point", 1);
    s->sample_point_pct = c_opt_dbl(di, "sample_point", 50.0);

    s->has_rx = c_has_ch(di, CH_RX);
    s->has_tx = c_has_ch(di, CH_TX);

    s->bw = (s->data_bits + 7) / 8;

    /* Allocate GArrays */
    for (int i = 0; i < 2; i++) {
        if (!s->databits[i])
            s->databits[i] = g_array_new(FALSE, FALSE, sizeof(data_bit_entry));
        if (!s->stopbits[i])
            s->stopbits[i] = g_array_new(FALSE, FALSE, sizeof(int));
        if (!s->packet_data[i])
            s->packet_data[i] = g_array_new(FALSE, FALSE, sizeof(int));
    }

    /* Packet detection options */
    s->delim[RX] = (int)c_opt_int(di, "rx_packet_delim", -1);
    s->delim[TX] = (int)c_opt_int(di, "tx_packet_delim", -1);
    s->plen[RX] = (int)c_opt_int(di, "rx_packet_len", -1);
    s->plen[TX] = (int)c_opt_int(di, "tx_packet_len", -1);

    s->frame_valid[RX] = 1;
    s->frame_valid[TX] = 1;

    s->samplerate = c_samplerate(di);

    /* If metadata() was called before start() (when baudrate was still 0),
     * bit_width won't have been set. Compute it now. */
    if (s->samplerate > 0 && s->baudrate > 0 && s->bit_width == 0) {
        s->bit_width = (double)s->samplerate / s->baudrate;
        s->half_bit_width = s->bit_width * 0.5;
        s->bit_samplenum = s->bit_width * s->sample_point_pct * 0.01;

        double frame_samples = 1.0 + s->data_bits
            + (s->parity_type != PARITY_NONE ? 1.0 : 0.0)
            + s->stop_bits;
        frame_samples *= s->bit_width;
        s->frame_len_samples = frame_samples;
        s->frame_len_samples_int = (uint64_t)round(frame_samples);
        s->break_min_samples = s->frame_len_samples_int + 1;

        int maxn = 0;
        while (s->frame_len_samples_int * (1ULL << maxn) < s->samplerate)
            maxn++;
        maxn += IDLE_NUM_WITHOUT_GROWTH - 1;
        s->idle_num_max = maxn;

        init_state_machine(s);

        s->packet_idle_us = c_opt_dbl(di, "packet_idle_us", -1);
        if (s->packet_idle_us > 0) {
            s->packet_idle_samples = (uint64_t)round(s->packet_idle_us * 1e-6 * s->samplerate);
            if (s->packet_idle_samples < 1)
                s->packet_idle_samples = 1;
        } else {
            s->packet_idle_samples = 0;
        }

        s->packet_enabled = (s->packet_idle_samples > 0 ||
                              s->delim[RX] >= 0 || s->delim[TX] >= 0 ||
                              s->plen[RX] > 0 || s->plen[TX] > 0);
    }
}

/* ---- metadata callback — match Python metadata() ---- */
static void uart_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->samplerate > 0 && s->baudrate > 0) {
            s->bit_width = (double)s->samplerate / s->baudrate;
            s->half_bit_width = s->bit_width * 0.5;

            /* Accept a position in the range of 1-99% of the full bit width.
             * Assume 50% for invalid input specs for backwards compatibility. */
            double perc = s->sample_point_pct;
            if (perc < 1.0 || perc > 99.0)
                perc = 50.0;
            s->bit_samplenum = s->bit_width * perc * 0.01;

            /* Determine the number of samples for a complete frame's time span. */
            double frame_samples = 1.0; /* START */
            frame_samples += s->data_bits;
            frame_samples += (s->parity_type != PARITY_NONE) ? 1.0 : 0.0;
            frame_samples += s->stop_bits;
            frame_samples *= s->bit_width;
            s->frame_len_samples = frame_samples;
            s->frame_len_samples_int = (uint64_t)round(frame_samples);
            s->break_min_samples = s->frame_len_samples_int + 1;

            /* Calculate idle_num_max: exponential backoff cap for IDLE detection */
            int maxn = 0;
            while (s->frame_len_samples_int * (1ULL << maxn) < s->samplerate)
                maxn++;
            maxn += IDLE_NUM_WITHOUT_GROWTH - 1;
            s->idle_num_max = maxn;

            /* Build state machine */
            init_state_machine(s);

            /* Init packet idle */
            s->packet_idle_us = c_opt_dbl(di, "packet_idle_us", -1);
            if (s->packet_idle_us > 0) {
                s->packet_idle_samples = (uint64_t)round(s->packet_idle_us * 1e-6 * s->samplerate);
                if (s->packet_idle_samples < 1)
                    s->packet_idle_samples = 1;
            } else {
                s->packet_idle_samples = 0;
            }

            s->packet_enabled = (s->packet_idle_samples > 0 ||
                                  s->delim[RX] >= 0 || s->delim[TX] >= 0 ||
                                  s->plen[RX] > 0 || s->plen[TX] > 0);
        }
    }
}

/* ---- Helper: allocate and init an srd_term ---- */
static struct srd_term *make_term(int type, int channel, uint64_t skip_count)
{
    struct srd_term *t = g_malloc0(sizeof(struct srd_term));
    t->type = type;
    t->channel = channel;
    t->num_samples_to_skip = skip_count;
    t->num_samples_already_skipped = 0;
    return t;
}

/* ---- decode callback — main decode loop, match Python decode() ---- */
static void uart_decode(struct srd_decoder_inst *di)
{
    uart_s *s = (uart_s *)c_decoder_get_private(di);

    if (s->samplerate == 0 || s->baudrate == 0 || s->bit_width == 0)
        return;

    int has_pin[2] = {s->has_rx, s->has_tx};
    if (!has_pin[RX] && !has_pin[TX])
        return;

    int inv[2] = {s->invert_rx, s->invert_tx};

    /* Emit ATK color styling annotation at start, matching Python:
     * self.put(self.samplenum, self.samplenum, self.out_ann, [20,["color:#fbca47"]]) */
    c_put(di, 0, 0, s->out_ann, ATK_POINT, "color:#fbca47");

    while (1) {
        /* Build wait conditions dynamically — same pattern as Python decode().
         *
         * For each rxtx line that has a pin, we add up to 3 condition groups:
         *   1. Data condition: falling/rising edge (start bit) or skip (sample point)
         *   2. Edge condition: either edge (for BREAK detection)
         *   3. Idle condition: skip (for IDLE detection)
         *
         * The Python code uses self.wait(conds) where conds is a flat list
         * of condition dicts. Each dict is an OR group.
         * We track which condition matched using di_matched(di).
         *
         * We build the condition list as GSList of OR groups (each OR group
         * is a GSList of srd_term AND conditions), then call c_decoder_wait().
         * This is equivalent to c_wait() but supports dynamic SKIP counts.
         */
        int cond_data_idx[2] = {-1, -1};
        int cond_edge_idx[2] = {-1, -1};
        int cond_idle_idx[2] = {-1, -1};
        int cond_count = 0;

        GSList *or_groups = NULL;
        int has_cond = 0;

        for (int rxtx = 0; rxtx < 2; rxtx++) {
            if (!has_pin[rxtx])
                continue;

            /* Data condition */
            cond_data_idx[rxtx] = cond_count;
            {
                GSList *and_group = NULL;
                if (s->state[rxtx] == WAIT_FOR_START_BIT) {
                    if (inv[rxtx])
                        and_group = g_slist_append(and_group,
                            make_term(SRD_TERM_RISING_EDGE, rxtx, 0));
                    else
                        and_group = g_slist_append(and_group,
                            make_term(SRD_TERM_FALLING_EDGE, rxtx, 0));
                } else {
                    uint64_t want_samplenum = get_sample_point(s, rxtx);
                    uint64_t cur = di_samplenum(di);
                    uint64_t skip = (want_samplenum > cur) ? (want_samplenum - cur) : 1;
                    and_group = g_slist_append(and_group,
                        make_term(SRD_TERM_SKIP, -1, skip));
                }
                or_groups = g_slist_append(or_groups, and_group);
            }
            cond_count++;
            has_cond = 1;

            /* Edge condition for BREAK detection */
            cond_edge_idx[rxtx] = cond_count;
            {
                GSList *and_group = NULL;
                and_group = g_slist_append(and_group,
                    make_term(SRD_TERM_EITHER_EDGE, rxtx, 0));
                or_groups = g_slist_append(or_groups, and_group);
            }
            cond_count++;
            has_cond = 1;

            /* Idle condition */
            if (s->idle_start_valid[rxtx] && s->frame_len_samples_int > 0) {
                uint64_t idle_wait;
                if (s->idle_num[rxtx] < IDLE_NUM_WITHOUT_GROWTH)
                    idle_wait = s->frame_len_samples_int;
                else if (s->idle_num[rxtx] >= s->idle_num_max)
                    idle_wait = s->samplerate;
                else {
                    int double_num = s->idle_num[rxtx] - (IDLE_NUM_WITHOUT_GROWTH - 1);
                    idle_wait = s->frame_len_samples_int * (1ULL << double_num);
                }
                uint64_t end_of_wait = s->idle_start[rxtx] + idle_wait;
                uint64_t cur = di_samplenum(di);
                if (end_of_wait > cur) {
                    cond_idle_idx[rxtx] = cond_count;
                    {
                        GSList *and_group = NULL;
                        and_group = g_slist_append(and_group,
                            make_term(SRD_TERM_SKIP, -1, end_of_wait - cur));
                        or_groups = g_slist_append(or_groups, and_group);
                    }
                    cond_count++;
                    has_cond = 1;
                }
            }
        }

        if (!has_cond) {
            GSList *and_group = NULL;
            and_group = g_slist_append(and_group, make_term(SRD_TERM_SKIP, -1, 1));
            or_groups = g_slist_append(or_groups, and_group);
        }

        int ret = c_decoder_wait(di, or_groups, NULL, NULL);
        /* NOTE: do NOT free or_groups here — c_decoder_wait_impl takes ownership
         * of the condition list and will free it on the next call. */

        if (ret != SRD_OK)
            return;

        /* Use di_matched() to determine which conditions matched */
        uint64_t matched = di_matched(di);

        /* Process each channel based on which condition matched */
        for (int rxtx = 0; rxtx < 2; rxtx++) {
            if (!has_pin[rxtx])
                continue;

            int signal = c_pin(di, rxtx);
            if (inv[rxtx])
                signal = signal ? 0 : 1;

            if (cond_data_idx[rxtx] >= 0 && (matched & (1ULL << cond_data_idx[rxtx])))
                inspect_sample(di, rxtx, signal);
            if (cond_edge_idx[rxtx] >= 0 && (matched & (1ULL << cond_edge_idx[rxtx]))) {
                inspect_edge(di, rxtx, signal);
                inspect_idle(di, rxtx, signal);
            }
            if (cond_idle_idx[rxtx] >= 0 && (matched & (1ULL << cond_idle_idx[rxtx])))
                inspect_idle(di, rxtx, signal);
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder uart_c_def = {
    .id = "uart_c",
    .name = "UART(C)",
    .longname = "Universal Asynchronous Receiver/Transmitter (C)",
    .desc = "UART protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = uart_optional_channels,
    .num_optional_channels = 2,
    .options = uart_options,
    .num_options = 16,
    .num_annotations = NUM_ANN,
    .ann_labels = uart_ann_labels,
    .num_annotation_rows = 13,
    .annotation_rows = uart_ann_rows,
    .inputs = uart_inputs,
    .num_inputs = 1,
    .outputs = uart_outputs,
    .num_outputs = 1,
    .binary = uart_binary,
    .num_binary = NUM_BIN,
    .tags = uart_tags,
    .num_tags = 1,
    .state_size = sizeof(uart_s),
    .reset = uart_reset_impl,
    .start = uart_start,
    .decode = uart_decode,
    .end = NULL,
    .metadata = uart_metadata,
    .destroy = uart_destroy_impl,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* Set option defaults */
    uart_options[0].def = g_variant_new_int64(115200);
    uart_options[1].def = g_variant_new_int64(8);
    uart_options[2].def = g_variant_new_double(1.0);
    uart_options[3].def = g_variant_new_string("none");
    uart_options[4].def = g_variant_new_string("lsb-first");
    uart_options[5].def = g_variant_new_string("hex");
    uart_options[6].def = g_variant_new_string("no");
    uart_options[7].def = g_variant_new_string("no");
    uart_options[8].def = g_variant_new_string("yes");
    uart_options[9].def = g_variant_new_double(50.0);
    uart_options[10].def = g_variant_new_string("yes");
    uart_options[11].def = g_variant_new_double(-1);
    uart_options[12].def = g_variant_new_int64(-1);
    uart_options[13].def = g_variant_new_int64(-1);
    uart_options[14].def = g_variant_new_int64(-1);
    uart_options[15].def = g_variant_new_int64(-1);

    /* Set option value lists */
    GSList *parity_vals = NULL;
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("none"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("odd"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("even"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("zero"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("one"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("ignore"));
    uart_options[3].values = parity_vals;

    GSList *data_bits_vals = NULL;
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(5));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(6));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(7));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(8));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(9));
    uart_options[1].values = data_bits_vals;

    GSList *stop_bits_vals = NULL;
    stop_bits_vals = g_slist_append(stop_bits_vals, g_variant_new_double(0.0));
    stop_bits_vals = g_slist_append(stop_bits_vals, g_variant_new_double(0.5));
    stop_bits_vals = g_slist_append(stop_bits_vals, g_variant_new_double(1.0));
    stop_bits_vals = g_slist_append(stop_bits_vals, g_variant_new_double(1.5));
    stop_bits_vals = g_slist_append(stop_bits_vals, g_variant_new_double(2.0));
    uart_options[2].values = stop_bits_vals;

    GSList *bitorder_vals = NULL;
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb-first"));
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb-first"));
    uart_options[4].values = bitorder_vals;

    GSList *format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("hex"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("ascii"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("dec"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("oct"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("bin"));
    uart_options[5].values = format_vals;

    GSList *yn_vals = NULL;
    yn_vals = g_slist_append(yn_vals, g_variant_new_string("yes"));
    yn_vals = g_slist_append(yn_vals, g_variant_new_string("no"));
    uart_options[6].values = yn_vals;
    uart_options[7].values = yn_vals;

    GSList *sdp_vals = NULL;
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("yes"));
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("no"));
    uart_options[8].values = sdp_vals;

    GSList *ss_vals = NULL;
    ss_vals = g_slist_append(ss_vals, g_variant_new_string("yes"));
    ss_vals = g_slist_append(ss_vals, g_variant_new_string("no"));
    uart_options[10].values = ss_vals;

    return &uart_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}