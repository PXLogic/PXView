#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum wiegand_state {
    STATE_NONE = -1,
    STATE_IDLE = 0,
    STATE_DATA,
    STATE_INVALID,
};

enum wiegand_ann {
    ANN_BITS = 0,
    ANN_STATE,
    NUM_ANN,
};

#define CH_D0 0
#define CH_D1 1
#define MAX_BITS 64

struct wiegand_priv {
    uint64_t samplerate;
    uint64_t samples_per_bit;
    int d0_prev;
    int d1_prev;
    int state;
    uint64_t ss_state;
    uint64_t ss_bit;
    uint64_t es_bit;
    int cur_bit;
    int bits[MAX_BITS];
    int num_bits;
    int active;
    int inactive;
    int out_ann;
};

static struct srd_channel wiegand_channels[] = {
    {"d0", "D0", "Data 0 line", 0, SRD_CHANNEL_SDATA, "dec_wiegand_chan_d0"},
    {"d1", "D1", "Data 1 line", 1, SRD_CHANNEL_SDATA, "dec_wiegand_chan_d1"},
};

static struct srd_decoder_option wiegand_options_arr[2];

static const char *wiegand_ann_labels[][3] = {
    {"", "bits", "Bits"},
    {"", "state", "State"},
};

static const int wiegand_row_bits_classes[] = {ANN_BITS};
static const int wiegand_row_state_classes[] = {ANN_STATE};
static const struct srd_c_ann_row wiegand_ann_rows[] = {
    {"bits", "Binary value", wiegand_row_bits_classes, 1},
    {"state", "Stream state", wiegand_row_state_classes, 1},
};

static const char *wiegand_inputs[] = {"logic", NULL};
static const char *wiegand_outputs[] = {NULL};
static const char *wiegand_tags[] = {"Embedded/industrial", "RFID", NULL};

static void wiegand_update_state(struct srd_decoder_inst *di, struct wiegand_priv *s,
    uint64_t samplenum, int new_state, int bit)
{
    if (s->cur_bit != -1) {
        if (s->num_bits < MAX_BITS) {
            s->bits[s->num_bits] = s->cur_bit;
            s->num_bits++;
        }
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->cur_bit);
        c_put(di, s->ss_bit, samplenum, s->out_ann, ANN_BITS, bit_str);
    }

    s->cur_bit = bit;
    s->ss_bit = samplenum;
    if (bit != -1)
        s->es_bit = samplenum + s->samples_per_bit;
    else
        s->es_bit = 0;

    if (new_state != s->state) {
        if (s->state == STATE_DATA) {
            char accum_str[MAX_BITS + 1];
            int i;
            for (i = 0; i < s->num_bits; i++)
                accum_str[i] = '0' + s->bits[i];
            accum_str[s->num_bits] = '\0';

            char s1[80];
            char s2[32];
            snprintf(s1, sizeof(s1), "%d bits %s", s->num_bits, accum_str);
            snprintf(s2, sizeof(s2), "%d bits", s->num_bits);
            c_put(di, s->ss_state, samplenum, s->out_ann, ANN_STATE, s1, s2);
        } else if (s->state == STATE_INVALID) {
            c_put(di, s->ss_state, samplenum, s->out_ann, ANN_STATE, "invalid");
        }

        s->ss_state = samplenum;
        s->state = new_state;
        s->num_bits = 0;
    }
}

static void wiegand_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct wiegand_priv)));
    }
    struct wiegand_priv *s = (struct wiegand_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct wiegand_priv));
    s->d0_prev = -1;
    s->d1_prev = -1;
    s->state = STATE_NONE;
    s->cur_bit = -1;
    s->es_bit = 0;
    s->samples_per_bit = 10;
}

static void wiegand_start(struct srd_decoder_inst *di)
{
    struct wiegand_priv *s = (struct wiegand_priv *)c_decoder_get_private(di);

    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "wiegand");

    const char *active_str = c_opt_str(di, "active", "low");
    if (active_str && strcmp(active_str, "high") == 0) {
        s->active = 1;
        s->inactive = 0;
    } else {
        s->active = 0;
        s->inactive = 1;
    }
}

static void wiegand_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct wiegand_priv *s = (struct wiegand_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->samplerate) {
            int bitwidth_ms = (int)c_opt_int(di, "bitwidth_ms", 4);
            double ms_per_sample = 1000.0 / (double)s->samplerate;
            double ms_per_bit = (double)bitwidth_ms;
            int spb = (int)(ms_per_bit / ms_per_sample);
            s->samples_per_bit = (uint64_t)(spb < 1 ? 1 : spb);
        }
    }
}

static void wiegand_decode(struct srd_decoder_inst *di)
{
    struct wiegand_priv *s = (struct wiegand_priv *)c_decoder_get_private(di);

    if (!s->samplerate)
        return;

    /* Grab first sample to initialize previous pin states */
    int ret = c_wait(di, CW_SKIP(0), CW_END);
    if (ret != SRD_OK)
        return;

    s->d0_prev = c_pin(di, CH_D0);
    s->d1_prev = c_pin(di, CH_D1);

    int loop_count = 0;
    while (1) {
        if (loop_count++ > 100000) {
            fprintf(stderr, "wiegand_decode infinite loop detected!\n");
            break;
        }

        /* Wait for edge on D0 or D1, or for bit timeout */
        if (s->es_bit && s->es_bit > di_samplenum(di)) {
            uint64_t skip = s->es_bit - di_samplenum(di);
            ret = c_wait(di, CW_E(CH_D0), CW_OR, CW_E(CH_D1), CW_OR, CW_SKIP(skip), CW_END);
        } else {
            ret = c_wait(di, CW_E(CH_D0), CW_OR, CW_E(CH_D1), CW_END);
        }
        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);
        int d0 = c_pin(di, CH_D0);
        int d1 = c_pin(di, CH_D1);

        fprintf(stderr, "wiegand samplenum=%llu, d0=%d, d1=%d, es_bit=%llu, ret=%d\n", (unsigned long long)samplenum, d0, d1, (unsigned long long)s->es_bit, ret);


        /* Check for bit timeout (es_bit reached without pin change) */
        if (s->es_bit && samplenum >= s->es_bit) {
            if (d0 == s->inactive && d1 == s->inactive) {
                wiegand_update_state(di, s, samplenum, STATE_IDLE, -1);
            } else {
                wiegand_update_state(di, s, samplenum, STATE_INVALID, -1);
            }
        }

        /* Skip if pins haven't changed */
        if (d0 == s->d0_prev && d1 == s->d1_prev)
            continue;

        if (s->state == STATE_NONE || s->state == STATE_IDLE || s->state == STATE_DATA) {
            if (d0 == s->active && d1 == s->inactive) {
                wiegand_update_state(di, s, samplenum, STATE_DATA, 0);
            } else if (d0 == s->inactive && d1 == s->active) {
                wiegand_update_state(di, s, samplenum, STATE_DATA, 1);
            } else if (d0 == s->active && d1 == s->active) {
                wiegand_update_state(di, s, samplenum, STATE_INVALID, -1);
            }
        } else if (s->state == STATE_INVALID) {
            if (d0 == s->inactive && d1 == s->inactive) {
                wiegand_update_state(di, s, samplenum, STATE_IDLE, -1);
            }
        }

        s->d0_prev = d0;
        s->d1_prev = d1;
    }
}

static void wiegand_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder wiegand_c_decoder = {
    .id = "wiegand_c",
    .name = "Wiegand(C)",
    .longname = "Wiegand interface (C)",
    .desc = "Wiegand interface for electronic entry systems (C implementation)",
    .license = "gplv2+",
    .channels = wiegand_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = wiegand_options_arr,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = wiegand_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = wiegand_ann_rows,
    .inputs = wiegand_inputs,
    .num_inputs = 1,
    .outputs = wiegand_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = wiegand_tags,
    .num_tags = 2,
    .reset = wiegand_reset,
    .start = wiegand_start,
    .decode = wiegand_decode,
    .end = NULL,
    .metadata = wiegand_metadata,
    .destroy = wiegand_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *active_vals[] = {
        g_variant_new_string("low"),
        g_variant_new_string("high"),
    };
    GSList *active_list = NULL;
    active_list = g_slist_append(active_list, active_vals[0]);
    active_list = g_slist_append(active_list, active_vals[1]);
    wiegand_options_arr[0].id = "active";
    wiegand_options_arr[0].idn = "dec_wiegand_opt_active";
    wiegand_options_arr[0].desc = "Data lines active level";
    wiegand_options_arr[0].def = g_variant_new_string("low");
    wiegand_options_arr[0].values = active_list;

    GSList *bitwidth_list = NULL;
    bitwidth_list = g_slist_append(bitwidth_list, g_variant_new_int64(1));
    bitwidth_list = g_slist_append(bitwidth_list, g_variant_new_int64(2));
    bitwidth_list = g_slist_append(bitwidth_list, g_variant_new_int64(4));
    bitwidth_list = g_slist_append(bitwidth_list, g_variant_new_int64(8));
    bitwidth_list = g_slist_append(bitwidth_list, g_variant_new_int64(16));
    bitwidth_list = g_slist_append(bitwidth_list, g_variant_new_int64(32));
    wiegand_options_arr[1].id = "bitwidth_ms";
    wiegand_options_arr[1].idn = "dec_wiegand_opt_bitwidth_ms";
    wiegand_options_arr[1].desc = "Single bit width in milliseconds";
    wiegand_options_arr[1].def = g_variant_new_int64(4);
    wiegand_options_arr[1].values = bitwidth_list;

    return &wiegand_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}