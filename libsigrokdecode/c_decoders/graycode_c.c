/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * Gray code and rotary encoder protocol decoder (C implementation).
 * Ported from Python decoder graycode.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 8
#define MAX_AVG_WINDOW 1024

enum {
    ANN_PHASE = 0,
    ANN_INCREMENT,
    ANN_COUNT,
    ANN_TURNS,
    ANN_INTERVAL,
    ANN_AVERAGE,
    ANN_RPM,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int num_channels;
    int encoder_steps;
    int edges_per_rotation;
    int avg_period;
    int prev_gray;
    int prev_bin;
    int64_t count;
    uint64_t last_edge_sample;
    int out_ann;
    int bFirst;
    int prev_increment;  /* Previous increment value for Value class change-detection */
    int increment_initialized; /* Whether increment has been set before */
    int64_t prev_count;  /* Previous count value for Value class change-detection */
    int count_initialized; /* Whether count has been set before */
    int64_t prev_turns;  /* Previous turns value for Value class change-detection */
    int turns_initialized; /* Whether turns has been set before */
    uint64_t count_timestamp;     /* Sample at which current count value was first set */
    uint64_t increment_timestamp; /* Sample at which current increment value was first set */
    uint64_t turns_timestamp;     /* Sample at which current turns value was first set */
    /* SI prefix */
    int si_prefix_exp; /* 0='', 3='k', 6='M', 9='G', 12='T' */
    /* Sliding window for averaging */
    struct {
        int abs_delta;
        double period;
    } avg_window[MAX_AVG_WINDOW];
    int avg_window_head;
    int avg_window_count;
} gray_code_state;

static uint32_t gray_to_binary(uint32_t gray, int bits)
{
    uint32_t bin = gray;
    for (int i = 1; i < bits; i++)
        bin ^= (gray >> i);
    return bin;
}

static int bitpack_pins(struct srd_decoder_inst* di, int num_channels, uint64_t samplenum)
{
    (void)samplenum;
    (void)samplenum;
    int val = 0;
    for (int i = 0; i < num_channels; i++) {
        if (c_pin(di, i))
            val |= (1 << i);
    }
    return val;
}

static const char* si_prefix_for_exp(int exp)
{
    switch (exp) {
        case -9: return "n";
        case -6: return "µ";
        case -3: return "m";
        case 0:  return "";
        case 3:  return "k";
        case 6:  return "M";
        case 9:  return "G";
        case 12: return "T";
        default: return "";
    }
}

static void format_si_value(char* buf, int bufsize, double value, int forced_exp)
{
    int sgn = (value > 0) - (value < 0);
    value = fabs(value);
    double p = value > 0 ? log10(value) : 0;
    /* Match Python: round value first, then compute SI exponent */
    value = sgn * floor(value * pow(10.0, 3 - p)) * pow(10.0, -(3 - p));
    int e = ((int)floor(p / 3.0)) * 3;
    if (forced_exp >= 0 && e < forced_exp)
        e = forced_exp;
    if (e < -9) e = -9;
    if (e > 9) e = 9;
    value *= pow(10.0, -e);
    p -= e;
    int decimals = 2 - (int)p;
    if (decimals < 0) decimals = 0;
    if (decimals > 3) decimals = 3;
    snprintf(buf, bufsize, "%.*f %s", decimals, value, si_prefix_for_exp(e));
}

static struct srd_decoder_option gray_code_options[] = {
    { "edges", NULL, "Edges per rotation", NULL, NULL },
    { "avg_period", NULL, "Averaging period", NULL, NULL },
    { "bits", NULL, "Number of Gray code bits", NULL, NULL },
    { "si_prefix", NULL, "SI prefix for interval/rate display", NULL, NULL },
};

static const char* gray_code_inputs[] = { "logic", NULL };
static const char* gray_code_outputs[] = { NULL };
static const char* gray_code_tags[] = { "Encoding", NULL };

static struct srd_channel gray_code_optional_channels[] = {
    { "d0", "D0", "Data line 0", 0, SRD_CHANNEL_COMMON, NULL },
    { "d1", "D1", "Data line 1", 1, SRD_CHANNEL_COMMON, NULL },
    { "d2", "D2", "Data line 2", 2, SRD_CHANNEL_COMMON, NULL },
    { "d3", "D3", "Data line 3", 3, SRD_CHANNEL_COMMON, NULL },
    { "d4", "D4", "Data line 4", 4, SRD_CHANNEL_COMMON, NULL },
    { "d5", "D5", "Data line 5", 5, SRD_CHANNEL_COMMON, NULL },
    { "d6", "D6", "Data line 6", 6, SRD_CHANNEL_COMMON, NULL },
    { "d7", "D7", "Data line 7", 7, SRD_CHANNEL_COMMON, NULL },
};

static const char* gray_code_ann_labels[][3] = {
    { "", "phase", "Phase" },
    { "", "increment", "Increment" },
    { "", "count", "Count" },
    { "", "turns", "Turns" },
    { "", "interval", "Interval" },
    { "", "average", "Average" },
    { "", "rpm", "Rate" },
};

static const int gray_code_row_phase_classes[] = { ANN_PHASE, -1 };
static const int gray_code_row_increment_classes[] = { ANN_INCREMENT, -1 };
static const int gray_code_row_count_classes[] = { ANN_COUNT, -1 };
static const int gray_code_row_turns_classes[] = { ANN_TURNS, -1 };
static const int gray_code_row_interval_classes[] = { ANN_INTERVAL, -1 };
static const int gray_code_row_average_classes[] = { ANN_AVERAGE, -1 };
static const int gray_code_row_rpm_classes[] = { ANN_RPM, -1 };
static const struct srd_c_ann_row gray_code_ann_rows[] = {
    { "phase", "Phase", gray_code_row_phase_classes, 1 },
    { "increment", "Increment", gray_code_row_increment_classes, 1 },
    { "count", "Count", gray_code_row_count_classes, 1 },
    { "turns", "Turns", gray_code_row_turns_classes, 1 },
    { "interval", "Interval", gray_code_row_interval_classes, 1 },
    { "average", "Average", gray_code_row_average_classes, 1 },
    { "rpm", "Rate", gray_code_row_rpm_classes, 1 },
};

static void graycode_metadata(struct srd_decoder_inst* di, int key, uint64_t value)
{
    gray_code_state* s = (gray_code_state*)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void gray_code_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(gray_code_state)));
    }
    gray_code_state* s = (gray_code_state*)c_decoder_get_private(di);
    memset(s, 0, sizeof(gray_code_state));
    s->prev_gray = -1;
    s->prev_bin = -1;
    s->si_prefix_exp = -100;
    s->bFirst = 1;
}

static int get_si_prefix_exp(const char* prefix)
{
    if (!prefix || prefix[0] == '\0') return -100;
    if (strcmp(prefix, "k") == 0) return 3;
    if (strcmp(prefix, "M") == 0) return 6;
    if (strcmp(prefix, "G") == 0) return 9;
    if (strcmp(prefix, "T") == 0) return 12;
    return -100;
}

static void gray_code_start(struct srd_decoder_inst* di)
{
    gray_code_state* s = (gray_code_state*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "graycode");
    s->samplerate = c_samplerate(di);
    s->edges_per_rotation = (int)c_opt_int(di, "edges", 0);
    s->avg_period = (int)c_opt_int(di, "avg_period", 10);
    if (s->avg_period > MAX_AVG_WINDOW)
        s->avg_period = MAX_AVG_WINDOW;

    int bits_opt = (int)c_opt_int(di, "bits", 0);

    /* Determine number of active channels */
    int num_ch = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (c_has_ch(di, i))
            num_ch++;
        else
            break;
    }

    /* bits option overrides if set, otherwise use detected channels */
    if (bits_opt > 0 && bits_opt <= MAX_CHANNELS)
        s->num_channels = bits_opt;
    else
        s->num_channels = num_ch;

    if (s->num_channels < 1)
        s->num_channels = 1;

    s->encoder_steps = 1 << s->num_channels;

    /* SI prefix */
    const char* si_str = c_opt_str(di, "si_prefix", "");
    s->si_prefix_exp = get_si_prefix_exp(si_str);
}

static void gray_code_decode(struct srd_decoder_inst* di)
{
    gray_code_state* s = (gray_code_state*)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
    }

    /* Read initial sample at current position, like Python's self.wait() */
    {
        if (c_wait(di, CW_END) == SRD_OK) {
            uint64_t cur_sample = di_samplenum(di);
            int cur_gray = bitpack_pins(di, s->num_channels, cur_sample);
            s->prev_gray = cur_gray;
            s->prev_bin = (int)gray_to_binary((uint32_t)cur_gray, s->num_channels);
            s->last_edge_sample = cur_sample;
            s->bFirst = 0;
            /* Initialize Value class timestamps, matching Python's Value.set(timestamp, 0) */
            s->count_timestamp = cur_sample;
            s->count_initialized = 1;  /* count has been "set" to 0 */
            s->increment_timestamp = cur_sample;
            /* increment_initialized remains 0 (increment not set yet, like Python's None) */
            if (s->edges_per_rotation > 0) {
                s->turns_timestamp = cur_sample;
                s->turns_initialized = 1;  /* turns has been "set" to 0 */
            }
        }
    }

    while (1) {
        /* Wait for edge on any of the active channels */
        int ret;
        switch (s->num_channels) {
        case 1:  ret = c_wait(di, CW_E(0), CW_END); break;
        case 2:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_END); break;
        case 3:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_OR, CW_E(2), CW_END); break;
        case 4:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_OR, CW_E(2), CW_OR, CW_E(3), CW_END); break;
        case 5:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_OR, CW_E(2), CW_OR, CW_E(3), CW_OR, CW_E(4), CW_END); break;
        case 6:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_OR, CW_E(2), CW_OR, CW_E(3), CW_OR, CW_E(4), CW_OR, CW_E(5), CW_END); break;
        case 7:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_OR, CW_E(2), CW_OR, CW_E(3), CW_OR, CW_E(4), CW_OR, CW_E(5), CW_OR, CW_E(6), CW_END); break;
        case 8:  ret = c_wait(di, CW_E(0), CW_OR, CW_E(1), CW_OR, CW_E(2), CW_OR, CW_E(3), CW_OR, CW_E(4), CW_OR, CW_E(5), CW_OR, CW_E(6), CW_OR, CW_E(7), CW_END); break;
        default: ret = c_wait(di, CW_END); break;
        }
        if (ret != SRD_OK)
            return;

        int cur_gray = bitpack_pins(di, s->num_channels, di_samplenum(di));

        if (s->bFirst) {
            s->bFirst = 0;
            s->prev_gray = cur_gray;
            s->prev_bin = (int)gray_to_binary((uint32_t)cur_gray, s->num_channels);
            s->last_edge_sample = di_samplenum(di);
            continue;
        }

        if (cur_gray == s->prev_gray)
            continue;

        int cur_bin = (int)gray_to_binary((uint32_t)cur_gray, s->num_channels);
        int old_bin = s->prev_bin;

        /* Save old values for annotation (Python outputs old values, not new) */

        /* Python: phasedelta_raw = (newphase - oldphase + (ENCODER_STEPS // 2 - 1)) % ENCODER_STEPS - (ENCODER_STEPS // 2 - 1) */
        int phasedelta_raw = (cur_bin - old_bin + (s->encoder_steps / 2 - 1)) % s->encoder_steps - (s->encoder_steps / 2 - 1);
        int phasedelta = phasedelta_raw;

        /* If ambiguous (jump of exactly half), treat as zero */
        if (abs(phasedelta) == s->encoder_steps / 2)
            phasedelta = 0;

        /* Compute new count BEFORE annotations (matching Python's Value.set order) */
        int64_t new_count = s->count + phasedelta;

        /* Phase annotation - show old value (valid during this period) */
        char t1[150];
        snprintf(t1, sizeof(t1), "%d", old_bin);
        c_put(di, s->last_edge_sample, di_samplenum(di), s->out_ann, ANN_PHASE, t1);

        /* Increment annotation - match Python's Value class behavior:
         * Value.set() only fires onchange when newval != self.value AND self.value is not None.
         * The first call (self.value is None) updates value but doesn't fire onchange.
         * Subsequent calls only fire when the value changes, outputting the OLD value. */
        if (s->increment_initialized && phasedelta_raw != s->prev_increment) {
            /* Output the OLD increment value (vold in Python's on_increment) */
            if (s->prev_increment == 0)
                snprintf(t1, sizeof(t1), "0");
            else if (abs(s->prev_increment) == s->encoder_steps / 2)
                snprintf(t1, sizeof(t1), "±π");
            else
                snprintf(t1, sizeof(t1), "%+d", s->prev_increment);
            c_put(di, s->increment_timestamp, di_samplenum(di), s->out_ann, ANN_INCREMENT, t1);
        }
        if (!s->increment_initialized || phasedelta_raw != s->prev_increment) {
            s->increment_timestamp = di_samplenum(di);
        }
        s->prev_increment = phasedelta_raw;
        s->increment_initialized = 1;

        /* Count - match Python's Value class: only output when value changes */
        if (s->count_initialized && new_count != s->count) {
            snprintf(t1, sizeof(t1), "%lld", (long long)s->count);
            c_put(di, s->count_timestamp, di_samplenum(di), s->out_ann, ANN_COUNT, t1);
        }
        if (new_count != s->count) {
            s->count_timestamp = di_samplenum(di);
        }
        s->count = new_count;

        /* Turns - match Python's Value class: only output when value changes */
        if (s->edges_per_rotation > 0) {
            int64_t new_turns = new_count / s->edges_per_rotation;
            if (s->turns_initialized && new_turns != s->prev_turns) {
                snprintf(t1, sizeof(t1), "%+lld", (long long)s->prev_turns);
                c_put(di, s->turns_timestamp, di_samplenum(di), s->out_ann, ANN_TURNS, t1);
            }
            if (!s->turns_initialized || new_turns != s->prev_turns) {
                s->turns_timestamp = di_samplenum(di);
            }
            s->prev_turns = new_turns;
            s->turns_initialized = 1;
        }

        /* Interval and rate */
        if (s->samplerate > 0 && di_samplenum(di) > s->last_edge_sample) {
            double period = (double)(di_samplenum(di) - s->last_edge_sample) / s->samplerate;
            double freq = abs(phasedelta_raw) / period;

            /* Interval annotation: "Xs, XHz" */
            char period_str[64], freq_str[64];
            format_si_value(period_str, sizeof(period_str), period, s->si_prefix_exp);
            format_si_value(freq_str, sizeof(freq_str), freq, s->si_prefix_exp);
            snprintf(t1, sizeof(t1), "%ss, %sHz", period_str, freq_str);
            c_put(di, s->last_edge_sample, di_samplenum(di), s->out_ann, ANN_INTERVAL, t1);

            /* Sliding window averaging */
            if (s->avg_period > 0) {
                /* Add to circular buffer */
                int idx = (s->avg_window_head + s->avg_window_count) % MAX_AVG_WINDOW;
                s->avg_window[idx].abs_delta = abs(phasedelta_raw);
                s->avg_window[idx].period = period;
                if (s->avg_window_count < MAX_AVG_WINDOW)
                    s->avg_window_count++;
                else
                    s->avg_window_head = (s->avg_window_head + 1) % MAX_AVG_WINDOW;

                /* Trim to avg_period window size */
                while (s->avg_window_count > s->avg_period) {
                    s->avg_window_head = (s->avg_window_head + 1) % MAX_AVG_WINDOW;
                    s->avg_window_count--;
                }

                /* Compute average period: sum(period) / sum(abs_delta) */
                double sum_period = 0;
                int sum_delta = 0;
                for (int i = 0; i < s->avg_window_count; i++) {
                    int j = (s->avg_window_head + i) % MAX_AVG_WINDOW;
                    sum_period += s->avg_window[j].period;
                    sum_delta += s->avg_window[j].abs_delta;
                }
                double avg_period = sum_period / (sum_delta > 0 ? sum_delta : 1);

                char avg_p_str[64], avg_f_str[64];
                format_si_value(avg_p_str, sizeof(avg_p_str), avg_period, s->si_prefix_exp);
                format_si_value(avg_f_str, sizeof(avg_f_str), 1.0 / avg_period, s->si_prefix_exp);
                snprintf(t1, sizeof(t1), "%ss, %sHz", avg_p_str, avg_f_str);
                c_put(di, s->last_edge_sample, di_samplenum(di), s->out_ann, ANN_AVERAGE, t1);

                /* RPM */
                if (s->edges_per_rotation > 0) {
                    double rpm_freq = 60.0 * freq / s->edges_per_rotation;
                    char rpm_str[64];
                    format_si_value(rpm_str, sizeof(rpm_str), rpm_freq, 0);
                    snprintf(t1, sizeof(t1), "%srpm", rpm_str);
                    c_put(di, s->last_edge_sample, di_samplenum(di), s->out_ann, ANN_RPM, t1);
                }
            }
        }

        s->prev_gray = cur_gray;
        s->prev_bin = cur_bin;
        s->last_edge_sample = di_samplenum(di);
    }
}

static void gray_code_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder gray_code_c_decoder = {
    .id = "graycode_c",
    .name = "Gray Code(C)",
    .longname = "Gray code and rotary encoder (C)",
    .desc = "Accumulate rotary encoder increments, provide statistics. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = gray_code_optional_channels,
    .num_optional_channels = 8,
    .options = gray_code_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = gray_code_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = gray_code_ann_rows,
    .inputs = gray_code_inputs,
    .num_inputs = 1,
    .outputs = gray_code_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = gray_code_tags,
    .num_tags = 1,
    .metadata = graycode_metadata,
    .reset = gray_code_reset,
    .start = gray_code_start,
    .decode = gray_code_decode,
    .destroy = gray_code_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    gray_code_options[0].id = "edges";
    gray_code_options[0].idn = "dec_graycode_opt_edges";
    gray_code_options[0].desc = "Edges per rotation";
    gray_code_options[0].def = g_variant_new_int64(0);

    gray_code_options[1].id = "avg_period";
    gray_code_options[1].idn = "dec_graycode_opt_avg_period";
    gray_code_options[1].desc = "Averaging period";
    gray_code_options[1].def = g_variant_new_int64(10);

    gray_code_options[2].id = "bits";
    gray_code_options[2].idn = "dec_graycode_opt_bits";
    gray_code_options[2].desc = "Number of Gray code bits";
    gray_code_options[2].def = g_variant_new_int64(0);

    GSList* si_vals = NULL;
    si_vals = g_slist_append(si_vals, g_variant_new_string(""));
    si_vals = g_slist_append(si_vals, g_variant_new_string("k"));
    si_vals = g_slist_append(si_vals, g_variant_new_string("M"));
    si_vals = g_slist_append(si_vals, g_variant_new_string("G"));
    si_vals = g_slist_append(si_vals, g_variant_new_string("T"));
    gray_code_options[3].id = "si_prefix";
    gray_code_options[3].idn = "dec_graycode_opt_si_prefix";
    gray_code_options[3].desc = "SI prefix for interval/rate display";
    gray_code_options[3].def = g_variant_new_string("");
    gray_code_options[3].values = si_vals;

    return &gray_code_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}