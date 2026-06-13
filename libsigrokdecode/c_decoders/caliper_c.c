#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum {
    ANN_MEASUREMENT = 0,
    ANN_WARNING,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;
    int timeout_ms;
    int unit;           /* 0=keep, 1=mm, 2=inch */
    int changes_only;   /* 0=no, 1=yes */
    uint64_t ss;
    uint64_t es;
    uint8_t number_bits[16];
    int number_count;
    uint8_t flags_bits[8];
    int flags_count;
    double last_number;
    int last_is_inch;
    int has_last;
} caliper_state;

static const double mm_per_inch = 25.4;

static struct srd_channel caliper_channels[] = {
    { "clk", "CLK", "Serial clock line", 0, SRD_CHANNEL_SCLK, "dec_caliper_chan_clk" },
    { "data", "DATA", "Serial data line", 1, SRD_CHANNEL_SDATA, "dec_caliper_chan_data" },
};

static struct srd_decoder_option caliper_options[] = {
    { "timeout_ms", NULL, "Packet timeout in ms, 0 to disable", NULL, NULL },
    { "unit", NULL, "Convert units", NULL, NULL },
    { "changes", NULL, "Changes only", NULL, NULL },
};

static const char* caliper_ann_labels[][3] = {
    { "", "measurement", "Measurement" },
    { "", "warning", "Warning" },
};

static const int caliper_row_measurements_classes[] = {ANN_MEASUREMENT};
static const int caliper_row_warnings_classes[] = {ANN_WARNING};

static const struct srd_c_ann_row caliper_ann_rows[] = {
    { "measurements", "Measurements", caliper_row_measurements_classes, 1 },
    { "warnings", "Warnings", caliper_row_warnings_classes, 1 },
};

static const char* caliper_inputs[] = { "logic" };
static const char* caliper_tags[] = { "Analog/digital", "Sensor" };

static void caliper_reset_state(caliper_state *s)
{
    s->ss = 0;
    s->es = 0;
    s->number_count = 0;
    s->flags_count = 0;
}

static void caliper_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(caliper_state)));
    }
    caliper_state *s = (caliper_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(caliper_state));
    s->out_ann = -1;
    s->timeout_ms = 10;
}

static void caliper_start(struct srd_decoder_inst *di)
{
    caliper_state *s = (caliper_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "caliper");

    s->timeout_ms = (int)c_opt_int(di, "timeout_ms", 10);

    const char *unit_str = c_opt_str(di, "unit", "keep");
    if (strcmp(unit_str, "mm") == 0)
        s->unit = 1;
    else if (strcmp(unit_str, "inch") == 0)
        s->unit = 2;
    else
        s->unit = 0;

    const char *changes_str = c_opt_str(di, "changes", "no");
    s->changes_only = (strcmp(changes_str, "yes") == 0) ? 1 : 0;
}

static void caliper_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    caliper_state *s = (caliper_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void caliper_decode(struct srd_decoder_inst *di)
{
    caliper_state *s = (caliper_state *)c_decoder_get_private(di);
    int ret;

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    int has_timeout = (s->timeout_ms > 0);
    uint64_t timeout_snum = 0;
    if (has_timeout) {
        timeout_snum = (uint64_t)s->timeout_ms * s->samplerate / 1000;
    }

    while (1) {
        /* Wait for CLK rising edge, optionally with timeout — single combined wait */
        if (has_timeout)
            ret = c_wait(di, CW_R(0), CW_OR, CW_SKIP(timeout_snum), CW_END);
        else
            ret = c_wait(di, CW_R(0), CW_END);
        if (ret != SRD_OK)
            return;

        /* Timeout check: bit 0 = CLK rise di_matched(di) */
        if (has_timeout && !(di_matched(di) & 0x1)) {
            if (s->number_count > 0 || s->flags_count > 0) {
                int count = s->number_count + s->flags_count;
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "timeout with %d bits in buffer", count);
                char tmp2[32];
                snprintf(tmp2, sizeof(tmp2), "timeout (%d bits)", count);
                c_put(di, s->ss, di_samplenum(di), s->out_ann, ANN_WARNING, tmp, tmp2, "timeout");
            }
            caliper_reset_state(s);
            continue;
        }

        /* Sample DATA pin at CLK rising edge */
        uint8_t data = c_pin(di, 1);

        /* Record position */
        if (!s->ss) s->ss = di_samplenum(di);
        s->es = di_samplenum(di);

        /* Collect bits */
        if (s->number_count < 16) {
            s->number_bits[s->number_count++] = data;
            continue;
        }
        if (s->flags_count < 8) {
            s->flags_bits[s->flags_count++] = data;
            if (s->flags_count < 8) continue;
        }

        /* 24 bits received, process data */
        int negative = s->flags_bits[4] ? 1 : 0;
        int is_inch = s->flags_bits[7] ? 1 : 0;

        /* bitpack: LSB first — match Python's bitpack() = sum([b << i for i, b in enumerate(bits)]) */
        uint32_t number = 0;
        for (int i = 0; i < 16; i++)
            number |= ((uint32_t)s->number_bits[i] << i);

        int32_t signed_number = (int32_t)number;
        if (negative) signed_number = -signed_number;

        double value;
        if (is_inch) {
            value = (double)signed_number / 2000.0;
            if (s->unit == 1) { /* convert to mm */
                value *= mm_per_inch;
                is_inch = 0;
            }
        } else {
            value = (double)signed_number / 100.0;
            if (s->unit == 2) { /* convert to inch */
                value = round(value / mm_per_inch * 10000.0) / 10000.0;
                is_inch = 1;
            }
        }

        const char *unit_str = is_inch ? "in" : "mm";

        /* Change detection */
        int should_output = 1;
        if (s->changes_only && s->has_last) {
            if (value == s->last_number && is_inch == s->last_is_inch)
                should_output = 0;
        }

        if (should_output) {
            char tmp[64];
            /* Match Python: '{number}{unit}'.format(number=value, unit=unit_str)
             * Python's default float formatting removes trailing zeros,
             * which is equivalent to %g with enough precision. */
            snprintf(tmp, sizeof(tmp), "%g%s", value, unit_str);
            char tmp2[32];
            snprintf(tmp2, sizeof(tmp2), "%g", value);
            c_put(di, s->ss, s->es, s->out_ann, ANN_MEASUREMENT, tmp, tmp2);
            s->last_number = value;
            s->last_is_inch = is_inch;
            s->has_last = 1;
        }

        caliper_reset_state(s);
    }
}

static void caliper_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder caliper_c_decoder = {
    .id = "caliper_c",
    .name = "Caliper(C)",
    .longname = "Digital calipers (C)",
    .desc = "Protocol of cheap generic digital calipers.",
    .license = "mit",
    .channels = caliper_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = caliper_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = caliper_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = caliper_ann_rows,
    .inputs = caliper_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = caliper_tags,
    .num_tags = 2,
    .reset = caliper_reset,
    .start = caliper_start,
    .decode = caliper_decode,
    .destroy = caliper_destroy,
    .state_size = 0,
    .metadata = caliper_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &caliper_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}