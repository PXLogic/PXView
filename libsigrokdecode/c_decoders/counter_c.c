#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_EDGE_COUNT = 0,
    ANN_WORD_COUNT,
    ANN_WORD_RESET,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int data_edge;
    int divider;
    int reset_edge;
    int64_t edge_off;
    int64_t word_off;
    int dead_cycles;
    int start_with_reset;
    int64_t edge_count;
    int64_t word_count;
    int dead_count;
    uint64_t edge_start;
    int edge_start_set;
    uint64_t word_start;
    int word_start_set;
    int out_ann;
} counter_state;

static struct srd_channel counter_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_counter_chan_data"},
};

static struct srd_channel counter_optional_channels[] = {
    {"reset", "Reset", "Reset line", 0, SRD_CHANNEL_SDATA, "dec_counter_opt_chan_reset"},
};

static struct srd_decoder_option counter_options[] = {
    {"data_edge", NULL, "Edges to count (data)", NULL, NULL},
    {"divider", NULL, "Count divider (word width)", NULL, NULL},
    {"reset_edge", NULL, "Edge which clears counters (reset)", NULL, NULL},
    {"edge_off", NULL, "Edge counter value after start/reset", NULL, NULL},
    {"word_off", NULL, "Word counter value after start/reset", NULL, NULL},
    {"dead_cycles", NULL, "Ignore this many edges after reset", NULL, NULL},
    {"start_with_reset", NULL, "Assume decode starts with reset", NULL, NULL},
};

static const char *counter_inputs[] = {"logic", NULL};
static const char *counter_outputs[] = {NULL};
static const char *counter_tags[] = {"Util", NULL};

static const char *counter_ann_labels[][3] = {
    {"", "edge_count", "Edge count"},
    {"", "word_count", "Word count"},
    {"", "word_reset", "Word reset"},
};

static const int counter_row_edge_classes[] = {ANN_EDGE_COUNT};
static const int counter_row_word_classes[] = {ANN_WORD_COUNT};
static const int counter_row_reset_classes[] = {ANN_WORD_RESET};
static const struct srd_c_ann_row counter_ann_rows[] = {
    {"edge_counts", "Edges", counter_row_edge_classes, 1},
    {"word_counts", "Words", counter_row_word_classes, 1},
    {"word_resets", "Word resets", counter_row_reset_classes, 1},
};

static void counter_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(counter_state)));
    }
    counter_state *s = (counter_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(counter_state));
}

static void counter_start(struct srd_decoder_inst *di)
{
    counter_state *s = (counter_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "counter");
    s->samplerate = c_samplerate(di);

    const char *de = c_opt_str(di, "data_edge", "any");
    if (strcmp(de, "rising") == 0)
        s->data_edge = 1;
    else if (strcmp(de, "falling") == 0)
        s->data_edge = 2;
    else
        s->data_edge = 0;

    s->divider = (int)c_opt_int(di, "divider", 0);
    if (s->divider < 0)
        s->divider = 0;

    const char *re = c_opt_str(di, "reset_edge", "falling");
    s->reset_edge = (strcmp(re, "rising") == 0) ? 1 : 2;

    s->edge_off = (int64_t)c_opt_int(di, "edge_off", 0);
    s->word_off = (int64_t)c_opt_int(di, "word_off", 0);
    s->dead_cycles = (int)c_opt_int(di, "dead_cycles", 0);

    const char *swr = c_opt_str(di, "start_with_reset", "no");
    s->start_with_reset = (strcmp(swr, "yes") == 0) ? 1 : 0;

    s->edge_count = s->edge_off;
    s->word_count = s->word_off;
    s->edge_start_set = 0;
    s->word_start_set = 0;

    if (s->start_with_reset) {
        s->dead_count = s->dead_cycles;
    }
}

static void counter_decode(struct srd_decoder_inst *di)
{
    counter_state *s = (counter_state *)c_decoder_get_private(di);
    int has_reset = c_has_ch(di, 1);

    while (1) {
        int ret;
        if (has_reset) {
            if (s->data_edge == 1) {
                if (s->reset_edge == 1)
                    ret = c_wait(di, CW_R(0), CW_OR, CW_R(1), CW_END);
                else
                    ret = c_wait(di, CW_R(0), CW_OR, CW_F(1), CW_END);
            } else if (s->data_edge == 2) {
                if (s->reset_edge == 1)
                    ret = c_wait(di, CW_F(0), CW_OR, CW_R(1), CW_END);
                else
                    ret = c_wait(di, CW_F(0), CW_OR, CW_F(1), CW_END);
            } else {
                if (s->reset_edge == 1)
                    ret = c_wait(di, CW_E(0), CW_OR, CW_R(1), CW_END);
                else
                    ret = c_wait(di, CW_E(0), CW_OR, CW_F(1), CW_END);
            }
        } else {
            if (s->data_edge == 1)
                ret = c_wait(di, CW_R(0), CW_END);
            else if (s->data_edge == 2)
                ret = c_wait(di, CW_F(0), CW_END);
            else
                ret = c_wait(di, CW_E(0), CW_END);
        }
        if (ret != SRD_OK)
            return;

        if (has_reset && (di_matched(di) & 0x2)) {
            s->edge_count = s->edge_off;
            s->edge_start = di_samplenum(di);
            s->edge_start_set = 1;
            s->word_count = s->word_off;
            s->word_start = di_samplenum(di);
            s->word_start_set = 1;
            c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_WORD_RESET,
                "Word reset", "Reset", "Rst", "R");
            s->dead_count = s->dead_cycles;
            continue;
        }

        if (s->dead_count) {
            s->dead_count--;
            s->edge_start = di_samplenum(di);
            s->edge_start_set = 1;
            s->word_start = di_samplenum(di);
            s->word_start_set = 1;
            continue;
        }

        if (!s->edge_start_set) {
            s->edge_start = 0;
            s->edge_start_set = 1;
        }
        if (!s->word_start_set) {
            s->word_start = 0;
            s->word_start_set = 1;
        }

        s->edge_count++;
        char t1[32];
        snprintf(t1, sizeof(t1), "%lld", (long long)s->edge_count);
        c_put(di, s->edge_start, di_samplenum(di), s->out_ann, ANN_EDGE_COUNT, t1);
        s->edge_start = di_samplenum(di);

        int64_t word_edge_count = s->edge_count - s->edge_off;
        if (s->divider && (word_edge_count % s->divider) == 0) {
            s->word_count++;
            snprintf(t1, sizeof(t1), "%lld", (long long)s->word_count);
            c_put(di, s->word_start, di_samplenum(di), s->out_ann, ANN_WORD_COUNT, t1);
            s->word_start = di_samplenum(di);
        }
    }
}

static void counter_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder counter_c_decoder = {
    .id = "counter_c",
    .name = "Counter(C)",
    .longname = "Edge counter (C)",
    .desc = "Count the number of edges in a signal. (C implementation)",
    .license = "gplv2+",
    .channels = counter_channels,
    .num_channels = 1,
    .optional_channels = counter_optional_channels,
    .num_optional_channels = 1,
    .options = counter_options,
    .num_options = 7,
    .num_annotations = NUM_ANN,
    .ann_labels = counter_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = counter_ann_rows,
    .inputs = counter_inputs,
    .num_inputs = 1,
    .outputs = counter_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = counter_tags,
    .num_tags = 1,
    .reset = counter_reset,
    .start = counter_start,
    .decode = counter_decode,
    .destroy = counter_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GSList *data_edge_vals = NULL;
    data_edge_vals = g_slist_append(data_edge_vals, g_variant_new_string("any"));
    data_edge_vals = g_slist_append(data_edge_vals, g_variant_new_string("rising"));
    data_edge_vals = g_slist_append(data_edge_vals, g_variant_new_string("falling"));
    counter_options[0].id = "data_edge";
    counter_options[0].idn = "dec_counter_opt_data_edge";
    counter_options[0].desc = "Edges to count (data)";
    counter_options[0].def = g_variant_new_string("any");
    counter_options[0].values = data_edge_vals;

    counter_options[1].id = "divider";
    counter_options[1].idn = "dec_counter_opt_divider";
    counter_options[1].desc = "Count divider (word width)";
    counter_options[1].def = g_variant_new_int64(0);

    GSList *reset_edge_vals = NULL;
    reset_edge_vals = g_slist_append(reset_edge_vals, g_variant_new_string("rising"));
    reset_edge_vals = g_slist_append(reset_edge_vals, g_variant_new_string("falling"));
    counter_options[2].id = "reset_edge";
    counter_options[2].idn = "dec_counter_opt_reset_edge";
    counter_options[2].desc = "Edge which clears counters (reset)";
    counter_options[2].def = g_variant_new_string("falling");
    counter_options[2].values = reset_edge_vals;

    counter_options[3].id = "edge_off";
    counter_options[3].idn = "dec_counter_opt_edge_off";
    counter_options[3].desc = "Edge counter value after start/reset";
    counter_options[3].def = g_variant_new_int64(0);

    counter_options[4].id = "word_off";
    counter_options[4].idn = "dec_counter_opt_word_off";
    counter_options[4].desc = "Word counter value after start/reset";
    counter_options[4].def = g_variant_new_int64(0);

    counter_options[5].id = "dead_cycles";
    counter_options[5].idn = "dec_counter_opt_dead_cycles";
    counter_options[5].desc = "Ignore this many edges after reset";
    counter_options[5].def = g_variant_new_int64(0);

    GSList *swr_vals = NULL;
    swr_vals = g_slist_append(swr_vals, g_variant_new_string("no"));
    swr_vals = g_slist_append(swr_vals, g_variant_new_string("yes"));
    counter_options[6].id = "start_with_reset";
    counter_options[6].idn = "dec_counter_opt_start_with_reset";
    counter_options[6].desc = "Assume decode starts with reset";
    counter_options[6].def = g_variant_new_string("no");
    counter_options[6].values = swr_vals;

    return &counter_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}