/*
 * UART-fast C decoder
 * Ported from Python uart-fast decoder
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX 0
#define TX 1

enum uart_fast_ann {
    ANN_RX_WARN = 0,
    ANN_TX_WARN,
    ANN_RX_DATA,
    ANN_TX_DATA,
    ANN_RX_START,
    ANN_TX_START,
    ANN_RX_PARITY_OK,
    ANN_TX_PARITY_OK,
    ANN_RX_PARITY_ERR,
    ANN_TX_PARITY_ERR,
    ANN_RX_STOP,
    ANN_TX_STOP,
    ANN_ATK_POINT,
    NUM_ANN,
};

enum uart_fast_bin {
    BIN_RX = 0,
    BIN_TX,
    BIN_RXTX,
    BIN_RX_OK,
    BIN_TX_OK,
    BIN_RXTX_OK,
    NUM_BIN,
};

enum uart_fast_state {
    STATE_WAIT_FOR_START_BIT = 0,
    STATE_GET_START_BIT,
    STATE_GET_DATA_BITS,
    STATE_GET_PARITY_BIT,
    STATE_GET_STOP_BITS,
};

enum uart_fast_parity {
    PARITY_NONE = 0,
    PARITY_ODD,
    PARITY_EVEN,
    PARITY_ZERO,
    PARITY_ONE,
    PARITY_IGNORE,
};

typedef struct {
    int state[2];
    int state_num[2];
    uint64_t frame_start[2];
    int frame_valid[2];
    int packet_valid[2];
    uint64_t datavalue[2];
    int paritybit[2];
    int databit_count[2];
    int stopbit_count[2];

    uint64_t samplerate;
    uint64_t baudrate;
    int data_bits;
    double stop_bits;
    enum uart_fast_parity parity_type;
    int bit_order_msb;
    int format;
    int invert;
    int64_t packet_idle_us;
    int show_data_point;

    double bit_width;
    double half_bit_width;
    double bit_samplenum;

    double frame_len_samples;
    uint64_t frame_len_samples_int;
    uint64_t break_min_samples;

    int has_rx;
    int has_tx;

    /* Break detection */
    uint64_t break_start[2];
    int break_start_valid[2];

    /* Packet idle */
    uint64_t packet_idle_samples;
    uint64_t ss_packet[2];
    uint64_t es_packet[2];
    uint64_t packet_data[2][4096];
    int packet_data_cnt[2];
    int packet_idle_valid[2];

    int out_ann;
    int out_python;
    int out_binary;
    int bw;
} uart_fast_state;

static struct srd_channel uart_fast_optional_channels[] = {
    { "rx", "RX", "UART receive line", 0, SRD_CHANNEL_SDATA, "dec_uart_fast_opt_chan_rx" },
    { "tx", "TX", "UART transmit line", 1, SRD_CHANNEL_SDATA, "dec_uart_fast_opt_chan_tx" },
};

static struct srd_decoder_option uart_fast_options[] = {
    { "baudrate", NULL, "Baud rate", NULL, NULL },
    { "data_bits", NULL, "Data bits", NULL, NULL },
    { "parity", NULL, "Parity", NULL, NULL },
    { "stop_bits", NULL, "Stop bits", NULL, NULL },
    { "bit_order", NULL, "Bit order", NULL, NULL },
    { "format", NULL, "Data format", NULL, NULL },
    { "invert", NULL, "Invert RX/TX", NULL, NULL },
    { "packet_idle_us", NULL, "Packet delimit by idle time, us", NULL, NULL },
    { "show_data_point", NULL, "Show data point", NULL, NULL },
};

static const char *uart_fast_ann_labels[][3] = {
    { "", "rx-warning", "RX warning" },
    { "", "tx-warning", "TX warning" },
    { "", "rx-data", "RX data" },
    { "", "tx-data", "TX data" },
    { "", "rx-start", "RX start bit" },
    { "", "tx-start", "TX start bit" },
    { "", "rx-parity-ok", "RX parity OK bit" },
    { "", "tx-parity-ok", "TX parity OK bit" },
    { "", "rx-parity-err", "RX parity error bit" },
    { "", "tx-parity-err", "TX parity error bit" },
    { "", "rx-stop", "RX stop bit" },
    { "", "tx-stop", "TX stop bit" },
    { "", "atk-data-point", "ATK Data point" },
};

static const int uart_fast_row_rx_data_classes[] = { ANN_RX_DATA, ANN_RX_START, ANN_RX_PARITY_OK, ANN_RX_PARITY_ERR, ANN_RX_STOP, -1 };
static const int uart_fast_row_rx_warn_classes[] = { ANN_RX_WARN, -1 };
static const int uart_fast_row_tx_data_classes[] = { ANN_TX_DATA, ANN_TX_START, ANN_TX_PARITY_OK, ANN_TX_PARITY_ERR, ANN_TX_STOP, -1 };
static const int uart_fast_row_tx_warn_classes[] = { ANN_TX_WARN, -1 };
static const int uart_fast_row_atk_classes[] = { ANN_ATK_POINT, -1 };

static const struct srd_c_ann_row uart_fast_ann_rows[] = {
    { "rx-data-vals", "RX data", uart_fast_row_rx_data_classes, 5 },
    { "rx-warnings", "RX warnings", uart_fast_row_rx_warn_classes, 1 },
    { "tx-data-vals", "TX data", uart_fast_row_tx_data_classes, 5 },
    { "tx-warnings", "TX warnings", uart_fast_row_tx_warn_classes, 1 },
    { "atk-signs", "ATK signs", uart_fast_row_atk_classes, 1 },
};

static const struct srd_decoder_binary uart_fast_binary[] = {
    { 0, "rx", "RX dump" },
    { 1, "tx", "TX dump" },
    { 2, "rxtx", "RX/TX dump" },
    { 3, "rx-ok", "RX dump (no error)" },
    { 4, "tx-ok", "TX dump (no error)" },
    { 5, "rxtx-ok", "RX/TX dump (no error)" },
};

static const char *uart_fast_inputs[] = { "logic" };
static const char *uart_fast_outputs[] = { "uart" };
static const char *uart_fast_tags[] = { "Embedded/industrial" };

static int uart_fast_parity_ok(enum uart_fast_parity ptype, int parity_bit, uint64_t data, int data_bits)
{
    if (ptype == PARITY_NONE || ptype == PARITY_IGNORE) return 1;
    if (ptype == PARITY_ZERO) return (parity_bit == 0);
    if (ptype == PARITY_ONE) return (parity_bit == 1);

    int ones = 0;
    uint64_t d = data;
    for (int i = 0; i < data_bits; i++) {
        ones += d & 1;
        d >>= 1;
    }
    ones += parity_bit;

    if (ptype == PARITY_ODD) return (ones % 2) == 1;
    if (ptype == PARITY_EVEN) return (ones % 2) == 0;
    return 1;
}

static void uart_fast_format_value(uint64_t v, int data_bits, int format, char *out, int out_size)
{
    switch (format) {
    case 4: /* ascii */
        if (v >= 32 && v <= 126) snprintf(out, out_size, "%c", (char)v);
        else if (data_bits <= 8) snprintf(out, out_size, "0x%02X", (unsigned)v);
        else snprintf(out, out_size, "0x%03X", (unsigned)v);
        break;
    case 1: /* dec */
        snprintf(out, out_size, "%llu", (unsigned long long)v);
        break;
    case 0: /* hex */
        if (data_bits <= 8) snprintf(out, out_size, "%02X", (unsigned)v);
        else snprintf(out, out_size, "%03X", (unsigned)v);
        break;
    case 2: /* oct */
        snprintf(out, out_size, "%03o", (unsigned)v);
        break;
    case 3: /* bin */
        for (int i = 0; i < data_bits; i++)
            out[i] = ((v >> (data_bits - 1 - i)) & 1) ? '1' : '0';
        out[data_bits] = '\0';
        break;
    default:
        snprintf(out, out_size, "%02X", (unsigned)v);
        break;
    }
}

static uint64_t get_bit_sample_point(uart_fast_state *s, int rxtx, int bit_num)
{
    return (uint64_t)(s->frame_start[rxtx] + (uint64_t)round(bit_num * s->bit_width + s->bit_samplenum));
}

static uint64_t get_bit_start(uart_fast_state *s, int rxtx, int bit_num)
{
    return (uint64_t)(s->frame_start[rxtx] + (uint64_t)round(bit_num * s->bit_width));
}

static uint64_t get_bit_end(uart_fast_state *s, int rxtx, int bit_num)
{
    return (uint64_t)(s->frame_start[rxtx] + (uint64_t)round((bit_num + 1) * s->bit_width));
}

static int get_rxtx_pin(uart_fast_state *s, struct srd_decoder_inst *di, int rxtx, int ch, uint64_t samplenum)
{
    (void)samplenum;
    (void)rxtx;
    int val = c_pin(di, ch);
    if (s->invert) val = val ? 0 : 1;
    return val;
}

static void uart_fast_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(uart_fast_state)));
    }
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(uart_fast_state));
    s->state[RX] = STATE_WAIT_FOR_START_BIT;
    s->state[TX] = STATE_WAIT_FOR_START_BIT;
    s->frame_valid[RX] = 1;
    s->frame_valid[TX] = 1;
    s->baudrate = 115200;
    s->data_bits = 8;
    s->stop_bits = 1.0;
    s->parity_type = PARITY_NONE;
    s->bit_order_msb = 0;
    s->format = 0;
    s->invert = 0;
    s->packet_idle_us = -1;
    s->show_data_point = 0;
    s->out_ann = -1;
    s->out_python = -1;
    s->out_binary = -1;
    s->bw = 1;
}

static void uart_fast_start(struct srd_decoder_inst *di)
{
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "uart");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "uart");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "uart");

    s->baudrate = (uint64_t)c_opt_int(di, "baudrate", 115200);
    s->data_bits = (int)c_opt_int(di, "data_bits", 8);
    s->stop_bits = c_opt_dbl(di, "stop_bits", 1.0);

    const char *parity_str = c_opt_str(di, "parity", "none");
    if (parity_str && strcmp(parity_str, "odd") == 0) s->parity_type = PARITY_ODD;
    else if (parity_str && strcmp(parity_str, "even") == 0) s->parity_type = PARITY_EVEN;
    else if (parity_str && strcmp(parity_str, "zero") == 0) s->parity_type = PARITY_ZERO;
    else if (parity_str && strcmp(parity_str, "one") == 0) s->parity_type = PARITY_ONE;
    else if (parity_str && strcmp(parity_str, "ignore") == 0) s->parity_type = PARITY_IGNORE;
    else s->parity_type = PARITY_NONE;

    const char *bit_order_str = c_opt_str(di, "bit_order", "lsb-first");
    s->bit_order_msb = (strcmp(bit_order_str, "msb-first") == 0) ? 1 : 0;

    const char *format_str = c_opt_str(di, "format", "hex");
    if (format_str && strcmp(format_str, "ascii") == 0) s->format = 4;
    else if (format_str && strcmp(format_str, "dec") == 0) s->format = 1;
    else if (format_str && strcmp(format_str, "oct") == 0) s->format = 2;
    else if (format_str && strcmp(format_str, "bin") == 0) s->format = 3;
    else s->format = 0;

    const char *inv_str = c_opt_str(di, "invert", "no");
    s->invert = (strcmp(inv_str, "yes") == 0) ? 1 : 0;

    s->packet_idle_us = c_opt_int(di, "packet_idle_us", -1);

    const char *show_dp_str = c_opt_str(di, "show_data_point", "no");
    s->show_data_point = (strcmp(show_dp_str, "yes") == 0) ? 1 : 0;

    s->has_rx = c_has_ch(di, 0);
    s->has_tx = c_has_ch(di, 1);
    s->bw = (s->data_bits + 7) / 8;

    /* If metadata() was called before start() (when baudrate was still 0),
     * bit_width won't have been set. Compute it now. */
    if (s->samplerate == 0)
        s->samplerate = c_samplerate(di);
    if (s->samplerate > 0 && s->baudrate > 0 && s->bit_width == 0) {
        s->bit_width = (double)s->samplerate / (double)s->baudrate;
        s->half_bit_width = s->bit_width * 0.5;
        s->bit_samplenum = s->bit_width * 0.5;

        double frame_samples = 1.0 + s->data_bits +
            (s->parity_type != PARITY_NONE ? 1.0 : 0.0) + s->stop_bits;
        frame_samples *= s->bit_width;
        s->frame_len_samples = frame_samples;
        s->frame_len_samples_int = (uint64_t)round(frame_samples);
        s->break_min_samples = s->frame_len_samples_int + 1;

        if (s->packet_idle_us > 0) {
            s->packet_idle_samples = (uint64_t)round(s->packet_idle_us * 1e-6 * s->samplerate);
            if (s->packet_idle_samples < 1) s->packet_idle_samples = 1;
        }
    }
}

static void uart_fast_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (value > 0 && s->baudrate > 0) {
            s->bit_width = (double)value / (double)s->baudrate;
            s->half_bit_width = s->bit_width * 0.5;
            s->bit_samplenum = s->bit_width * 0.5;

            double frame_samples = 1.0 + s->data_bits +
                (s->parity_type != PARITY_NONE ? 1.0 : 0.0) + s->stop_bits;
            frame_samples *= s->bit_width;
            s->frame_len_samples = frame_samples;
            s->frame_len_samples_int = (uint64_t)round(frame_samples);
            s->break_min_samples = s->frame_len_samples_int + 1;

            if (s->packet_idle_us > 0) {
                s->packet_idle_samples = (uint64_t)round(s->packet_idle_us * 1e-6 * value);
                if (s->packet_idle_samples < 1) s->packet_idle_samples = 1;
            }
        }
    }
}

static void handle_frame(struct srd_decoder_inst *di, int rxtx, uint64_t ss, uint64_t es)
{
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    
    c_proto(di, ss, es, s->out_python, "FRAME", C_U8(s->datavalue[rxtx]), C_U8(rxtx), C_U8(s->frame_valid[rxtx]), C_END);

    /* Binary output */
    {
        unsigned char bdata[2];
        bdata[0] = (unsigned char)s->datavalue[rxtx];
        c_put_bin(di, ss, es, s->out_binary, BIN_RX + rxtx, 1, bdata);
        c_put_bin(di, ss, es, s->out_binary, BIN_RXTX, 1, bdata);
        if (s->frame_valid[rxtx]) {
            c_put_bin(di, ss, es, s->out_binary, BIN_RX_OK + rxtx, 1, bdata);
            c_put_bin(di, ss, es, s->out_binary, BIN_RXTX_OK, 1, bdata);
        }
    }
}

static void handle_packet(struct srd_decoder_inst *di, int rxtx)
{
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    if (s->packet_data_cnt[rxtx] == 0) return;
    unsigned char pkt_data[8194];
    int pos = 0;
    pkt_data[pos++] = (unsigned char)rxtx;
    pkt_data[pos++] = (unsigned char)s->packet_valid[rxtx];
    for (int i = 0; i < s->packet_data_cnt[rxtx] && pos + 1 < (int)sizeof(pkt_data); i++) {
        pkt_data[pos++] = (unsigned char)s->packet_data[rxtx][i];
    }
    c_proto(di, s->ss_packet[rxtx], s->es_packet[rxtx], s->out_python, "PACKET", C_U8(rxtx), C_U8(s->packet_valid[rxtx]), C_BYTES((const uint8_t *)&s->packet_data[rxtx][0], s->packet_data_cnt[rxtx] * sizeof(uint64_t)), C_END);
    s->packet_data_cnt[rxtx] = 0;
}

static void get_packet_data(uart_fast_state *s, int rxtx, uint64_t frame_end)
{
    if (s->packet_data_cnt[rxtx] == 0) {
        s->ss_packet[rxtx] = s->frame_start[rxtx];
        s->packet_valid[rxtx] = s->frame_valid[rxtx];
    } else {
        if (!s->frame_valid[rxtx])
            s->packet_valid[rxtx] = 0;
    }
    if (s->packet_data_cnt[rxtx] < 4096)
        s->packet_data[rxtx][s->packet_data_cnt[rxtx]++] = s->datavalue[rxtx];
    s->es_packet[rxtx] = frame_end;
}

static void process_rxtx(struct srd_decoder_inst *di, int rxtx, uint64_t samplenum)
{
    (void)samplenum;
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    int ch = rxtx;
    int signal = get_rxtx_pin(s, di, rxtx, ch, samplenum);

    switch (s->state[rxtx]) {
    case STATE_WAIT_FOR_START_BIT:
        if (signal == 0) {
            s->break_start[rxtx] = samplenum;
            s->break_start_valid[rxtx] = 1;
            s->frame_start[rxtx] = samplenum;
            s->frame_valid[rxtx] = 1;
            s->datavalue[rxtx] = 0;
            s->paritybit[rxtx] = -1;
            s->databit_count[rxtx] = 0;
            s->stopbit_count[rxtx] = 0;
            s->state[rxtx] = STATE_GET_START_BIT;
        }
        break;

    case STATE_GET_START_BIT: {
        uint64_t sample_point = get_bit_sample_point(s, rxtx, 0);
        if (samplenum >= sample_point) {
            int start_bit = get_rxtx_pin(s, di, rxtx, ch, sample_point);
            uint64_t ss = get_bit_start(s, rxtx, 0);
            uint64_t es = get_bit_end(s, rxtx, 0);

            if (start_bit != 0) {
                c_put(di, ss, samplenum, s->out_ann, ANN_RX_WARN + rxtx, "Start bit error", "Start err", "SE");
                unsigned char py_val = (unsigned char)start_bit;
                c_proto(di, ss, samplenum, s->out_python, "INVALID STARTBIT", C_U8(py_val), C_END);
                s->frame_valid[rxtx] = 0;
                handle_frame(di, rxtx, ss, samplenum);
                s->state[rxtx] = STATE_WAIT_FOR_START_BIT;
            } else {
                c_put(di, ss, es, s->out_ann, ANN_RX_START + rxtx, "Start bit", "Start", "S");
                unsigned char py_val = (unsigned char)start_bit;
                c_proto(di, ss, es, s->out_python, "STARTBIT", C_U8(py_val), C_END);
                s->state[rxtx] = STATE_GET_DATA_BITS;
                s->databit_count[rxtx] = 0;
            }
        }
    } break;

    case STATE_GET_DATA_BITS: {
        int bit_idx = s->databit_count[rxtx];
        uint64_t sample_point = get_bit_sample_point(s, rxtx, bit_idx + 1);
        if (samplenum >= sample_point) {
            int bit_val = get_rxtx_pin(s, di, rxtx, ch, sample_point);
            uint64_t ss = get_bit_start(s, rxtx, bit_idx + 1);

            if (s->show_data_point) {
                uint64_t center = ss + (uint64_t)(s->bit_width / 2);
                char rxtx_str[4];
                snprintf(rxtx_str, sizeof(rxtx_str), "%d", rxtx);
                c_put(di, center, center, s->out_ann, ANN_ATK_POINT, rxtx_str);
            }

            if (s->bit_order_msb)
                s->datavalue[rxtx] |= (bit_val << (s->data_bits - 1 - bit_idx));
            else
                s->datavalue[rxtx] |= (bit_val << bit_idx);

            s->databit_count[rxtx]++;

            if (s->databit_count[rxtx] >= s->data_bits) {
                /* Data complete */
                uint64_t data_ss = get_bit_start(s, rxtx, 1);
                uint64_t data_es = get_bit_end(s, rxtx, s->data_bits);
                char val_str[32];
                uart_fast_format_value(s->datavalue[rxtx], s->data_bits, s->format, val_str, sizeof(val_str));
                c_put(di, data_ss, data_es, s->out_ann, ANN_RX_DATA + rxtx, val_str);
                
                c_proto(di, data_ss, data_es, s->out_python, "DATA", C_U8(s->datavalue[rxtx]), C_U8(rxtx), C_END);

                if (s->parity_type != PARITY_NONE)
                    s->state[rxtx] = STATE_GET_PARITY_BIT;
                else {
                    s->state[rxtx] = STATE_GET_STOP_BITS;
                    s->stopbit_count[rxtx] = 0;
                }
            }
        }
    } break;

    case STATE_GET_PARITY_BIT: {
        int parity_bit_num = 1 + s->data_bits;
        uint64_t sample_point = get_bit_sample_point(s, rxtx, parity_bit_num);
        if (samplenum >= sample_point) {
            int parity_val = get_rxtx_pin(s, di, rxtx, ch, sample_point);
            s->paritybit[rxtx] = parity_val;
            uint64_t ss = get_bit_start(s, rxtx, parity_bit_num);
            uint64_t es = get_bit_end(s, rxtx, parity_bit_num);

            if (uart_fast_parity_ok(s->parity_type, parity_val, s->datavalue[rxtx], s->data_bits)) {
                c_put(di, ss, es, s->out_ann, ANN_RX_PARITY_OK + rxtx, "Parity bit", "Parity", "P");
                unsigned char pval = (unsigned char)parity_val;
                c_proto(di, ss, es, s->out_python, "PARITYBIT", C_U8(pval), C_END);
            } else {
                c_put(di, ss, es, s->out_ann, ANN_RX_PARITY_ERR + rxtx, "Parity error", "Parity err", "PE");
                unsigned char pval = (unsigned char)parity_val;
                c_proto(di, ss, es, s->out_python, "PARITYBIT", C_U8(pval), C_END);
                
                c_proto(di, ss, es, s->out_python, "PARITY ERROR", C_U8(!parity_val), C_U8(parity_val), C_END);
                s->frame_valid[rxtx] = 0;
            }
            s->state[rxtx] = STATE_GET_STOP_BITS;
            s->stopbit_count[rxtx] = 0;
        }
    } break;

    case STATE_GET_STOP_BITS: {
        int stop_bit_num = 1 + s->data_bits;
        if (s->parity_type != PARITY_NONE) stop_bit_num++;
        stop_bit_num += s->stopbit_count[rxtx];

        double remaining_stop = s->stop_bits - s->stopbit_count[rxtx];
        int is_half_stop = (remaining_stop > 0.4 && remaining_stop < 0.6);

        uint64_t sample_point;
        if (is_half_stop) {
            sample_point = s->frame_start[rxtx] + (uint64_t)round(stop_bit_num * s->bit_width + s->bit_samplenum * 0.5);
        } else {
            sample_point = get_bit_sample_point(s, rxtx, stop_bit_num);
        }

        if (samplenum >= sample_point) {
            int stop_val = get_rxtx_pin(s, di, rxtx, ch, sample_point);
            uint64_t ss, es;

            if (is_half_stop) {
                ss = s->frame_start[rxtx] + (uint64_t)round(stop_bit_num * s->bit_width);
                es = ss + (uint64_t)round(s->bit_width * 0.5);
            } else {
                ss = get_bit_start(s, rxtx, stop_bit_num);
                es = get_bit_end(s, rxtx, stop_bit_num);
            }

            if (stop_val != 1) {
                /* Python uart-fast uses es = di_samplenum(di) (sample_point) for stop bit error */
                c_put(di, ss, sample_point, s->out_ann, ANN_RX_WARN + rxtx, "Stop bit error", "Stop err", "TE");
                unsigned char py_val = (unsigned char)stop_val;
                c_proto(di, ss, sample_point, s->out_python, "INVALID STOPBIT", C_U8(py_val), C_END);
                s->frame_valid[rxtx] = 0;
                /* Python uart-fast always outputs RX_STOP annotation, even on error */
                c_put(di, ss, sample_point, s->out_ann, ANN_RX_STOP + rxtx, "Stop bit", "Stop", "T");
                unsigned char sval = (unsigned char)stop_val;
                c_proto(di, ss, sample_point, s->out_python, "STOPBIT", C_U8(sval), C_END);
            } else {
                c_put(di, ss, es, s->out_ann, ANN_RX_STOP + rxtx, "Stop bit", "Stop", "T");
                unsigned char sval = (unsigned char)stop_val;
                c_proto(di, ss, es, s->out_python, "STOPBIT", C_U8(sval), C_END);
            }

            s->stopbit_count[rxtx]++;

            double total_stop_counted = s->stopbit_count[rxtx];
            int all_stop_bits_done = 0;
            if (s->stop_bits == 0.5) all_stop_bits_done = (s->stopbit_count[rxtx] >= 1);
            else if (s->stop_bits == 1.0) all_stop_bits_done = (s->stopbit_count[rxtx] >= 1);
            else if (s->stop_bits == 1.5) all_stop_bits_done = (s->stopbit_count[rxtx] >= 2);
            else if (s->stop_bits == 2.0) all_stop_bits_done = (s->stopbit_count[rxtx] >= 2);
            else all_stop_bits_done = (total_stop_counted >= s->stop_bits);

            if (all_stop_bits_done) {
                /* Break detection */
                if (s->break_start_valid[rxtx] && !s->frame_valid[rxtx] &&
                    s->datavalue[rxtx] == 0 && stop_val == 0) {
                    uint64_t break_ss = s->break_start[rxtx];
                    uint64_t break_es = samplenum;
                    if (break_es - break_ss >= s->break_min_samples) {
                        unsigned char rxtx_byte = (unsigned char)rxtx;
                        c_proto(di, break_ss, break_es, s->out_python, "BREAK", C_U8(rxtx_byte), C_END);
                        s->break_start_valid[rxtx] = 0;
                    }
                }

                uint64_t frame_ss = s->frame_start[rxtx];
                uint64_t frame_es = frame_ss + (uint64_t)round(s->frame_len_samples);
                handle_frame(di, rxtx, frame_ss, frame_es);
                get_packet_data(s, rxtx, frame_es);

                /* Check packet idle */
                if (s->packet_idle_samples > 0 && s->packet_data_cnt[rxtx] > 0) {
                    if (samplenum >= s->es_packet[rxtx] + s->packet_idle_samples) {
                        handle_packet(di, rxtx);
                    }
                }

                s->state[rxtx] = STATE_WAIT_FOR_START_BIT;
            }
        }
    } break;
    }
}

static void uart_fast_decode(struct srd_decoder_inst *di)
{
    uart_fast_state *s = (uart_fast_state *)c_decoder_get_private(di);
    /* Samplerate fallback */
    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0 && s->baudrate > 0) {
            s->bit_width = (double)s->samplerate / (double)s->baudrate;
            s->half_bit_width = s->bit_width * 0.5;
            s->bit_samplenum = s->bit_width * 0.5;

            double frame_samples = 1.0 + s->data_bits +
                (s->parity_type != PARITY_NONE ? 1.0 : 0.0) + s->stop_bits;
            frame_samples *= s->bit_width;
            s->frame_len_samples = frame_samples;
            s->frame_len_samples_int = (uint64_t)round(frame_samples);
            s->break_min_samples = s->frame_len_samples_int + 1;

            if (s->packet_idle_us > 0) {
                s->packet_idle_samples = (uint64_t)round(s->packet_idle_us * 1e-6 * s->samplerate);
                if (s->packet_idle_samples < 1) s->packet_idle_samples = 1;
            }
        }
    }

    if (s->samplerate == 0 || s->baudrate == 0 || s->bit_width == 0)
        return;

    if (!s->has_rx && !s->has_tx)
        return;

    c_put(di, 0, 0, s->out_ann, ANN_ATK_POINT, "color:#fbca47");

    while (1) {
        int ret;
        if (s->has_rx) {
            if (s->invert)
                ret = c_wait(di, CW_R(0), CW_END);
            else
                ret = c_wait(di, CW_F(0), CW_OR, CW_END);
        }
        if (s->has_tx) {
            if (s->invert)
                ret = c_wait(di, CW_R(1), CW_END);
            else
                ret = c_wait(di, CW_F(1), CW_OR, CW_END);
        }
        if (!s->has_rx && !s->has_tx)
            ret = c_wait(di, CW_SKIP(1), CW_END);
        if (ret != SRD_OK)
            return;

        if (s->has_rx) process_rxtx(di, RX, di_samplenum(di));
        if (s->has_tx) process_rxtx(di, TX, di_samplenum(di));
    }
}

static void uart_fast_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder uart_fast_c_decoder = {
    .id = "uart_fast_c",
    .name = "UART-fast(C)",
    .longname = "Universal Asynchronous Receiver/Transmitter (C)",
    .desc = "Asynchronous, serial bus. (Ultra-fast version, C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = uart_fast_optional_channels,
    .num_optional_channels = 2,
    .options = uart_fast_options,
    .num_options = 9,
    .num_annotations = NUM_ANN,
    .ann_labels = uart_fast_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = uart_fast_ann_rows,
    .inputs = uart_fast_inputs,
    .num_inputs = 1,
    .outputs = uart_fast_outputs,
    .num_outputs = 1,
    .binary = uart_fast_binary,
    .num_binary = NUM_BIN,
    .tags = uart_fast_tags,
    .num_tags = 1,
    .reset = uart_fast_reset,
    .start = uart_fast_start,
    .decode = uart_fast_decode,
    .destroy = uart_fast_destroy,
    .state_size = 0,
    .metadata = uart_fast_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    uart_fast_options[0].def = g_variant_new_int64(115200);
    uart_fast_options[1].def = g_variant_new_int64(8);
    uart_fast_options[2].def = g_variant_new_string("none");
    uart_fast_options[3].def = g_variant_new_double(1.0);
    uart_fast_options[4].def = g_variant_new_string("lsb-first");
    uart_fast_options[5].def = g_variant_new_string("hex");
    uart_fast_options[6].def = g_variant_new_string("no");
    uart_fast_options[7].def = g_variant_new_int64(-1);
    uart_fast_options[8].def = g_variant_new_string("no");

    GSList *data_bits_vals = NULL;
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(5));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(6));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(7));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(8));
    data_bits_vals = g_slist_append(data_bits_vals, g_variant_new_int64(9));
    uart_fast_options[1].values = data_bits_vals;

    GSList *parity_vals = NULL;
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("none"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("odd"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("even"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("zero"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("one"));
    parity_vals = g_slist_append(parity_vals, g_variant_new_string("ignore"));
    uart_fast_options[2].values = parity_vals;

    GSList *stop_vals = NULL;
    stop_vals = g_slist_append(stop_vals, g_variant_new_double(0.0));
    stop_vals = g_slist_append(stop_vals, g_variant_new_double(0.5));
    stop_vals = g_slist_append(stop_vals, g_variant_new_double(1.0));
    stop_vals = g_slist_append(stop_vals, g_variant_new_double(1.5));
    stop_vals = g_slist_append(stop_vals, g_variant_new_double(2.0));
    uart_fast_options[3].values = stop_vals;

    GSList *bitorder_vals = NULL;
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb-first"));
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb-first"));
    uart_fast_options[4].values = bitorder_vals;

    GSList *format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("ascii"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("dec"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("hex"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("oct"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("bin"));
    uart_fast_options[5].values = format_vals;

    GSList *yn_vals = NULL;
    yn_vals = g_slist_append(yn_vals, g_variant_new_string("yes"));
    yn_vals = g_slist_append(yn_vals, g_variant_new_string("no"));
    uart_fast_options[6].values = yn_vals;
    uart_fast_options[8].values = yn_vals;

    return &uart_fast_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}