#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

#define TDM_AUDIO_MAX_CHANNELS 8

enum tdm_audio_ann {
    ANN_CH0 = 0,
    ANN_CH1,
    ANN_CH2,
    ANN_CH3,
    ANN_CH4,
    ANN_CH5,
    ANN_CH6,
    ANN_CH7,
    NUM_ANN = TDM_AUDIO_MAX_CHANNELS,
};

typedef struct {
    uint64_t samplerate;
    int channels;
    int channel;
    int bitdepth;
    int bitcount;
    int samplecount;
    int lastsync;
    int lastframe;
    uint64_t data;
    uint64_t ss_block;
    int have_ss_block;

    int edge;           /* 0=rising, 1=falling */
    int sampling_edge;  /* 0=first edge, 1=second edge */

    int out_ann;
} tdm_audio_state;

static struct srd_channel tdm_audio_channels[] = {
    {"clock", "Bitclk", "Data bit clock", 0, SRD_CHANNEL_SCLK, NULL},
    {"frame", "Framesync", "Frame sync", 1, SRD_CHANNEL_COMMON, NULL},
    {"data", "Data", "Serial data", 2, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option tdm_audio_options[] = {
    {"bps", NULL, "Bits per sample", NULL, NULL},
    {"channels", NULL, "Channels per frame", NULL, NULL},
    {"edge", NULL, "Clock edge to sample on", NULL, NULL},
    {"sampling_edge", NULL, "Sampling Edge", NULL, NULL},
};

static const char *tdm_audio_ann_labels[][3] = {
    {"", "ch0", "Ch0"},
    {"", "ch1", "Ch1"},
    {"", "ch2", "Ch2"},
    {"", "ch3", "Ch3"},
    {"", "ch4", "Ch4"},
    {"", "ch5", "Ch5"},
    {"", "ch6", "Ch6"},
    {"", "ch7", "Ch7"},
};

static const int row_ch0_classes[] = {ANN_CH0, -1};
static const int row_ch1_classes[] = {ANN_CH1, -1};
static const int row_ch2_classes[] = {ANN_CH2, -1};
static const int row_ch3_classes[] = {ANN_CH3, -1};
static const int row_ch4_classes[] = {ANN_CH4, -1};
static const int row_ch5_classes[] = {ANN_CH5, -1};
static const int row_ch6_classes[] = {ANN_CH6, -1};
static const int row_ch7_classes[] = {ANN_CH7, -1};

static const struct srd_c_ann_row tdm_audio_ann_rows[] = {
    {"ch0-vals", "Ch0", row_ch0_classes, 1},
    {"ch1-vals", "Ch1", row_ch1_classes, 1},
    {"ch2-vals", "Ch2", row_ch2_classes, 1},
    {"ch3-vals", "Ch3", row_ch3_classes, 1},
    {"ch4-vals", "Ch4", row_ch4_classes, 1},
    {"ch5-vals", "Ch5", row_ch5_classes, 1},
    {"ch6-vals", "Ch6", row_ch6_classes, 1},
    {"ch7-vals", "Ch7", row_ch7_classes, 1},
};

static const char *tdm_audio_inputs[] = {"logic"};
static const char *tdm_audio_tags[] = {"Audio"};

static void tdm_audio_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(tdm_audio_state)));
    tdm_audio_state *s = (tdm_audio_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tdm_audio_state));
    s->out_ann = -1;
    s->channels = 8;
    s->bitdepth = 16;
    s->edge = 0;
    s->sampling_edge = 0;
}

static void tdm_audio_start(struct srd_decoder_inst *di)
{
    tdm_audio_state *s = (tdm_audio_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tdm_audio");

    s->bitdepth = (int)c_opt_int(di, "bps", 16);
    if (s->bitdepth < 1)
        s->bitdepth = 16;

    s->channels = (int)c_opt_int(di, "channels", 8);
    if (s->channels < 1)
        s->channels = 1;
    if (s->channels > TDM_AUDIO_MAX_CHANNELS)
        s->channels = TDM_AUDIO_MAX_CHANNELS;

    const char *edge_str = c_opt_str(di, "edge", "rising");
    s->edge = (strcmp(edge_str, "falling") == 0) ? 1 : 0;

    const char *se_str = c_opt_str(di, "sampling_edge", "first edge");
    s->sampling_edge = (strcmp(se_str, "second edge") == 0) ? 1 : 0;
}

static void tdm_audio_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    tdm_audio_state *s = (tdm_audio_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void tdm_audio_decode(struct srd_decoder_inst *di)
{
    tdm_audio_state *s = (tdm_audio_state *)c_decoder_get_private(di);
    int CLK = 0;
    int FRAME = 1;
    int DATA = 2;

    while (1) {
        int ret;
        if (s->edge == 0)
            ret = c_wait(di, CW_R(CLK), CW_END);
        else
            ret = c_wait(di, CW_F(CLK), CW_END);
        if (ret != SRD_OK)
            return;

        int data_val = c_pin(di, DATA);
        int frame = c_pin(di, FRAME);

        /* Shift in data bit (same order as Python: shift before frame sync check) */
        s->data = (s->data << 1) | data_val;
        s->bitcount++;

        /* Check if we have enough bits for a channel sample */
        if (s->have_ss_block && s->bitcount >= s->bitdepth) {
            s->bitcount = 0;
            int ch = s->channel % s->channels;

            char c1[32], c2[16], c3[24], v[16];
            snprintf(c1, sizeof(c1), "Channel %d", ch);
            snprintf(c2, sizeof(c2), "C%d", ch);
            snprintf(c3, sizeof(c3), "%d", ch);

            if (s->bitdepth <= 8)
                snprintf(v, sizeof(v), "%02llX", (unsigned long long)s->data);
            else if (s->bitdepth <= 16)
                snprintf(v, sizeof(v), "%04llX", (unsigned long long)s->data);
            else
                snprintf(v, sizeof(v), "%08llX", (unsigned long long)s->data);

            char ann_long[64], ann_mid[48], ann_short[48];
            snprintf(ann_long, sizeof(ann_long), "%s: %s", c1, v);
            snprintf(ann_mid, sizeof(ann_mid), "%s: %s", c2, v);
            snprintf(ann_short, sizeof(ann_short), "%s: %s", c3, v);
            c_put(di, s->ss_block, di_samplenum(di), s->out_ann, ch,
                      ann_long, ann_mid, ann_short);

            s->data = 0;
            s->ss_block = di_samplenum(di);
            s->samplecount++;
            s->channel++;
        }

        /* Frame sync detection (after data shift, matching Python order) */
        if (frame != s->lastframe && frame == 1) {
            s->channel = 0;
            if (s->sampling_edge == 0) {
                /* First edge: include current sample */
                s->bitcount = 1;
                s->data = data_val;
            } else {
                /* Second edge: start from next */
                s->bitcount = 0;
                s->data = 0;
            }
            if (!s->have_ss_block) {
                s->ss_block = di_samplenum(di);
                s->have_ss_block = 1;
            }
        }
        s->lastframe = frame;
    }
}

static void tdm_audio_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder tdm_audio_c_decoder = {
    .id = "tdm_audio_c",
    .name = "TDM audio(C)",
    .longname = "Time division multiplex audio (C)",
    .desc = "TDM multi-channel audio protocol (C implementation)",
    .license = "gplv2+",
    .channels = tdm_audio_channels,
    .num_channels = 3,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = tdm_audio_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = tdm_audio_ann_labels,
    .num_annotation_rows = 8,
    .annotation_rows = tdm_audio_ann_rows,
    .inputs = tdm_audio_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = tdm_audio_tags,
    .num_tags = 1,
    .reset = tdm_audio_reset,
    .start = tdm_audio_start,
    .decode = tdm_audio_decode,
    .destroy = tdm_audio_destroy,
    .state_size = 0,
    .metadata = tdm_audio_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    tdm_audio_options[0].idn = "dec_tdm_audio_opt_bps";
    tdm_audio_options[0].def = g_variant_new_uint64(16);

    tdm_audio_options[1].idn = "dec_tdm_audio_opt_channels";
    tdm_audio_options[1].def = g_variant_new_uint64(8);
    GSList *ch_vals = NULL;
    for (int i = 1; i <= 8; i++)
        ch_vals = g_slist_append(ch_vals, g_variant_new_uint64(i));
    tdm_audio_options[1].values = ch_vals;

    tdm_audio_options[2].idn = "dec_tdm_audio_opt_edge";
    tdm_audio_options[2].def = g_variant_new_string("rising");
    GSList *edge_vals = NULL;
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("rising"));
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("falling"));
    tdm_audio_options[2].values = edge_vals;

    tdm_audio_options[3].idn = "dec_tdm_audio_opt_sampling_edge";
    tdm_audio_options[3].def = g_variant_new_string("first edge");
    GSList *se_vals = NULL;
    se_vals = g_slist_append(se_vals, g_variant_new_string("first edge"));
    se_vals = g_slist_append(se_vals, g_variant_new_string("second edge"));
    tdm_audio_options[3].values = se_vals;

    return &tdm_audio_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}