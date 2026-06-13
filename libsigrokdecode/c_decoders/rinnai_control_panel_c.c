#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BIT = 0,
    ANN_WARNING,
    ANN_RESET,
    ANN_BYTE,
    ANN_PACKET,
    NUM_ANN,
};

enum {
    STATE_INITIAL,
    STATE_IDLE,
    STATE_PRE,
    STATE_SYMBOL,
};

#define SYMBOL_DURATION_US       600
#define SHORT_RATIO_MIN          0.15
#define SHORT_RATIO_MAX          0.35
#define LONG_RATIO_MIN           0.65
#define LONG_RATIO_MAX           0.85
#define RESET_RATIO_MIN          1.0
#define RESET_RATIO_MAX          2.0

#define MAX_PACKET_BYTES         64

typedef struct {
    uint64_t samplerate;
    int state;
    uint64_t fall;
    uint64_t rise;
    int invert;
    int lsb_first;

    int bit_count;
    uint8_t byte_val;
    uint64_t byte_start;

    uint8_t bytes[MAX_PACKET_BYTES];
    int byte_count;
    uint64_t packet_start;

    int out_ann;
    int out_python;
} rinnai_state;

static struct srd_channel rinnai_channels[] = {
    {"data", "Data", "Pulse length signal line", 0, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option rinnai_options[] = {
    {"invert", NULL, "Invert bits", NULL, NULL},
    {"bit_numbering", NULL, "Bit numbering, first", NULL, NULL},
};

static const char *rinnai_ann_labels[][3] = {
    {"", "bit", "Bit"},
    {"", "warning", "Warning"},
    {"", "reset", "Reset"},
    {"", "byte", "Byte"},
    {"", "packet", "Packet"},
};

static const int rinnai_row_bits_classes[] = {ANN_BIT, ANN_RESET, -1};
static const int rinnai_row_warnings_classes[] = {ANN_WARNING, -1};
static const int rinnai_row_bytes_classes[] = {ANN_BYTE, -1};
static const int rinnai_row_packets_classes[] = {ANN_PACKET, -1};
static const struct srd_c_ann_row rinnai_ann_rows[] = {
    {"bits", "Bits", rinnai_row_bits_classes, 2},
    {"warnings", "Warnings", rinnai_row_warnings_classes, 1},
    {"bytes", "Bytes", rinnai_row_bytes_classes, 1},
    {"packets", "Packets", rinnai_row_packets_classes, 1},
};

static const char *rinnai_inputs[] = {"logic"};
static const char *rinnai_outputs[] = {"rinnai"};
static const char *rinnai_tags[] = {"Embedded/industrial"};

static double rinnai_samples_to_us(rinnai_state *s, uint64_t samples)
{
    return (samples * 1000000.0) / s->samplerate;
}

static void rinnai_bits_reset(rinnai_state *s)
{
    s->bit_count = 0;
    s->byte_val = 0;
    s->byte_start = (uint64_t)-1;
}

static void rinnai_bytes_reset(rinnai_state *s)
{
    s->byte_count = 0;
    s->packet_start = (uint64_t)-1;
}

static void rinnai_byte_append(struct srd_decoder_inst *di, rinnai_state *s,
                                uint64_t start, uint64_t end, uint8_t byte_val)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%02x", byte_val);
    c_put(di, start, end, s->out_ann, ANN_BYTE, buf);
    /* Python output */
    
    
    c_proto(di, start, end, s->out_python, "BYTE", C_U8(byte_val), C_END);
    rinnai_bits_reset(s);
    if (s->packet_start == (uint64_t)-1)
        s->packet_start = start;
    if (s->byte_count < MAX_PACKET_BYTES)
        s->bytes[s->byte_count++] = byte_val;
}

static void rinnai_bytes_flush(struct srd_decoder_inst *di, rinnai_state *s, uint64_t end)
{
    if (s->byte_count > 0) {
        char pkt_buf[MAX_PACKET_BYTES * 4];
        int pos = 0;
        for (int i = 0; i < s->byte_count && pos < (int)sizeof(pkt_buf) - 4; i++) {
            if (i > 0)
                pos += snprintf(pkt_buf + pos, sizeof(pkt_buf) - pos, ",");
            pos += snprintf(pkt_buf + pos, sizeof(pkt_buf) - pos, "%02x", s->bytes[i]);
        }
        c_put(di, s->packet_start, end, s->out_ann, ANN_PACKET, pkt_buf);
    }
    rinnai_bytes_reset(s);
}

static void rinnai_bit_append(struct srd_decoder_inst *di, rinnai_state *s,
                               uint64_t start, uint64_t end, int bit)
{
    /* Render bit */
    char bit_str[2] = {bit ? '1' : '0', '\0'};
    c_put(di, start, end, s->out_ann, ANN_BIT, bit_str);

    /* Manage bytes */
    if (s->byte_start == (uint64_t)-1)
        s->byte_start = start;

    if (s->lsb_first)
        s->byte_val |= (bit << s->bit_count);
    else
        s->byte_val = 2 * s->byte_val + bit;

    s->bit_count++;
    if (s->bit_count == 8)
        rinnai_byte_append(di, s, s->byte_start, end, s->byte_val);
}

static void rinnai_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(rinnai_state)));
    rinnai_state *s = (rinnai_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(rinnai_state));
    s->state = STATE_INITIAL;
    s->byte_start = (uint64_t)-1;
    s->packet_start = (uint64_t)-1;
}

static void rinnai_start(struct srd_decoder_inst *di)
{
    rinnai_state *s = (rinnai_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rinnai");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "rinnai");
    s->samplerate = c_samplerate(di);

    const char *inv = c_opt_str(di, "invert", "no");
    s->invert = (strcmp(inv, "yes") == 0) ? 1 : 0;

    const char *bn = c_opt_str(di, "bit_numbering", "lsb");
    s->lsb_first = (strcmp(bn, "lsb") == 0) ? 1 : 0;

    s->byte_start = (uint64_t)-1;
    s->packet_start = (uint64_t)-1;
}

static void rinnai_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    rinnai_state *s = (rinnai_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void rinnai_decode(struct srd_decoder_inst *di)
{
    rinnai_state *s = (rinnai_state *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate == 0) return;
    }

    while (1) {
        switch (s->state) {

        case STATE_INITIAL: {
            ret = c_wait(di, CW_L(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->fall = di_samplenum(di);
            s->state = STATE_IDLE;
            break;
        }

        case STATE_IDLE: {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->rise = di_samplenum(di);
            s->state = STATE_PRE;
            break;
        }

        case STATE_PRE: {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
            double time_us = rinnai_samples_to_us(s, di_samplenum(di) - s->rise);
            if (time_us > RESET_RATIO_MIN * SYMBOL_DURATION_US &&
                time_us < RESET_RATIO_MAX * SYMBOL_DURATION_US) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Reset: %d", (int)time_us);
                c_put(di, s->rise, di_samplenum(di), s->out_ann, ANN_RESET, buf);
                s->state = STATE_SYMBOL;
                rinnai_bytes_flush(di, s, di_samplenum(di));
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "Bad pre: %d", (int)time_us);
                c_put(di, s->rise, di_samplenum(di), s->out_ann, ANN_WARNING, buf);
                s->state = STATE_IDLE;
                rinnai_bytes_flush(di, s, di_samplenum(di));
            }
            s->fall = di_samplenum(di);
            break;
        }

        case STATE_SYMBOL: {
            /* Wait for rising edge */
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->rise = di_samplenum(di);

            /* Wait for falling edge */
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;

            double timeA = rinnai_samples_to_us(s, s->rise - s->fall);
            double timeB = rinnai_samples_to_us(s, di_samplenum(di) - s->rise);

            if (timeA > SHORT_RATIO_MIN * SYMBOL_DURATION_US &&
                timeA < SHORT_RATIO_MAX * SYMBOL_DURATION_US &&
                timeB > LONG_RATIO_MIN * SYMBOL_DURATION_US &&
                timeB < LONG_RATIO_MAX * SYMBOL_DURATION_US) {
                /* Short A + Long B = bit 1 (or 0 if inverted) */
                int bit = s->invert ? 0 : 1;
                rinnai_bit_append(di, s, s->fall, di_samplenum(di), bit);
            } else if (timeB > SHORT_RATIO_MIN * SYMBOL_DURATION_US &&
                       timeB < SHORT_RATIO_MAX * SYMBOL_DURATION_US &&
                       timeA > LONG_RATIO_MIN * SYMBOL_DURATION_US &&
                       timeA < LONG_RATIO_MAX * SYMBOL_DURATION_US) {
                /* Long A + Short B = bit 0 (or 1 if inverted) */
                int bit = s->invert ? 1 : 0;
                rinnai_bit_append(di, s, s->fall, di_samplenum(di), bit);
            } else if (timeB > RESET_RATIO_MIN * SYMBOL_DURATION_US &&
                       timeB < RESET_RATIO_MAX * SYMBOL_DURATION_US) {
                /* Reset detected in B phase */
                rinnai_bits_reset(s);
                char buf[32];
                snprintf(buf, sizeof(buf), "Reset: %d", (int)timeB);
                c_put(di, s->rise, di_samplenum(di), s->out_ann, ANN_RESET, buf);
                rinnai_bytes_flush(di, s, s->fall);
            } else {
                /* Bad bit */
                rinnai_bits_reset(s);
                char buf[64];
                snprintf(buf, sizeof(buf), "Bad Bit: %d,%d", (int)timeA, (int)timeB);
                c_put(di, s->fall, di_samplenum(di), s->out_ann, ANN_WARNING, buf);
                s->state = STATE_IDLE;
                rinnai_bytes_flush(di, s, s->fall);
            }
            s->fall = di_samplenum(di);
            break;
        }

        }
    }
}

static void rinnai_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder rinnai_control_panel_c_decoder = {
    .id = "rinnai_control_panel_c",
    .name = "Rinnai Control Panel(C)",
    .longname = "Rinnai control panel internal pulse length encoding protocol (C)",
    .desc = "Bidirectional, half-duplex, asynchronous serial bus. (C implementation)",
    .license = "gplv2+",
    .channels = rinnai_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = rinnai_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = rinnai_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = rinnai_ann_rows,
    .inputs = rinnai_inputs,
    .num_inputs = 1,
    .outputs = rinnai_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = rinnai_tags,
    .num_tags = 1,
    .reset = rinnai_reset,
    .start = rinnai_start,
    .metadata = rinnai_metadata,
    .decode = rinnai_decode,
    .destroy = rinnai_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GSList *inv_vals = NULL;
    inv_vals = g_slist_append(inv_vals, g_variant_new_string("yes"));
    inv_vals = g_slist_append(inv_vals, g_variant_new_string("no"));
    rinnai_options[0].id = "invert";
    rinnai_options[0].idn = "dec_rinnai_control_panel_opt_invert";
    rinnai_options[0].desc = "Invert bits";
    rinnai_options[0].def = g_variant_new_string("no");
    rinnai_options[0].values = inv_vals;

    GSList *bn_vals = NULL;
    bn_vals = g_slist_append(bn_vals, g_variant_new_string("lsb"));
    bn_vals = g_slist_append(bn_vals, g_variant_new_string("msb"));
    rinnai_options[1].id = "bit_numbering";
    rinnai_options[1].idn = "dec_rinnai_control_panel_opt_bit_numbering";
    rinnai_options[1].desc = "Bit numbering, first";
    rinnai_options[1].def = g_variant_new_string("lsb");
    rinnai_options[1].values = bn_vals;

    return &rinnai_control_panel_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}