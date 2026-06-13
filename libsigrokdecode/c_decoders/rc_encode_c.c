#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum rc_encode_ann {
    ANN_BIT_0 = 0,
    ANN_BIT_1,
    ANN_BIT_F,
    ANN_BIT_U,
    ANN_BIT_SYNC,
    ANN_PIN,
    ANN_CODE_WORD_ADDR,
    ANN_CODE_WORD_DATA,
    NUM_ANN,
};

struct rc_encode_priv {
    uint64_t samplerate;
    int out_ann;

    uint64_t samplenumber_last;
    int have_last;

    uint64_t pulses[4];
    int pulse_cnt;

    int bits[12];           /* 0='0', 1='1', 2='f', 3='U' */
    uint64_t bits_ss[12];
    uint64_t bits_es[12];
    int bit_count;

    uint64_t ss;
    uint64_t es;

    int state;              /* 0=IDLE, 1=DECODING, 2=DECODE_TIMEOUT */
    int model;              /* 0=none, 1=maplin_l95ar */
};

static struct srd_channel rc_encode_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_rc_encode_chan_data"},
};

static struct srd_decoder_option rc_encode_options[] = {
    {"remote", NULL, "Remote", NULL, NULL},
};

static const char *rc_encode_ann_labels[][3] = {
    {"", "bit-0", "Bit 0"},
    {"", "bit-1", "Bit 1"},
    {"", "bit-f", "Bit f"},
    {"", "bit-U", "Bit U"},
    {"", "bit-sync", "Bit sync"},
    {"", "pin", "Pin"},
    {"", "code-word-addr", "Code word address"},
    {"", "code-word-data", "Code word data"},
};

static const int rc_encode_row_bits_classes[] = {ANN_BIT_0, ANN_BIT_1, ANN_BIT_F, ANN_BIT_U, ANN_BIT_SYNC, -1};
static const int rc_encode_row_pins_classes[] = {ANN_PIN, -1};
static const int rc_encode_row_codewords_classes[] = {ANN_CODE_WORD_ADDR, ANN_CODE_WORD_DATA, -1};

static const struct srd_c_ann_row rc_encode_ann_rows[] = {
    {"bits", "Bits", rc_encode_row_bits_classes, 5},
    {"pins", "Pins", rc_encode_row_pins_classes, 1},
    {"code-words", "Code words", rc_encode_row_codewords_classes, 2},
};

static const char *rc_encode_inputs[] = {"logic"};
static const char *rc_encode_outputs[] = {};
static const char *rc_encode_tags[] = {"IC", "IR"};

/* Returns: 0='0', 1='1', 2='f', 3='U' */
static int rc_decode_bit(const uint64_t *edges)
{
    double lmin = 2.0, lmax = 5.0;
    double eqmin = 0.5, eqmax = 1.5;

    /* Logic 0: -___-___ (short-long-short-long) */
    if ((double)edges[1] >= (double)edges[0] * lmin &&
        (double)edges[1] <= (double)edges[0] * lmax &&
        (double)edges[2] >= (double)edges[0] * eqmin &&
        (double)edges[2] <= (double)edges[0] * eqmax &&
        (double)edges[3] >= (double)edges[0] * lmin &&
        (double)edges[3] <= (double)edges[0] * lmax) {
        return 0;
    }
    /* Logic 1: ---_---_ (long-short-long-short) */
    if ((double)edges[0] >= (double)edges[1] * lmin &&
        (double)edges[0] <= (double)edges[1] * lmax &&
        (double)edges[0] >= (double)edges[2] * eqmin &&
        (double)edges[0] <= (double)edges[2] * eqmax &&
        (double)edges[0] >= (double)edges[3] * lmin &&
        (double)edges[0] <= (double)edges[3] * lmax) {
        return 1;
    }
    /* Logic F: ---_-___ (short-long-long-short) */
    if ((double)edges[1] >= (double)edges[0] * lmin &&
        (double)edges[1] <= (double)edges[0] * lmax &&
        (double)edges[2] >= (double)edges[0] * lmin &&
        (double)edges[2] <= (double)edges[0] * lmax &&
        (double)edges[3] >= (double)edges[0] * eqmin &&
        (double)edges[3] <= (double)edges[0] * eqmax) {
        return 2;
    }
    return 3; /* Unknown */
}

static void rc_decode_model(struct srd_decoder_inst *di)
{
    struct rc_encode_priv *s = (struct rc_encode_priv *)c_decoder_get_private(di);

    /* Address: A0-A5, 0=on, 1/f=off */
    char addr_text[64] = "";
    int pos = 0;
    for (int i = 0; i < 6 && pos < 50; i++) {
        const char *state_str;
        if (s->bits[i] == 0)
            state_str = "on";
        else if (s->bits[i] == 1 || s->bits[i] == 2)
            state_str = "off";
        else
            state_str = "?";
        pos += snprintf(addr_text + pos, sizeof(addr_text) - pos,
                        "%sA%d=%s", (i > 0 ? ", " : ""), i, state_str);
    }
    c_put(di, s->bits_ss[0], s->bits_es[5], s->out_ann, ANN_CODE_WORD_ADDR, addr_text);

    /* Buttons: A6/D5-A11/D0 */
    char data_text[128] = "";
    pos = 0;
    for (int i = 6; i < 12 && pos < 100; i++) {
        const char *bit_str;
        switch (s->bits[i]) {
        case 0: bit_str = "0"; break;
        case 1: bit_str = "1"; break;
        case 2: bit_str = "f"; break;
        default: bit_str = "?"; break;
        }
        pos += snprintf(data_text + pos, sizeof(data_text) - pos,
                        "%sA%d/D%d=%s", (i > 6 ? ", " : ""), i, 12 - i, bit_str);
    }
    c_put(di, s->bits_ss[6], s->bits_es[11], s->out_ann, ANN_CODE_WORD_DATA, data_text);
}

static void rc_encode_reset_state(struct srd_decoder_inst *di)
{
    struct rc_encode_priv *s = (struct rc_encode_priv *)c_decoder_get_private(di);
    s->bit_count = 0;
    s->pulse_cnt = 0;
    s->have_last = 0;
    s->state = 0; /* IDLE */
}

static void rc_encode_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct rc_encode_priv)));
    struct rc_encode_priv *s = (struct rc_encode_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct rc_encode_priv));
    s->out_ann = -1;
    s->state = 0;
    s->model = 0;
}

static void rc_encode_start(struct srd_decoder_inst *di)
{
    struct rc_encode_priv *s = (struct rc_encode_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rc_encode");

    const char *remote_str = c_opt_str(di, "remote", "none");
    if (strcmp(remote_str, "maplin_l95ar") == 0)
        s->model = 1;
    else
        s->model = 0;
}

static void rc_encode_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct rc_encode_priv *s = (struct rc_encode_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void rc_encode_decode(struct srd_decoder_inst *di)
{
    struct rc_encode_priv *s = (struct rc_encode_priv *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0)
        return;

    while (1) {
        ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        if (!s->have_last) {
            s->samplenumber_last = di_samplenum(di);
            s->ss = di_samplenum(di);
            s->have_last = 1;
            continue;
        }

        if (s->bit_count < 12) {
            s->bit_count++;

            /* Collect 4 pulses */
            for (int i = 0; i < 4; i++) {
                if (i > 0) {
                    ret = c_wait(di, CW_E(0), CW_END);
                    if (ret != SRD_OK)
                        return;
                }
                uint64_t samples = di_samplenum(di) - s->samplenumber_last;
                s->pulses[i] = samples;
                s->samplenumber_last = di_samplenum(di);
            }
            s->es = di_samplenum(di);

            int bit_val = rc_decode_bit(s->pulses);
            s->bits[s->bit_count - 1] = bit_val;
            s->bits_ss[s->bit_count - 1] = s->ss;
            s->bits_es[s->bit_count - 1] = s->es;

            /* Output bit annotation */
            static const char *bit_names[] = {"0", "1", "f", "U"};
            c_put(di, s->ss, s->es, s->out_ann, bit_val, bit_names[bit_val]);

            /* Output pin label */
            char pin_label[32];
            if (s->bit_count <= 6)
                snprintf(pin_label, sizeof(pin_label), "A%d", s->bit_count - 1);
            else
                snprintf(pin_label, sizeof(pin_label), "A%d/D%d",
                         s->bit_count - 1, 12 - s->bit_count);
            c_put(di, s->ss, s->es, s->out_ann, ANN_PIN, pin_label);

            s->ss = di_samplenum(di);
        } else {
            /* Sync bit */
            if (s->model == 1) {
                rc_decode_model(di);
            }
            uint64_t samples = di_samplenum(di) - s->samplenumber_last;
            /* Wait for sync bit end */
            {
                ret = c_wait(di, CW_SKIP(8 * samples), CW_END);
                if (ret != SRD_OK)
                    return;
            }
            s->es = di_samplenum(di);
            c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_SYNC, "Sync");
            /* Reset */
            rc_encode_reset_state(di);
            s->state = 2; /* DECODE_TIMEOUT */
        }

        if (s->state != 2) {
            s->samplenumber_last = di_samplenum(di);
        }
    }
}

static void rc_encode_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder rc_encode_c_decoder = {
    .id = "rc_encode_c",
    .name = "RC encode(C)",
    .longname = "Remote control encoder (C)",
    .desc = "PT2262/HX2262/SC5262 remote control encoder protocol. (C implementation)",
    .license = "gplv2+",
    .channels = rc_encode_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = rc_encode_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = rc_encode_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = rc_encode_ann_rows,
    .inputs = rc_encode_inputs,
    .num_inputs = 1,
    .outputs = rc_encode_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rc_encode_tags,
    .num_tags = 2,
    .reset = rc_encode_reset,
    .start = rc_encode_start,
    .decode = rc_encode_decode,
    .destroy = rc_encode_destroy,
    .state_size = 0,
    .metadata = rc_encode_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    rc_encode_options[0].idn = "dec_rc_encode_opt_remote";
    rc_encode_options[0].def = g_variant_new_string("none");

    GSList *values = NULL;
    values = g_slist_append(values, g_variant_new_string("none"));
    values = g_slist_append(values, g_variant_new_string("maplin_l95ar"));
    rc_encode_options[0].values = values;

    return &rc_encode_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}