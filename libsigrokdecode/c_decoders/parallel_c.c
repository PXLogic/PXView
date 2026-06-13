#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

#define PARALLEL_MAX_CHANNELS 33
#define PARALLEL_MAX_WORD_ITEMS 256

enum {
    ANN_ITEMS = 0,
    ANN_WORDS,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int have_clock;
    int clock_edge;       /* 0=rising, 1=falling */
    int wordsize;
    int endianness;       /* 0=little, 1=big */
    int num_item_bits;
    int max_connected;

    int idx_channels[PARALLEL_MAX_CHANNELS];   /* channel index mapping, -1=not connected */
    int has_channels[PARALLEL_MAX_CHANNELS];   /* connected channel list */
    int num_has_channels;

    uint64_t prv_dex;
    uint64_t saved_item;
    int has_saved_item;

    uint64_t items[PARALLEL_MAX_WORD_ITEMS];
    int item_count;
    uint64_t saved_word;
    int has_saved_word;
    uint64_t ss_word;
    uint64_t es_word;

    int first;
    int is_first_wait;

    int out_ann;
    int out_python;
} parallel_state;

static struct srd_channel parallel_optional_channels[PARALLEL_MAX_CHANNELS];
static int parallel_optional_channels_initialized = 0;

static struct srd_decoder_option parallel_options[] = {
    {"clock_edge", NULL, "Clock edge to sample on", NULL, NULL},
    {"wordsize", NULL, "Data wordsize (# bus cycles)", NULL, NULL},
    {"endianness", NULL, "Data endianness", NULL, NULL},
};

static const char *parallel_ann_labels[][3] = {
    {"", "items", "Items"},
    {"", "words", "Words"},
};

static const int parallel_row_items_classes[] = {ANN_ITEMS, -1};
static const int parallel_row_words_classes[] = {ANN_WORDS, -1};
static const struct srd_c_ann_row parallel_ann_rows[] = {
    {"items", "Items", parallel_row_items_classes, 1},
    {"words", "Words", parallel_row_words_classes, 1},
};

static const char *parallel_inputs[] = {"logic"};
static const char *parallel_outputs[] = {"parallel"};
static const char *parallel_tags[] = {"Util"};

static uint64_t parallel_bitpack(struct srd_decoder_inst *di,
                                  parallel_state *s,
                                  uint64_t samplenum)
{
    (void)samplenum;
    uint64_t item = 0;
    int idx_strip = s->max_connected + 1;
    for (int i = 1; i < idx_strip; i++) {
        if (s->idx_channels[i] != -1) {
            int pin = c_pin(di, s->idx_channels[i]);
            item |= ((uint64_t)pin << (i - 1));
        }
        /* Not-connected channels are treated as 0 */
    }
    return item;
}

static void parallel_handle_word(struct srd_decoder_inst *di,
                                  parallel_state *s,
                                  uint64_t item, uint64_t cur_dex)
{
    /* If a word was previously accumulated, emit its annotation */
    if (s->has_saved_word) {
        if (s->wordsize > 0) {
            s->es_word = cur_dex;
            int num_word_bits = s->num_item_bits * s->wordsize;
            int num_digits = (num_word_bits + 3) / 4;
            char fmt[32];
            snprintf(fmt, sizeof(fmt), "@%%0%dX", num_digits);
            char word_str[64];
            snprintf(word_str, sizeof(word_str), fmt, s->saved_word);
            c_put(di, s->ss_word, s->es_word, s->out_ann, ANN_WORDS, word_str);
            /* Python output */
            
            
            c_proto(di, s->ss_word, s->es_word, s->out_python,
                                "WORD", C_U64(s->saved_word), C_U16(s->num_item_bits), C_U16(s->wordsize), C_END);
        }
        s->has_saved_word = 0;
    }

    if (item == (uint64_t)-1)
        return;

    /* Accumulate items for word assembly */
    if (s->item_count == 0)
        s->ss_word = cur_dex;

    s->items[s->item_count] = item;
    s->item_count++;

    if (s->item_count < s->wordsize)
        return;

    /* Assemble word from items */
    uint64_t word = 0;
    if (s->endianness == 1) {
        /* Big endian: reverse items */
        for (int i = 0; i < s->wordsize; i++)
            word |= (s->items[s->wordsize - 1 - i] << (i * s->num_item_bits));
    } else {
        /* Little endian */
        for (int i = 0; i < s->wordsize; i++)
            word |= (s->items[i] << (i * s->num_item_bits));
    }
    s->saved_word = word;
    s->has_saved_word = 1;
    s->item_count = 0;
}

static void parallel_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(parallel_state)));
    parallel_state *s = (parallel_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(parallel_state));
    for (int i = 0; i < PARALLEL_MAX_CHANNELS; i++)
        s->idx_channels[i] = -1;
    s->is_first_wait = 1;
}

static void parallel_start(struct srd_decoder_inst *di)
{
    parallel_state *s = (parallel_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "parallel");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "parallel");
    s->samplerate = c_samplerate(di);

    const char *ce = c_opt_str(di, "clock_edge", "rising");
    s->clock_edge = (strcmp(ce, "falling") == 0) ? 1 : 0;

    s->wordsize = (int)c_opt_int(di, "wordsize", 0);
    if (s->wordsize < 0)
        s->wordsize = 0;

    const char *en = c_opt_str(di, "endianness", "little");
    s->endianness = (strcmp(en, "big") == 0) ? 1 : 0;

    /* Determine which optional channels are connected */
    s->num_has_channels = 0;
    s->max_connected = -1;
    for (int i = 0; i < PARALLEL_MAX_CHANNELS; i++) {
        if (c_has_ch(di, i)) {
            s->idx_channels[i] = i;
            s->has_channels[s->num_has_channels++] = i;
            if (i > s->max_connected)
                s->max_connected = i;
        } else {
            s->idx_channels[i] = -1;
        }
    }

    s->have_clock = c_has_ch(di, 0);
    s->num_item_bits = s->max_connected; /* max_connected is the highest index; data lines start at 1 */
    s->first = 1;
}

static void parallel_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    parallel_state *s = (parallel_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void parallel_decode(struct srd_decoder_inst *di)
{
    parallel_state *s = (parallel_state *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate == 0) return;
    }

    if (s->num_has_channels == 0)
        return;

    while (1) {
        if (s->have_clock) {
            if (s->clock_edge == 0)
                ret = c_wait(di, CW_R(0), CW_END);
            else
                ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
        } else {
            if (s->is_first_wait) {
                /* No clock first wait: get initial values */
                ret = c_wait(di, CW_END);
                if (ret != SRD_OK) return;
            } else {
                /* Wait for edge on any connected data channel */
                GSList *or_groups = NULL;
                for (int i = 0; i < s->num_has_channels; i++) {
                    GSList *and_group = NULL;
                    struct srd_term *t = g_malloc0(sizeof(struct srd_term));
                    t->type = SRD_TERM_EITHER_EDGE;
                    t->channel = s->has_channels[i];
                    and_group = g_slist_append(and_group, t);
                    or_groups = g_slist_append(or_groups, and_group);
                }
                ret = c_decoder_wait(di, or_groups, NULL, NULL);
                /* NOTE: c_decoder_wait_impl takes ownership of or_groups;
                 * do NOT free it here to avoid double-free. */
                if (ret != SRD_OK) return;
            }
        }

        uint64_t item = parallel_bitpack(di, s, di_samplenum(di));

        if (!s->have_clock && s->is_first_wait) {
            s->is_first_wait = 0;
            s->saved_item = item;
            s->has_saved_item = 1;
            s->prv_dex = di_samplenum(di);
            continue;
        }

        /* Output saved item */
        if (s->has_saved_item) {
            char item_str[64];
            if (s->num_item_bits <= 8 && s->saved_item <= 0xFF) {
                unsigned char ch = (unsigned char)s->saved_item;
                if (ch >= 0x20 && ch <= 0x7E) {
                    snprintf(item_str, sizeof(item_str), "%c", ch);
                } else {
                    switch (ch) {
                    case '\0': snprintf(item_str, sizeof(item_str), "\\0"); break;
                    case '\a': snprintf(item_str, sizeof(item_str), "\\a"); break;
                    case '\b': snprintf(item_str, sizeof(item_str), "\\b"); break;
                    case '\t': snprintf(item_str, sizeof(item_str), "\\t"); break;
                    case '\n': snprintf(item_str, sizeof(item_str), "\\n"); break;
                    case '\v': snprintf(item_str, sizeof(item_str), "\\v"); break;
                    case '\f': snprintf(item_str, sizeof(item_str), "\\f"); break;
                    case '\r': snprintf(item_str, sizeof(item_str), "\\r"); break;
                    default:   snprintf(item_str, sizeof(item_str), "\\x%02x", ch); break;
                    }
                }
            } else {
                int num_digits = (s->num_item_bits + 3) / 4;
                char fmt[32];
                snprintf(fmt, sizeof(fmt), "@%%0%dX", num_digits);
                snprintf(item_str, sizeof(item_str), fmt, s->saved_item);
            }
            c_put(di, s->prv_dex, di_samplenum(di), s->out_ann, ANN_ITEMS, item_str);
            /* Python output */
            
            
            c_proto(di, s->prv_dex, di_samplenum(di), s->out_python,
                                "ITEM", C_U64(s->saved_item), C_U16(s->num_item_bits), C_END);
        }

        s->saved_item = item;
        s->has_saved_item = 1;
        s->prv_dex = di_samplenum(di);

        /* Handle word assembly */
        parallel_handle_word(di, s, item, di_samplenum(di));
    }
}

static void parallel_end(struct srd_decoder_inst *di)
{
    parallel_state *s = (parallel_state *)c_decoder_get_private(di);
    if (!s || !s->has_saved_item) return;

    uint64_t last_sample = c_last_samplenum(di);
    char item_str[64];
    if (s->num_item_bits <= 8 && s->saved_item <= 0xFF) {
        unsigned char ch = (unsigned char)s->saved_item;
        if (ch >= 0x20 && ch <= 0x7E) {
            snprintf(item_str, sizeof(item_str), "%c", ch);
        } else {
            switch (ch) {
            case '\0': snprintf(item_str, sizeof(item_str), "\\0"); break;
            case '\a': snprintf(item_str, sizeof(item_str), "\\a"); break;
            case '\b': snprintf(item_str, sizeof(item_str), "\\b"); break;
            case '\t': snprintf(item_str, sizeof(item_str), "\\t"); break;
            case '\n': snprintf(item_str, sizeof(item_str), "\\n"); break;
            case '\v': snprintf(item_str, sizeof(item_str), "\\v"); break;
            case '\f': snprintf(item_str, sizeof(item_str), "\\f"); break;
            case '\r': snprintf(item_str, sizeof(item_str), "\\r"); break;
            default:   snprintf(item_str, sizeof(item_str), "\\x%02x", ch); break;
            }
        }
    } else {
        int num_digits = (s->num_item_bits + 3) / 4;
        char fmt[32];
        snprintf(fmt, sizeof(fmt), "@%%0%dX", num_digits);
        snprintf(item_str, sizeof(item_str), fmt, s->saved_item);
    }
    c_put(di, s->prv_dex, last_sample, s->out_ann, ANN_ITEMS, item_str);
    
    
    c_proto(di, s->prv_dex, last_sample, s->out_python,
                        "ITEM", C_U64(s->saved_item), C_U16(s->num_item_bits), C_END);
    s->has_saved_item = 0;
}

static void parallel_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder parallel_c_decoder = {
    .id = "parallel_c",
    .name = "Parallel(C)",
    .longname = "Parallel sync bus (C)",
    .desc = "Generic parallel synchronous bus. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = parallel_optional_channels,
    .num_optional_channels = PARALLEL_MAX_CHANNELS,
    .options = parallel_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = parallel_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = parallel_ann_rows,
    .inputs = parallel_inputs,
    .num_inputs = 1,
    .outputs = parallel_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = parallel_tags,
    .num_tags = 1,
    .reset = parallel_reset,
    .start = parallel_start,
    .metadata = parallel_metadata,
    .decode = parallel_decode,
    .end = parallel_end,
    .destroy = parallel_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* Initialize optional channels dynamically */
    if (!parallel_optional_channels_initialized) {
        parallel_optional_channels[0].id = "clk";
        parallel_optional_channels[0].name = "CLK";
        parallel_optional_channels[0].desc = "Clock line";
        parallel_optional_channels[0].order = 0;
        parallel_optional_channels[0].type = SRD_CHANNEL_SCLK;
        parallel_optional_channels[0].idn = NULL;
        for (int i = 0; i < 32; i++) {
            char *id_buf = g_strdup_printf("d%d", i);
            char *name_buf = g_strdup_printf("D%d", i);
            char *desc_buf = g_strdup_printf("Data line %d", i);
            parallel_optional_channels[i + 1].id = id_buf;
            parallel_optional_channels[i + 1].name = name_buf;
            parallel_optional_channels[i + 1].desc = desc_buf;
            parallel_optional_channels[i + 1].order = i + 1;
            parallel_optional_channels[i + 1].type = SRD_CHANNEL_SDATA;
            parallel_optional_channels[i + 1].idn = NULL;
        }
        parallel_optional_channels_initialized = 1;
    }

    /* Initialize options */
    GSList *ce_vals = NULL;
    ce_vals = g_slist_append(ce_vals, g_variant_new_string("rising"));
    ce_vals = g_slist_append(ce_vals, g_variant_new_string("falling"));
    parallel_options[0].id = "clock_edge";
    parallel_options[0].idn = "dec_parallel_opt_clock_edge";
    parallel_options[0].desc = "Clock edge to sample on";
    parallel_options[0].def = g_variant_new_string("rising");
    parallel_options[0].values = ce_vals;

    parallel_options[1].id = "wordsize";
    parallel_options[1].idn = "dec_parallel_opt_wordsize";
    parallel_options[1].desc = "Data wordsize (# bus cycles)";
    parallel_options[1].def = g_variant_new_int64(0);

    GSList *en_vals = NULL;
    en_vals = g_slist_append(en_vals, g_variant_new_string("little"));
    en_vals = g_slist_append(en_vals, g_variant_new_string("big"));
    parallel_options[2].id = "endianness";
    parallel_options[2].idn = "dec_parallel_opt_endianness";
    parallel_options[2].desc = "Data endianness";
    parallel_options[2].def = g_variant_new_string("little");
    parallel_options[2].values = en_vals;

    return &parallel_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}