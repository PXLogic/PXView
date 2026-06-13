/*
 * stepper_motor_c.c — Stepper motor position / speed decoder (C implementation)
 *
 * Decodes absolute position and movement speed from step/dir signals.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum stepper_ann {
    ANN_SPEED = 0,
    ANN_POSITION,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;

    uint64_t ss_prev_step;
    int64_t pos;
    double scale;
    int is_mm;             /* 0=steps, 1=mm */

    /* Options */
    double steps_per_mm;
} stepper_state;

static struct srd_channel stepper_channels[] = {
    { "step", "Step", "Step pulse", 0, SRD_CHANNEL_SCLK, "dec_stepper_motor_chan_step" },
    { "dir",  "Direction", "Direction select", 1, SRD_CHANNEL_SDATA, "dec_stepper_motor_chan_dir" },
};

static struct srd_decoder_option stepper_options[] = {
    { "unit", NULL, "Unit", NULL, NULL },
    { "steps_per_mm", NULL, "Steps per mm", NULL, NULL },
};

static const char *stepper_ann_labels[][3] = {
    { "", "speed", "Speed" },
    { "", "position", "Position" },
};

static const int stepper_row_speed_classes[] = { ANN_SPEED, -1 };
static const int stepper_row_position_classes[] = { ANN_POSITION, -1 };

static const struct srd_c_ann_row stepper_ann_rows[] = {
    { "speed", "Speed", stepper_row_speed_classes, 1 },
    { "position", "Position", stepper_row_position_classes, 1 },
};

static const char *stepper_inputs[] = { "logic" };
static const char *stepper_tags[] = { "Embedded/industrial" };

static void stepper_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(stepper_state)));
    stepper_state *s = (stepper_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(stepper_state));
    s->out_ann = -1;
    s->steps_per_mm = 100.0;
    s->scale = 1.0;
    s->is_mm = 0;
}

static void stepper_start(struct srd_decoder_inst *di)
{
    stepper_state *s = (stepper_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "stepper_motor");

    const char *unit_str = c_opt_str(di, "unit", "steps");
    if (strcmp(unit_str, "mm") == 0) {
        s->is_mm = 1;
        s->steps_per_mm = c_opt_dbl(di, "steps_per_mm", 100.0);
        if (s->steps_per_mm <= 0)
            s->steps_per_mm = 100.0;
        s->scale = s->steps_per_mm;
    } else {
        s->is_mm = 0;
        s->scale = 1.0;
    }
}

static void stepper_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    stepper_state *s = (stepper_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void stepper_decode(struct srd_decoder_inst *di)
{
    stepper_state *s = (stepper_state *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
    }
    if (s->samplerate == 0)
        return;

    while (1) {
        int ret = c_wait(di, CW_R(0), CW_END);
        if (ret != SRD_OK)
            return;

        int direction = c_pin(di, 1);

        if (s->ss_prev_step != 0) {
            uint64_t delta = di_samplenum(di) - s->ss_prev_step;

            /* Speed */
            double speed = (double)s->samplerate / (double)delta / s->scale;
            char speed_str[64], speed_short[32];
            if (s->is_mm) {
                snprintf(speed_str, sizeof(speed_str), "%.2f mm/s", speed);
                snprintf(speed_short, sizeof(speed_short), "%.2f", speed);
            } else {
                snprintf(speed_str, sizeof(speed_str), "%.0f steps/s", speed);
                snprintf(speed_short, sizeof(speed_short), "%.0f", speed);
            }
            c_put(di, s->ss_prev_step, di_samplenum(di), s->out_ann, ANN_SPEED,
                      speed_str, speed_short);

            /* Position */
            char pos_str[64], pos_short[32];
            if (s->is_mm) {
                snprintf(pos_str, sizeof(pos_str), "%.2f mm", (double)s->pos / s->scale);
                snprintf(pos_short, sizeof(pos_short), "%.2f", (double)s->pos / s->scale);
            } else {
                snprintf(pos_str, sizeof(pos_str), "%lld steps", (long long)s->pos);
                snprintf(pos_short, sizeof(pos_short), "%lld", (long long)s->pos);
            }
            c_put(di, s->ss_prev_step, di_samplenum(di), s->out_ann, ANN_POSITION,
                      pos_str, pos_short);
        }

        s->pos += direction ? 1 : -1;
        s->ss_prev_step = di_samplenum(di);
    }
}

static void stepper_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder stepper_motor_c_decoder = {
    .id = "stepper_motor_c",
    .name = "Stepper motor(C)",
    .longname = "Stepper motor position / speed (C)",
    .desc = "Absolute position and movement speed from step/dir (C implementation)",
    .license = "gplv2+",
    .channels = stepper_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = stepper_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = stepper_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = stepper_ann_rows,
    .inputs = stepper_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = stepper_tags,
    .num_tags = 1,
    .reset = stepper_reset,
    .start = stepper_start,
    .decode = stepper_decode,
    .destroy = stepper_destroy,
    .state_size = 0,
    .metadata = stepper_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals_unit[] = {
        g_variant_new_string("steps"),
        g_variant_new_string("mm"),
    };
    GSList *list_unit = NULL;
    list_unit = g_slist_append(list_unit, vals_unit[0]);
    list_unit = g_slist_append(list_unit, vals_unit[1]);

    stepper_options[0].id = "unit";
    stepper_options[0].idn = "dec_stepper_motor_opt_unit";
    stepper_options[0].desc = "Unit";
    stepper_options[0].def = g_variant_new_string("steps");
    stepper_options[0].values = list_unit;

    stepper_options[1].id = "steps_per_mm";
    stepper_options[1].idn = "dec_stepper_motor_opt_steps_per_mm";
    stepper_options[1].desc = "Steps per mm";
    stepper_options[1].def = g_variant_new_double(100.0);
    stepper_options[1].values = NULL;

    return &stepper_motor_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}