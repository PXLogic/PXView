#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_RPM = 0,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    uint64_t last_samplenum;
    int edge_num;
    int edge_type;       /* 0=rising, 1=falling */
    int num_pulses;
    int out_ann;
} rpm_state;

static struct srd_channel rpm_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option rpm_options[] = {
    {"num_pulses", NULL, "Number of pulses per revolution", NULL, NULL},
    {"edge", NULL, "Edges to check", NULL, NULL},
};

static const char *rpm_ann_labels[][3] = {
    {"", "rpm", "RPM"},
};

static const int rpm_row_rpms_classes[] = {ANN_RPM, -1};
static const struct srd_c_ann_row rpm_ann_rows[] = {
    {"rpms", "RPM", rpm_row_rpms_classes, 1},
};

static const char *rpm_inputs[] = {"logic"};
static const char *rpm_outputs[] = {NULL};
static const char *rpm_tags[] = {"Util"};

static void rpm_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(rpm_state)));
    rpm_state *s = (rpm_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(rpm_state));
}

static void rpm_start(struct srd_decoder_inst *di)
{
    rpm_state *s = (rpm_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rpm");
    s->samplerate = c_samplerate(di);

    s->num_pulses = (int)c_opt_int(di, "num_pulses", 2);
    if (s->num_pulses < 1)
        s->num_pulses = 1;

    const char *edge_str = c_opt_str(di, "edge", "falling");
    s->edge_type = (strcmp(edge_str, "rising") == 0) ? 0 : 1;
}

static void rpm_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    rpm_state *s = (rpm_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void rpm_decode(struct srd_decoder_inst *di)
{
    rpm_state *s = (rpm_state *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate == 0) return;
    }

    while (1) {
        if (s->edge_type == 0)
            ret = c_wait(di, CW_R(0), CW_END);
        else
            ret = c_wait(di, CW_F(0), CW_END);
        if (ret != SRD_OK)
            return;

        if (s->last_samplenum == 0) {
            s->last_samplenum = di_samplenum(di);
            continue;
        }

        s->edge_num++;
        double t = (double)(di_samplenum(di) - s->last_samplenum) / (double)s->samplerate;

        if (t >= 0.5) {
            s->edge_num = 0;
            s->last_samplenum = di_samplenum(di);
            continue;
        }

        if (s->edge_num == s->num_pulses) {
            s->edge_num = 0;
            int rpm = (int)(60.0 / t);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", rpm);
            c_put_v(di, s->last_samplenum, di_samplenum(di), s->out_ann, ANN_RPM, rpm, buf);
            s->last_samplenum = di_samplenum(di);
        }
    }
}

static void rpm_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder rpm_c_decoder = {
    .id = "rpm_c",
    .name = "RPM(C)",
    .longname = "Revolutions per minute (C)",
    .desc = "Calculate the number of turns in one minute. (C implementation)",
    .license = "gplv2+",
    .channels = rpm_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = rpm_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = rpm_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = rpm_ann_rows,
    .inputs = rpm_inputs,
    .num_inputs = 1,
    .outputs = rpm_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rpm_tags,
    .num_tags = 1,
    .reset = rpm_reset,
    .start = rpm_start,
    .metadata = rpm_metadata,
    .decode = rpm_decode,
    .destroy = rpm_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    rpm_options[0].id = "num_pulses";
    rpm_options[0].idn = "dec_rpm_opt_num_pulses";
    rpm_options[0].desc = "Number of pulses per revolution";
    rpm_options[0].def = g_variant_new_int64(2);

    GSList *edge_vals = NULL;
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("rising"));
    edge_vals = g_slist_append(edge_vals, g_variant_new_string("falling"));
    rpm_options[1].id = "edge";
    rpm_options[1].idn = "dec_rpm_opt_edge";
    rpm_options[1].desc = "Edges to check";
    rpm_options[1].def = g_variant_new_string("falling");
    rpm_options[1].values = edge_vals;

    return &rpm_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}