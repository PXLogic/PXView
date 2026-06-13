#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

#define TIMING_MAX_AVG 10000

enum timing_ann {
    ANN_TIME = 0,
    ANN_TERSE,
    ANN_AVG,
    ANN_DELTA,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;

    int avg_period;
    int edge;       /* 0=any, 1=rising, 2=falling */
    int delta;      /* 0=no, 1=yes */
    int format;     /* 0=full, 1=terse-auto, 2=terse-s, 3=terse-ms, 4=terse-us, 5=terse-ns, 6=terse-ps, 7=samples */

    double avg_buffer[TIMING_MAX_AVG];
    int avg_count;
    int avg_head;
    double avg_sum;

    double last_t;
    uint64_t ss;
    int have_ss;

    int out_ann;
} timing_state;

static struct srd_channel timing_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option timing_options[] = {
    {"avg_period", NULL, "Averaging period", NULL, NULL},
    {"edge", NULL, "Edges to check", NULL, NULL},
    {"delta", NULL, "Show delta from last", NULL, NULL},
    {"format", NULL, "Format of 'time' annotation", NULL, NULL},
};

static const char *timing_ann_labels[][3] = {
    {"", "time", "Time"},
    {"", "terse", "Terse"},
    {"", "average", "Average"},
    {"", "delta", "Delta"},
};

static const int row_times_classes[] = {ANN_TIME, ANN_TERSE, -1};
static const int row_averages_classes[] = {ANN_AVG, -1};
static const int row_deltas_classes[] = {ANN_DELTA, -1};

static const struct srd_c_ann_row timing_ann_rows[] = {
    {"times", "Times", row_times_classes, 2},
    {"averages", "Averages", row_averages_classes, 1},
    {"deltas", "Deltas", row_deltas_classes, 1},
};

static const char *timing_inputs[] = {"logic"};
static const char *timing_tags[] = {"Clock/timing", "Util"};

static void timing_normalize_time(double t, char *buf, int bufsize)
{
    if (fabs(t) >= 1.0) {
        snprintf(buf, bufsize, "%.3f s  (%.3f Hz)", t, 1.0 / t);
    } else if (fabs(t) >= 1e-3) {
        if (1.0 / t / 1000.0 < 1.0)
            snprintf(buf, bufsize, "%.3f ms (%.3f Hz)", t * 1000.0, 1.0 / t);
        else
            snprintf(buf, bufsize, "%.3f ms (%.3f kHz)", t * 1000.0, 1.0 / t / 1000.0);
    } else if (fabs(t) >= 1e-6) {
        if (1.0 / t / 1000.0 / 1000.0 < 1.0)
            snprintf(buf, bufsize, "%.3f \xCE\xBCs (%.3f kHz)", t * 1000.0 * 1000.0, 1.0 / t / 1000.0);
        else
            snprintf(buf, bufsize, "%.3f \xCE\xBCs (%.3f MHz)", t * 1000.0 * 1000.0, 1.0 / t / 1000.0 / 1000.0);
    } else if (fabs(t) >= 1e-9) {
        /* Python uses "if 1/t/1000/1000/1000:" which is a truthiness check (always True),
           so the GHz branch is dead code. Always show MHz for ns range. */
        snprintf(buf, bufsize, "%.3f ns (%.3f MHz)", t * 1000.0 * 1000.0 * 1000.0, 1.0 / t / 1000.0 / 1000.0);
    } else {
        snprintf(buf, bufsize, "%f", t);
    }
}

static void timing_terse_time(double t, int fmt, char *out1, int out1_size,
                               char *out2, int out2_size)
{
    double scale = 0;
    const char *unit = "";
    switch (fmt) {
    case 1: /* terse-auto */
        if (fabs(t) >= 1e0) { scale = 1e0; unit = "s"; }
        else if (fabs(t) >= 1e-3) { scale = 1e3; unit = "ms"; }
        else if (fabs(t) >= 1e-6) { scale = 1e6; unit = "\xCE\xBCs"; }
        else if (fabs(t) >= 1e-9) { scale = 1e9; unit = "ns"; }
        else if (fabs(t) >= 1e-12) { scale = 1e12; unit = "ps"; }
        break;
    case 2: scale = 1e0; unit = "s"; break;
    case 3: scale = 1e3; unit = "ms"; break;
    case 4: scale = 1e6; unit = "\xCE\xBCs"; break;
    case 5: scale = 1e9; unit = "ns"; break;
    case 6: scale = 1e12; unit = "ps"; break;
    default:
        out1[0] = '\0';
        out2[0] = '\0';
        return;
    }

    if (scale > 0) {
        t *= scale;
        snprintf(out1, out1_size, "%.0f%s", t, unit);
        snprintf(out2, out2_size, "%.0f", t);
    }
}

static void timing_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(timing_state)));
    timing_state *s = (timing_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(timing_state));
    s->avg_period = 100;
    s->out_ann = -1;
}

static void timing_start(struct srd_decoder_inst *di)
{
    timing_state *s = (timing_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "timing");

    s->avg_period = (int)c_opt_int(di, "avg_period", 100);
    if (s->avg_period < 0)
        s->avg_period = 0;
    if (s->avg_period > TIMING_MAX_AVG)
        s->avg_period = TIMING_MAX_AVG;

    const char *edge_str = c_opt_str(di, "edge", "any");
    if (strcmp(edge_str, "rising") == 0)
        s->edge = 1;
    else if (strcmp(edge_str, "falling") == 0)
        s->edge = 2;
    else
        s->edge = 0;

    const char *delta_str = c_opt_str(di, "delta", "no");
    s->delta = (strcmp(delta_str, "yes") == 0) ? 1 : 0;

    const char *format_str = c_opt_str(di, "format", "full");
    if (strcmp(format_str, "full") == 0)
        s->format = 0;
    else if (strcmp(format_str, "terse-auto") == 0)
        s->format = 1;
    else if (strcmp(format_str, "terse-s") == 0)
        s->format = 2;
    else if (strcmp(format_str, "terse-ms") == 0)
        s->format = 3;
    else if (strcmp(format_str, "terse-us") == 0)
        s->format = 4;
    else if (strcmp(format_str, "terse-ns") == 0)
        s->format = 5;
    else if (strcmp(format_str, "terse-ps") == 0)
        s->format = 6;
    else if (strcmp(format_str, "samples") == 0)
        s->format = 7;
    else
        s->format = 0;
}

static void timing_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    timing_state *s = (timing_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void timing_decode(struct srd_decoder_inst *di)
{
    timing_state *s = (timing_state *)c_decoder_get_private(di);
    if (!s->samplerate)
        return;

    while (1) {
        int ret;
        switch (s->edge) {
        case 1:  ret = c_wait(di, CW_R(0), CW_END); break;
        case 2:  ret = c_wait(di, CW_F(0), CW_END); break;
        default: ret = c_wait(di, CW_E(0), CW_END); break;
        }
        if (ret != SRD_OK)
            return;

        if (!s->have_ss) {
            s->ss = di_samplenum(di);
            s->have_ss = 1;
            continue;
        }

        uint64_t es = di_samplenum(di);
        uint64_t sa = es - s->ss;
        double t = (double)sa / (double)s->samplerate;

        /* Full format */
        if (s->format == 0) {
            char full_buf[128];
            timing_normalize_time(t, full_buf, sizeof(full_buf));
            c_put(di, s->ss, es, s->out_ann, ANN_TIME, full_buf);
        } else if (s->format == 7) {
            /* Samples format */
            char samp_buf[32];
            snprintf(samp_buf, sizeof(samp_buf), "%llu samples", (unsigned long long)sa);
            c_put(di, s->ss, es, s->out_ann, ANN_TIME, samp_buf);
        } else {
            /* Terse formats */
            char terse1[64], terse2[64];
            timing_terse_time(t, s->format, terse1, sizeof(terse1), terse2, sizeof(terse2));
            c_put(di, s->ss, es, s->out_ann, ANN_TIME, terse1, terse2);
        }

        /* Terse annotation (only for non-full formats) */
        if (s->format != 0) {
            char terse1[64], terse2[64];
            timing_terse_time(t, s->format, terse1, sizeof(terse1), terse2, sizeof(terse2));
            c_put(di, s->ss, es, s->out_ann, ANN_TERSE, terse1, terse2);
        }

        /* Sliding window average */
        if (s->avg_period > 0 && t > 0) {
            s->avg_sum += t;
            s->avg_buffer[s->avg_head] = t;
            s->avg_head = (s->avg_head + 1) % s->avg_period;
            if (s->avg_count < s->avg_period)
                s->avg_count++;
            else
                s->avg_sum -= s->avg_buffer[s->avg_head];

            double average = s->avg_sum / s->avg_count;
            char avg_buf[128];
            timing_normalize_time(average, avg_buf, sizeof(avg_buf));
            c_put(di, s->ss, es, s->out_ann, ANN_AVG, avg_buf);
        }

        /* Delta from last */
        if (s->delta && s->last_t > 0) {
            double dt = t - s->last_t;
            char delta_buf[128];
            if (fabs(dt) >= 1e-3)
                snprintf(delta_buf, sizeof(delta_buf), "%+.3f ms", dt * 1e3);
            else if (fabs(dt) >= 1e-6)
                snprintf(delta_buf, sizeof(delta_buf), "%+.3f \xCE\xBCs", dt * 1e6);
            else
                snprintf(delta_buf, sizeof(delta_buf), "%+.3f ns", dt * 1e9);
            c_put(di, s->ss, es, s->out_ann, ANN_DELTA, delta_buf);
        }

        s->last_t = t;
        s->ss = di_samplenum(di);
    }
}

static void timing_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder timing_c_decoder = {
    .id = "timing_c",
    .name = "Timing(C)",
    .longname = "Timing calculation with frequency and averaging (C)",
    .desc = "Calculate time between edges (C implementation)",
    .license = "gplv2+",
    .channels = timing_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = timing_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = timing_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = timing_ann_rows,
    .inputs = timing_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = timing_tags,
    .num_tags = 2,
    .reset = timing_reset,
    .start = timing_start,
    .decode = timing_decode,
    .destroy = timing_destroy,
    .state_size = 0,
    .metadata = timing_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    timing_options[0].idn = "dec_timing_opt_avg_period";
    timing_options[0].def = g_variant_new_uint64(100);

    timing_options[1].idn = "dec_timing_opt_edge";
    timing_options[1].def = g_variant_new_string("any");
    GSList *edge_vals = NULL;
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("any"));
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("rising"));
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("falling"));
    timing_options[1].values = edge_vals;

    timing_options[2].idn = "dec_timing_opt_delta";
    timing_options[2].def = g_variant_new_string("no");
    GSList *delta_vals = NULL;
    delta_vals = g_slist_append(delta_vals, g_variant_new_string("yes"));
    delta_vals = g_slist_append(delta_vals, g_variant_new_string("no"));
    timing_options[2].values = delta_vals;

    timing_options[3].idn = "dec_timing_opt_format";
    timing_options[3].def = g_variant_new_string("full");
    GSList *format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("full"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("terse-auto"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("terse-s"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("terse-ms"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("terse-us"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("terse-ns"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("terse-ps"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("samples"));
    timing_options[3].values = format_vals;

    return &timing_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}