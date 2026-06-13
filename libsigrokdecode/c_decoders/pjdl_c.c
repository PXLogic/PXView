#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PJDL - Padded Jittering Data Link decoder */

#define PIN_DATA 0

enum pjdl_ann {
    ANN_CARRIER_BUSY = 0,
    ANN_CARRIER_IDLE,
    ANN_PAD_BIT,
    ANN_LOW_BIT,
    ANN_DATA_BIT,
    ANN_SHORT_DATA,
    ANN_SYNC_LOSS,
    ANN_DATA_BYTE,
    ANN_FRAME_INIT,
    ANN_FRAME_BYTES,
    ANN_FRAME_WAIT,
    NUM_ANN,
};

enum pjdl_symbol_type {
    SYM_IDLE = 0,
    SYM_PAD_BIT,
    SYM_ZERO_BIT,
    SYM_DATA_BIT,
    SYM_SHORT_BIT,
    SYM_SYNC_PAD,
    SYM_DATA_BYTE,
    SYM_FRAME_INIT,
    SYM_WAIT_ACK,
};

#define PJDL_MAX_SYMBOLS 1024
#define PJDL_MAX_FRAME_BYTES 256

struct pjdl_symbol {
    uint64_t ss;
    uint64_t es;
    int type;
    int data;
};

struct pjdl_priv {
    uint64_t samplerate;
    int mode;

    /* Timing parameters in samples */
    double data_width;
    double pad_width;
    double byte_width;
    double idle_width;
    double idle_add_width;
    double add_idle_width;
    uint64_t hold_high_width;
    uint64_t lookahead_width;

    /* Bit width ranges in samples */
    uint64_t data_bit_1_range[2];
    uint64_t data_bit_2_range[2];
    uint64_t data_bit_3_range[2];
    uint64_t data_bit_4_range[2];
    uint64_t short_data_range[2];
    uint64_t pad_bit_range[2];

    /* Carrier sense */
    int carrier_want_idle;
    int carrier_is_busy;
    int carrier_is_idle;
    uint64_t carrier_idle_ss;
    uint64_t carrier_busy_ss;

    /* Edge tracking */
    uint64_t edges[4];
    int edge_count;

    /* Symbol list */
    struct pjdl_symbol symbols[PJDL_MAX_SYMBOLS];
    int symbol_count;

    /* Data bit collection */
    int data_bits[8];
    int data_bit_count;
    uint64_t data_fall_time;

    /* Frame byte collection */
    uint8_t frame_bytes[PJDL_MAX_FRAME_BYTES];
    int frame_byte_count;

    int out_ann;
    int out_python;
};

static struct srd_channel pjdl_channels[] = {
    { "data", "DATA", "Single wire data", 0, SRD_CHANNEL_SDATA, "dec_pjdl_chan_data" },
};

static struct srd_decoder_option pjdl_options[] = {
    { "mode", "dec_pjdl_opt_mode", "Communication mode", NULL, NULL },
    { "idle_add_us", "dec_pjdl_opt_idle_add_us", "Added idle time (us)", NULL, NULL },
};

static const char *pjdl_ann_labels[][3] = {
    { "", "Carrier busy", "BUSY" },
    { "", "Carrier idle", "IDLE" },
    { "", "Pad bit", "PAD" },
    { "", "Low bit", "ZERO" },
    { "", "Data bit", "DATA" },
    { "", "Short data", "SHORT" },
    { "", "Sync loss", "LOSS" },
    { "", "Data byte", "BYTE" },
    { "", "Frame init", "INIT" },
    { "", "Frame bytes", "FRAME" },
    { "", "Frame wait", "WAIT" },
};

static const int pjdl_row_carriers_classes[] = { ANN_CARRIER_BUSY, ANN_CARRIER_IDLE, -1 };
static const int pjdl_row_bits_classes[] = { ANN_PAD_BIT, ANN_LOW_BIT, ANN_DATA_BIT, ANN_SHORT_DATA, -1 };
static const int pjdl_row_bytes_classes[] = { ANN_FRAME_INIT, ANN_DATA_BYTE, ANN_FRAME_WAIT, -1 };
static const int pjdl_row_frames_classes[] = { ANN_FRAME_BYTES, -1 };
static const int pjdl_row_warns_classes[] = { ANN_SYNC_LOSS, -1 };

static const struct srd_c_ann_row pjdl_ann_rows[] = {
    { "carriers", "Carriers", pjdl_row_carriers_classes, 2 },
    { "bits", "Bits", pjdl_row_bits_classes, 4 },
    { "bytes", "Bytes", pjdl_row_bytes_classes, 3 },
    { "frames", "Frames", pjdl_row_frames_classes, 1 },
    { "warns", "Warnings", pjdl_row_warns_classes, 1 },
};

static const char *pjdl_inputs[] = { "logic" };
static const char *pjdl_outputs[] = { "pjon_link" };
static const char *pjdl_tags[] = { "Embedded/industrial" };

/* Mode times: (data_width_us, pad_width_us) */
static const double mode_times[5][2] = {
    {0, 0},        /* placeholder for index 0 */
    {44, 116},     /* mode 1 */
    {40, 92},      /* mode 2 */
    {28, 88},      /* mode 3 */
    {26, 60},      /* mode 4 */
};

#define TIME_TOL_PERC 10
#define TIME_TOL_ABS 1.5

static void pjdl_get_range(double width, double usec_width, uint64_t *lo, uint64_t *hi)
{
    double reladd = TIME_TOL_PERC / 100.0;
    double absadd = TIME_TOL_ABS;
    double lower = fmin(width * (1 - reladd), width - absadd);
    double upper = fmax(width * (1 + reladd), width + absadd);
    *lo = (uint64_t)floor(lower * usec_width);
    *hi = (uint64_t)ceil(upper * usec_width) + 1;
}

static void pjdl_span_prepare(struct pjdl_priv *s)
{
    if (!s->samplerate)
        return;

    double dw = mode_times[s->mode][0];
    double pw = mode_times[s->mode][1];
    s->data_width = dw;
    s->pad_width = pw;
    s->byte_width = pw + 9 * dw;
    s->add_idle_width = s->idle_add_width;
    s->idle_width = s->byte_width + s->add_idle_width;

    double usec_width = s->samplerate / 1e6;
    s->hold_high_width = (uint64_t)(9 * TIME_TOL_ABS * usec_width);

    pjdl_get_range(dw * 1, usec_width, &s->data_bit_1_range[0], &s->data_bit_1_range[1]);
    pjdl_get_range(dw * 2, usec_width, &s->data_bit_2_range[0], &s->data_bit_2_range[1]);
    pjdl_get_range(dw * 3, usec_width, &s->data_bit_3_range[0], &s->data_bit_3_range[1]);
    pjdl_get_range(dw * 4, usec_width, &s->data_bit_4_range[0], &s->data_bit_4_range[1]);
    pjdl_get_range(dw / 4, usec_width, &s->short_data_range[0], &s->short_data_range[1]);
    pjdl_get_range(pw, usec_width, &s->pad_bit_range[0], &s->pad_bit_range[1]);

    s->data_width *= usec_width;
    s->pad_width *= usec_width;
    s->byte_width *= usec_width;
    s->idle_width *= usec_width;

    s->lookahead_width = (uint64_t)(4 * s->data_width);
}

static int pjdl_span_is_pad(struct pjdl_priv *s, uint64_t span)
{
    return span >= s->pad_bit_range[0] && span < s->pad_bit_range[1];
}

static int pjdl_span_is_data(struct pjdl_priv *s, uint64_t span)
{
    if (span >= s->data_bit_1_range[0] && span < s->data_bit_1_range[1]) return 1;
    if (span >= s->data_bit_2_range[0] && span < s->data_bit_2_range[1]) return 2;
    if (span >= s->data_bit_3_range[0] && span < s->data_bit_3_range[1]) return 3;
    if (span >= s->data_bit_4_range[0] && span < s->data_bit_4_range[1]) return 4;
    return 0;
}

static int pjdl_span_is_short(struct pjdl_priv *s, uint64_t span)
{
    return span >= s->short_data_range[0] && span < s->short_data_range[1];
}

static void pjdl_symbols_clear(struct pjdl_priv *s)
{
    s->symbol_count = 0;
}

static void pjdl_symbols_append(struct pjdl_priv *s, uint64_t ss, uint64_t es, int type, int data)
{
    if (s->symbol_count >= PJDL_MAX_SYMBOLS)
        return;
    s->symbols[s->symbol_count].ss = ss;
    s->symbols[s->symbol_count].es = es;
    s->symbols[s->symbol_count].type = type;
    s->symbols[s->symbol_count].data = data;
    s->symbol_count++;
}

static int pjdl_symbols_has_prev(struct pjdl_priv *s, const int *want_items, int count)
{
    if (s->symbol_count < count)
        return 0;
    int off = s->symbol_count - count;
    for (int i = 0; i < count; i++) {
        if (s->symbols[off + i].type != want_items[i])
            return 0;
    }
    return 1;
}

static void pjdl_symbols_collapse(struct pjdl_priv *s, int count, int symbol, int squeeze)
{
    if (s->symbol_count < count)
        return;
    int start = s->symbol_count - count;
    /* Squeeze: remove leading items matching squeeze type */
    while (squeeze && start > 0 && s->symbols[start - 1].type == squeeze) {
        start--;
        count++;
    }
    uint64_t ss = s->symbols[start].ss;
    uint64_t es = s->symbols[start + count - 1].es;
    /* Remove collapsed items */
    s->symbol_count = start;
    /* Append new symbol */
    pjdl_symbols_append(s, ss, es, symbol, 0);
}

static void pjdl_frame_flush(struct srd_decoder_inst *di, struct pjdl_priv *s)
{
    if (s->symbol_count == 0)
        return;

    /* Build frame text */
    char text[4096];
    int pos = 0;
    text[0] = '\0';

    for (int i = 0; i < s->symbol_count && pos < (int)sizeof(text) - 64; i++) {
        struct pjdl_symbol *sym = &s->symbols[i];
        if (sym->type == SYM_IDLE)
            continue;
        if (sym->type == SYM_FRAME_INIT) {
            if (pos > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
            pos += snprintf(text + pos, sizeof(text) - pos, "INIT");
        } else if (sym->type == SYM_SYNC_PAD) {
            if (pos == 0 || text[pos - 1] != 'C') {
                if (pos > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
                pos += snprintf(text + pos, sizeof(text) - pos, "SYNC");
            }
        } else if (sym->type == SYM_DATA_BYTE) {
            if (pos > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
            pos += snprintf(text + pos, sizeof(text) - pos, "%02x", sym->data);
        } else if (sym->type == SYM_SHORT_BIT) {
            if (pos > 0 && text[pos - 1] != 'T') {
                if (pos > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
                pos += snprintf(text + pos, sizeof(text) - pos, "SHORT");
            }
        } else if (sym->type == SYM_WAIT_ACK) {
            if (pos > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
            pos += snprintf(text + pos, sizeof(text) - pos, "WAIT");
        }
    }

    /* Find first and last non-IDLE symbol for annotation range */
    uint64_t ss = 0, es = 0;
    int has_non_idle = 0;
    for (int i = 0; i < s->symbol_count; i++) {
        if (s->symbols[i].type != SYM_IDLE) {
            if (!has_non_idle) { ss = s->symbols[i].ss; has_non_idle = 1; }
            es = s->symbols[i].es;
        }
    }

    if (has_non_idle && ss < es) {
        if (pos > 0)
            c_put(di, ss, es, s->out_ann, ANN_FRAME_BYTES, text);
        else
            c_put(di, ss, es, s->out_ann, ANN_FRAME_BYTES, "");
    }

    pjdl_symbols_clear(s);
}

static void pjdl_carrier_check(struct srd_decoder_inst *di, struct pjdl_priv *s, int level, uint64_t snum)
{
    if (level) {
        /* HIGH: end IDLE, start BUSY */
        s->carrier_is_idle = 0;
        s->carrier_idle_ss = 0;
        if (!s->carrier_is_busy) {
            s->carrier_is_busy = 1;
            s->carrier_busy_ss = snum;
        }
        return;
    }

    /* LOW: start tracking IDLE */
    if (!s->carrier_idle_ss)
        s->carrier_idle_ss = snum;

    uint64_t span = snum - s->carrier_idle_ss;
    if (span >= (uint64_t)s->byte_width)
        s->carrier_is_busy = 0;
    if (span >= (uint64_t)s->idle_width) {
        if (!s->carrier_is_idle)
            pjdl_frame_flush(di, s);
        s->carrier_is_idle = 1;
        s->carrier_want_idle = 0;
    }
}

static void pjdl_reset_state(struct pjdl_priv *s)
{
    s->carrier_want_idle = 1;
    s->carrier_is_busy = 0;
    s->carrier_is_idle = 0;
    s->carrier_idle_ss = 0;
    s->carrier_busy_ss = 0;
    s->edge_count = 0;
    s->symbol_count = 0;
    s->data_bit_count = 0;
    s->frame_byte_count = 0;
}

static void pjdl_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct pjdl_priv)));
    struct pjdl_priv *s = (struct pjdl_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct pjdl_priv));
    pjdl_reset_state(s);
    s->out_ann = -1;
    s->out_python = -1;
}

static void pjdl_start(struct srd_decoder_inst *di)
{
    struct pjdl_priv *s = (struct pjdl_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pjon_link");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "pjon_link");
}

static void pjdl_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct pjdl_priv *s = (struct pjdl_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        pjdl_span_prepare(s);
    }
}

static void pjdl_decode(struct srd_decoder_inst *di)
{
    struct pjdl_priv *s = (struct pjdl_priv *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate || s->samplerate < 1000000)
        return;

    /* Read options */
    s->mode = (int)c_opt_int(di, "mode", 1);
    if (s->mode < 1 || s->mode > 4)
        s->mode = 1;
    s->idle_add_width = (double)c_opt_int(di, "idle_add_us", 4);

    pjdl_span_prepare(s);

    /* Wait for first low */
    int ret = c_wait(di, CW_L(PIN_DATA), CW_END);
    if (ret != SRD_OK)
        return;

    pjdl_carrier_check(di, s, 0, di_samplenum(di));
    s->edges[0] = di_samplenum(di);
    s->edge_count = 1;

    while (1) {
        uint64_t last_snum = di_samplenum(di);

        /* Wait for edge or timeout */
        ret = c_wait(di, CW_E(PIN_DATA), CW_OR, CW_SKIP(s->lookahead_width), CW_END);
        if (ret != SRD_OK)
            return;

        int curr_level = c_pin(di, PIN_DATA);
        pjdl_carrier_check(di, s, curr_level, di_samplenum(di));

        int bit_level = curr_level;
        int edge_seen = (di_matched(di) & 0b1) != 0;
        if (edge_seen)
            bit_level = 1 - bit_level;

        if (s->edge_count == 0) {
            s->edges[0] = di_samplenum(di);
            s->edge_count = 1;
            continue;
        }

        /* Track edges */
        if (s->edge_count < 4) {
            s->edges[s->edge_count] = di_samplenum(di);
            s->edge_count++;
        } else {
            memmove(&s->edges[0], &s->edges[1], sizeof(uint64_t) * 3);
            s->edges[3] = di_samplenum(di);
        }

        uint64_t span = s->edges[s->edge_count - 1] - s->edges[s->edge_count - 2];
        int is_pad = bit_level && pjdl_span_is_pad(s, span);
        int is_data = pjdl_span_is_data(s, span);
        int is_short = bit_level && pjdl_span_is_short(s, span);

        if (is_pad) {
            uint64_t ss = s->edges[s->edge_count - 2];
            uint64_t es = di_samplenum(di);
            char txt[16];
            snprintf(txt, sizeof(txt), "%d", bit_level);
            c_put(di, ss, es, s->out_ann, ANN_PAD_BIT, "PAD", txt);
            pjdl_symbols_append(s, ss, es, SYM_PAD_BIT, bit_level);
            unsigned char pd = (unsigned char)bit_level;
            c_proto(di, ss, es, s->out_python, "PAD_BIT", C_U8(pd), C_END);
            continue;
        }

        if (is_short) {
            uint64_t ss = last_snum;
            uint64_t es = di_samplenum(di);
            char txt[16];
            snprintf(txt, sizeof(txt), "%d", bit_level);
            c_put(di, ss, es, s->out_ann, ANN_SHORT_DATA, "SHORT", txt);
            pjdl_symbols_append(s, ss, es, SYM_SHORT_BIT, bit_level);
            unsigned char pd = (unsigned char)bit_level;
            c_proto(di, ss, es, s->out_python, "SHORT_BIT", C_U8(pd), C_END);
            continue;
        }

        /* Force IDLE check when seeking sync */
        if (!bit_level && s->symbol_count == 0 && s->carrier_want_idle)
            continue;

        /* Accept LOW phases after DATA_BYTE, SHORT_BIT, WAIT_ACK */
        if (!bit_level) {
            int prev_types[] = { SYM_DATA_BYTE, SYM_SHORT_BIT, SYM_WAIT_ACK };
            for (int i = 0; i < 3; i++) {
                int t = prev_types[i];
                if (pjdl_symbols_has_prev(s, &t, 1))
                    goto next_iter;
            }
        }

        /* Get LOW DATA bit after PAD */
        int took_low = 0;
        if (is_data && !bit_level) {
            int pad_seq[] = { SYM_PAD_BIT };
            if (pjdl_symbols_has_prev(s, pad_seq, 1)) {
                took_low = 1;
                is_data -= 1;
                uint64_t next_snum = (uint64_t)(last_snum + s->data_width);
                uint64_t ss = last_snum;
                uint64_t es = next_snum;
                char txt[16];
                snprintf(txt, sizeof(txt), "%d", bit_level);
                c_put(di, ss, es, s->out_ann, ANN_LOW_BIT, "ZERO", txt);
                pjdl_symbols_append(s, ss, es, SYM_ZERO_BIT, bit_level);
                unsigned char pd = (unsigned char)bit_level;
                c_proto(di, ss, es, s->out_python, "DATA_BIT", C_U8(pd), C_END);
                s->data_fall_time = last_snum;
            }
        }

        /* Turn PAD + ZERO into SYNC_PAD */
        {
            int sync_seq[] = { SYM_PAD_BIT, SYM_ZERO_BIT };
            if (pjdl_symbols_has_prev(s, sync_seq, 2)) {
                pjdl_symbols_collapse(s, 2, SYM_SYNC_PAD, 0);
                unsigned char pd = 1;
                c_proto(di, s->symbols[s->symbol_count - 1].ss, s->symbols[s->symbol_count - 1].es, s->out_python, "SYNC_PAD", C_U8(pd), C_END);
                s->data_bit_count = 0;
            }
        }

        /* Turn 3 SYNC_PAD into FRAME_INIT */
        {
            int fi_seq[] = { SYM_SYNC_PAD, SYM_SYNC_PAD, SYM_SYNC_PAD };
            if (pjdl_symbols_has_prev(s, fi_seq, 3)) {
                /* Keep FRAME_INIT across flush */
                pjdl_symbols_collapse(s, 3, SYM_FRAME_INIT, 0);
                if (s->symbol_count > 1) {
                    struct pjdl_symbol keep = s->symbols[s->symbol_count - 1];
                    pjdl_frame_flush(di, s);
                    pjdl_symbols_clear(s);
                    pjdl_symbols_append(s, keep.ss, keep.es, keep.type, keep.data);
                }
                uint64_t fss = s->symbols[s->symbol_count - 1].ss;
                uint64_t fes = s->symbols[s->symbol_count - 1].es;
                c_put(di, fss, fes, s->out_ann, ANN_FRAME_INIT, "FRAME INIT", "INIT", "I");
                unsigned char pd = 1;
                c_proto(di, fss, fes, s->out_python, "FRAME_INIT", C_U8(pd), C_END);
                s->frame_byte_count = 0;
            }
        }

        /* Collapse SHORT + SYNC_PAD into WAIT_ACK */
        {
            int wait_seq[] = { SYM_SHORT_BIT, SYM_SYNC_PAD };
            if (pjdl_symbols_has_prev(s, wait_seq, 2)) {
                pjdl_symbols_collapse(s, 2, SYM_WAIT_ACK, SYM_SHORT_BIT);
                uint64_t wss = s->symbols[s->symbol_count - 1].ss;
                uint64_t wes = s->symbols[s->symbol_count - 1].es;
                c_put(di, wss, wes, s->out_ann, ANN_FRAME_WAIT,
                    "WAIT for sync response", "WAIT response", "WAIT", "W");
                unsigned char pd = 1;
                c_proto(di, wss, wes, s->out_python, "SYNC_RESP_WAIT", C_U8(pd), C_END);
            }
        }

        if (took_low && !is_data)
            continue;

        /* If no data, sync loss - but don't output annotation (Python has _with_ann_sync_loss = False) */
        if (!is_data) {
            pjdl_frame_flush(di, s);
            pjdl_reset_state(s);
            if (edge_seen && curr_level)
                s->edges[0] = di_samplenum(di);
            s->edge_count = (edge_seen && curr_level) ? 1 : 0;
            continue;
        }

        /* Need SYNC_PAD before data */
        {
            int sp_seq[] = { SYM_SYNC_PAD };
            if (!pjdl_symbols_has_prev(s, sp_seq, 1)) {
                pjdl_frame_flush(di, s);
                pjdl_reset_state(s);
                if (edge_seen && curr_level)
                    s->edges[0] = di_samplenum(di);
                s->edge_count = (edge_seen && curr_level) ? 1 : 0;
                continue;
            }
        }

        /* Sample 8 data bits at fixed intervals */
        int bit_field[8] = {0};
        double bit_ss = s->data_fall_time + s->data_width;
        for (int bit_idx = 0; bit_idx < 8; bit_idx++) {
            double bit_es = bit_ss + s->data_width;
            double bit_snum = (bit_es + bit_ss) / 2.0;

            /* Wait until sample point */
            if ((uint64_t)bit_snum > di_samplenum(di)) {
                uint64_t diff = (uint64_t)bit_snum - di_samplenum(di);
                if (diff > 0) {
                    ret = c_wait(di, CW_E(PIN_DATA), CW_OR, CW_SKIP(diff), CW_END);
                    if (ret != SRD_OK)
                        return;
                    curr_level = c_pin(di, PIN_DATA);
                    pjdl_carrier_check(di, s, curr_level, di_samplenum(di));
                }
            }

            int bl = c_pin(di, PIN_DATA);
            bit_field[bit_idx] = bl;

            uint64_t bss = (uint64_t)ceil(bit_ss);
            uint64_t bes = (uint64_t)floor(bit_es);
            char txt[4];
            snprintf(txt, sizeof(txt), "%d", bl);
            c_put(di, bss, bes, s->out_ann, ANN_DATA_BIT, txt);
            pjdl_symbols_append(s, bss, bes, SYM_DATA_BIT, bl);
            unsigned char pd = (unsigned char)bl;
            c_proto(di, bss, bes, s->out_python, "DATA_BIT", C_U8(pd), C_END);

            if (s->data_bit_count < 8)
                s->data_bits[s->data_bit_count++] = bl;

            bit_ss = bit_es;
        }

        /* Wait for end of last bit */
        {
            uint64_t end_snum = (uint64_t)bit_ss;
            if (end_snum > di_samplenum(di)) {
                uint64_t diff = end_snum - di_samplenum(di);
                ret = c_wait(di, CW_E(PIN_DATA), CW_OR, CW_SKIP(diff), CW_END);
                if (ret != SRD_OK)
                    return;
                curr_level = c_pin(di, PIN_DATA);
                pjdl_carrier_check(di, s, curr_level, di_samplenum(di));
            }
        }

        /* Check if last data bit is held high */
        if (curr_level) {
            ret = c_wait(di, CW_F(PIN_DATA), CW_OR, CW_SKIP(s->hold_high_width), CW_END);
            if (ret != SRD_OK)
                return;
            curr_level = c_pin(di, PIN_DATA);
            pjdl_carrier_check(di, s, curr_level, di_samplenum(di));
        }

        /* Compose byte value */
        uint8_t data_byte = 0;
        for (int i = 0; i < 8; i++)
            data_byte |= (bit_field[i] & 1) << i;

        if (s->data_bit_count == 8) {
            data_byte = 0;
            for (int i = 0; i < 8; i++)
                data_byte |= (s->data_bits[i] & 1) << i;
            s->data_bit_count = 0;
            if (s->frame_byte_count < PJDL_MAX_FRAME_BYTES)
                s->frame_bytes[s->frame_byte_count++] = data_byte;
        }

        /* Collapse SYNC_PAD + 8 DATA_BIT into DATA_BYTE */
        {
            int byte_seq[9] = { SYM_SYNC_PAD, SYM_DATA_BIT, SYM_DATA_BIT, SYM_DATA_BIT,
                SYM_DATA_BIT, SYM_DATA_BIT, SYM_DATA_BIT, SYM_DATA_BIT, SYM_DATA_BIT };
            if (pjdl_symbols_has_prev(s, byte_seq, 9)) {
                pjdl_symbols_collapse(s, 9, SYM_DATA_BYTE, 0);
                s->symbols[s->symbol_count - 1].data = data_byte;
                char txt[8];
                snprintf(txt, sizeof(txt), "%02x", data_byte);
                c_put_v(di, s->symbols[s->symbol_count - 1].ss,
                    s->symbols[s->symbol_count - 1].es, s->out_ann, ANN_DATA_BYTE,
                    data_byte, txt);
                c_proto(di, s->symbols[s->symbol_count - 1].ss, s->symbols[s->symbol_count - 1].es, s->out_python, "DATA_BYTE", C_U8(data_byte), C_END);
            }
        }

        /* Flush frame after WAIT_ACK + DATA_BYTE */
        {
            int resp_seq[] = { SYM_WAIT_ACK, SYM_DATA_BYTE };
            if (pjdl_symbols_has_prev(s, resp_seq, 2))
                pjdl_frame_flush(di, s);
        }

next_iter:
        /* Update edge tracking for next iteration */
        if (s->edge_count < 4) {
            s->edges[s->edge_count] = di_samplenum(di);
            s->edge_count++;
        } else {
            memmove(&s->edges[0], &s->edges[1], sizeof(uint64_t) * 3);
            s->edges[3] = di_samplenum(di);
        }
    }
}

static void pjdl_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder pjdl_c_decoder = {
    .id = "pjdl_c",
    .name = "PJDL(C)",
    .longname = "Padded Jittering Data Link(C)",
    .desc = "PJDL, a single wire serial link layer for PJON.(C implementation)",
    .license = "gplv2+",
    .channels = pjdl_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = pjdl_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = pjdl_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = pjdl_ann_rows,
    .inputs = pjdl_inputs,
    .num_inputs = 1,
    .outputs = pjdl_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = pjdl_tags,
    .num_tags = 1,
    .reset = pjdl_reset,
    .start = pjdl_start,
    .decode = pjdl_decode,
    .destroy = pjdl_destroy,
    .state_size = 0,
    .metadata = pjdl_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    pjdl_options[0].def = g_variant_new_int64(1);
    GSList *mode_vals = NULL;
    mode_vals = g_slist_append(mode_vals, g_variant_new_int64(1));
    mode_vals = g_slist_append(mode_vals, g_variant_new_int64(2));
    mode_vals = g_slist_append(mode_vals, g_variant_new_int64(3));
    mode_vals = g_slist_append(mode_vals, g_variant_new_int64(4));
    pjdl_options[0].values = mode_vals;

    pjdl_options[1].def = g_variant_new_int64(4);
    return &pjdl_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}