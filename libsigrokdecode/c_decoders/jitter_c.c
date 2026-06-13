#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum jitter_ann {
    ANN_JITTER = 0,
    ANN_CLK_MISSED,
    ANN_SIG_MISSED,
    NUM_ANN,
};

enum {
    STATE_CLK = 0,
    STATE_SIG = 1,
};

struct jitter_priv {
    int state;
    uint64_t samplerate;
    int oldclk;
    int oldsig;
    uint64_t clk_start;
    uint64_t sig_start;
    int clk_missed;
    int sig_missed;
    int clk_edge_type;  /* 0=rising, 1=falling, 2=both */
    int sig_edge_type;
    int out_ann;
    int out_binary;
};

static struct srd_channel jitter_channels[] = {
    {"clk", "Clock", "Clock reference channel", 0, SRD_CHANNEL_SCLK, "dec_jitter_chan_clk"},
    {"sig", "Resulting signal", "Resulting signal controlled by the clock", 1, SRD_CHANNEL_SDATA, "dec_jitter_chan_sig"},
};

static struct srd_decoder_option jitter_options[] = {
    {"clk_polarity", "dec_jitter_opt_clk_polarity", "Clock edge polarity", NULL, NULL},
    {"sig_polarity", "dec_jitter_opt_sig_polarity", "Resulting signal edge polarity", NULL, NULL},
};

static const char *jitter_ann_labels[][3] = {
    {"", "jitter", "Jitter value"},
    {"", "clk_missed", "Clock missed"},
    {"", "sig_missed", "Signal missed"},
};

static const int jitter_row_jitter_classes[] = {ANN_JITTER, -1};
static const int jitter_row_clk_missed_classes[] = {ANN_CLK_MISSED, -1};
static const int jitter_row_sig_missed_classes[] = {ANN_SIG_MISSED, -1};

static const struct srd_c_ann_row jitter_ann_rows[] = {
    {"jitter", "Jitter values", jitter_row_jitter_classes, 1},
    {"clk_missed", "Clock missed", jitter_row_clk_missed_classes, 1},
    {"sig_missed", "Signal missed", jitter_row_sig_missed_classes, 1},
};

static const struct srd_decoder_binary jitter_binary[] = {
    {0, "ascii-float", "Jitter values as newline-separated ASCII floats"},
};

static const char *jitter_inputs[] = {"logic"};
static const char *jitter_tags[] = {"Clock/timing", "Util"};

static int is_edge(int old_val, int new_val, int edge_type)
{
    switch (edge_type) {
    case 0: return (!old_val && new_val);   /* rising */
    case 1: return (old_val && !new_val);   /* falling */
    case 2: return (old_val != new_val);    /* both */
    default: return 0;
    }
}

static void format_jitter(double delta, char *buf, int buf_size)
{
    double t = delta;
    if (t == 0.0) {
        snprintf(buf, buf_size, "0 s");
        return;
    }
    double abs_t = fabs(t);
    if (abs_t >= 1.0)
        snprintf(buf, buf_size, "%.3g s", t);
    else if (abs_t >= 1e-3)
        snprintf(buf, buf_size, "%.3g ms", t * 1e3);
    else if (abs_t >= 1e-6)
        snprintf(buf, buf_size, "%.3g μs", t * 1e6);
    else if (abs_t >= 1e-9)
        snprintf(buf, buf_size, "%.3g ns", t * 1e9);
    else if (abs_t >= 1e-12)
        snprintf(buf, buf_size, "%.3g ps", t * 1e12);
    else
        snprintf(buf, buf_size, "%.3g fs", t * 1e15);
}

static void jitter_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct jitter_priv)));
    struct jitter_priv *s = (struct jitter_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct jitter_priv));
    s->out_ann = -1;
    s->out_binary = -1;
}

static void jitter_start(struct srd_decoder_inst *di)
{
    struct jitter_priv *s = (struct jitter_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "jitter");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "jitter");

    const char *clk_pol = c_opt_str(di, "clk_polarity", "rising");
    if (strcmp(clk_pol, "falling") == 0)
        s->clk_edge_type = 1;
    else if (strcmp(clk_pol, "both") == 0)
        s->clk_edge_type = 2;
    else
        s->clk_edge_type = 0;

    const char *sig_pol = c_opt_str(di, "sig_polarity", "rising");
    if (strcmp(sig_pol, "falling") == 0)
        s->sig_edge_type = 1;
    else if (strcmp(sig_pol, "both") == 0)
        s->sig_edge_type = 2;
    else
        s->sig_edge_type = 0;
}

static void jitter_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct jitter_priv *s = (struct jitter_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void jitter_decode(struct srd_decoder_inst *di)
{
    struct jitter_priv *s = (struct jitter_priv *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_END);
        if (ret != SRD_OK)
            return;

        int clk = c_pin(di, 0);
        int sig = c_pin(di, 1);

        /* Inner loop: can advance 2 states per sample */
        while (1) {
            if (s->state == STATE_CLK) {
                if (s->clk_start == di_samplenum(di))
                    break;
                if (is_edge(s->oldclk, clk, s->clk_edge_type)) {
                    s->clk_start = di_samplenum(di);
                    s->state = STATE_SIG;
                } else {
                    /* Check for missed signal */
                    if (s->sig_start != 0 && s->sig_start != di_samplenum(di)
                        && is_edge(s->oldsig, sig, s->sig_edge_type)) {
                        s->sig_missed++;
                        char miss_str[64];
                        snprintf(miss_str, sizeof(miss_str), "Missed signal");
                        char miss_short[16];
                        snprintf(miss_short, sizeof(miss_short), "MS");
                        c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_SIG_MISSED, miss_str, miss_short);
                    }
                    break;
                }
            }
            if (s->state == STATE_SIG) {
                if (s->sig_start == di_samplenum(di))
                    break;
                if (is_edge(s->oldsig, sig, s->sig_edge_type)) {
                    s->sig_start = di_samplenum(di);
                    double delta = (double)(s->sig_start - s->clk_start) / (double)s->samplerate;

                    char jitter_str[64];
                    format_jitter(delta, jitter_str, sizeof(jitter_str));
                    c_put(di, s->clk_start, s->sig_start, s->out_ann, ANN_JITTER, jitter_str);

                    /* Binary output: ASCII float + newline */
                    char bin_str[64];
                    snprintf(bin_str, sizeof(bin_str), "%.9g\n", delta);
                    c_put_bin(di, s->clk_start, s->sig_start,
                                         s->out_binary, 0, strlen(bin_str), (const unsigned char *)bin_str);

                    s->state = STATE_CLK;
                } else {
                    /* Check for missed clock */
                    if (s->clk_start != di_samplenum(di)
                        && is_edge(s->oldclk, clk, s->clk_edge_type)) {
                        s->clk_missed++;
                        char miss_str[64];
                        snprintf(miss_str, sizeof(miss_str), "Missed clock");
                        char miss_short[16];
                        snprintf(miss_short, sizeof(miss_short), "MC");
                        c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_CLK_MISSED, miss_str, miss_short);
                    }
                    break;
                }
            }
        }

        s->oldclk = clk;
        s->oldsig = sig;
    }
}

static void jitter_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder jitter_c_decoder = {
    .id = "jitter_c",
    .name = "Jitter(C)",
    .longname = "Timing jitter calculation (C)",
    .desc = "Retrieves the timing jitter between two digital signals (C implementation)",
    .license = "gplv2+",
    .channels = jitter_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = jitter_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = jitter_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = jitter_ann_rows,
    .inputs = jitter_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = jitter_binary,
    .num_binary = 1,
    .tags = jitter_tags,
    .num_tags = 2,
    .reset = jitter_reset,
    .start = jitter_start,
    .decode = jitter_decode,
    .destroy = jitter_destroy,
    .state_size = 0,
    .metadata = jitter_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GSList *clk_vals = NULL;
    clk_vals = g_slist_append(clk_vals, g_variant_new_string("rising"));
    clk_vals = g_slist_append(clk_vals, g_variant_new_string("falling"));
    clk_vals = g_slist_append(clk_vals, g_variant_new_string("both"));
    jitter_options[0].def = g_variant_new_string("rising");
    jitter_options[0].values = clk_vals;

    GSList *sig_vals = NULL;
    sig_vals = g_slist_append(sig_vals, g_variant_new_string("rising"));
    sig_vals = g_slist_append(sig_vals, g_variant_new_string("falling"));
    sig_vals = g_slist_append(sig_vals, g_variant_new_string("both"));
    jitter_options[1].def = g_variant_new_string("rising");
    jitter_options[1].values = sig_vals;

    return &jitter_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}