#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_BIT_RAW = 0,
    ANN_BIT_ERROR,
    ANN_BIT_PHASE,
    NUM_ANN,
};

enum cycle_type {
    CYCLE_IDLE = 0,
    CYCLE_SPACE,
    CYCLE_MARK,
    CYCLE_ERROR,
    CYCLE_PROCESSED,
};

C_DECODER_STATE(afsk, {
    uint64_t samplerate;
    int out_ann;
    int out_proto;
    int markfreq;
    int spacefreq;
    int marginpct;
    int64_t markhalfcycle;
    int64_t markmargin;
    int64_t spacehalfcycle;
    int64_t spacemargin;
    int lastbit;
    int cycletype;
    int lastcycletype;
    uint64_t twoedgesagosample;
    uint64_t oneedgeagosample;
    uint64_t currentedgesample;
})

static struct srd_channel afsk_channels[] = {
    { "afsk", "afsk", "AFSK stream", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option afsk_options[] = {
    { "markfreq", NULL, "Mark(1) Frequency", NULL, NULL },
    { "spacefreq", NULL, "Space(0) Frequency", NULL, NULL },
    { "marginpct", NULL, "Error margin %", NULL, NULL },
};

static const char* afsk_ann_labels[][3] = {
    { "", "bit-raw", "Raw Bit" },
    { "", "bit-error", "Unknown half-cycle" },
    { "", "bit-phase", "Phase error" },
};

static const int afsk_row_raw_classes[] = {ANN_BIT_RAW};
static const int afsk_row_errors_classes[] = {ANN_BIT_ERROR, ANN_BIT_PHASE};

static const struct srd_c_ann_row afsk_ann_rows[] = {
    { "raw-bits", "Raw Bits", afsk_row_raw_classes, 1 },
    { "errors", "Errors", afsk_row_errors_classes, 2 },
};

static const char* afsk_inputs[] = { "logic" };
static const char* afsk_outputs[] = { "afsk_bits" };
static const char* afsk_tags[] = { "Embedded/industrial" };

static void afsk_start(struct srd_decoder_inst *di)
{
    afsk_s *s = (afsk_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "afsk");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "afsk_bits");

    s->markfreq = (int)c_opt_int(di, "markfreq", 2000);
    s->spacefreq = (int)c_opt_int(di, "spacefreq", 4000);
    s->marginpct = (int)c_opt_int(di, "marginpct", 40);

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);

    if (s->samplerate) {
        s->markhalfcycle = (int64_t)(s->samplerate * (1.0 / s->markfreq) / 2.0) - 1;
        s->markmargin = (int64_t)(s->markhalfcycle * (s->marginpct / 100.0));
        s->spacehalfcycle = (int64_t)(s->samplerate * (1.0 / s->spacefreq) / 2.0) - 1;
        s->spacemargin = (int64_t)(s->spacehalfcycle * (s->marginpct / 100.0));
    }
}

static void afsk_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    afsk_s *s = (afsk_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        s->markhalfcycle = (int64_t)(s->samplerate * (1.0 / s->markfreq) / 2.0) - 1;
        s->markmargin = (int64_t)(s->markhalfcycle * (s->marginpct / 100.0));
        s->spacehalfcycle = (int64_t)(s->samplerate * (1.0 / s->spacefreq) / 2.0) - 1;
        s->spacemargin = (int64_t)(s->spacehalfcycle * (s->marginpct / 100.0));
    }
}

static void afsk_decode(struct srd_decoder_inst *di)
{
    afsk_s *s = (afsk_s *)c_decoder_get_private(di);

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    /* Recalculate in case samplerate was not available at start */
    if (s->markhalfcycle == 0 && s->samplerate) {
        s->markhalfcycle = (int64_t)(s->samplerate * (1.0 / s->markfreq) / 2.0) - 1;
        s->markmargin = (int64_t)(s->markhalfcycle * (s->marginpct / 100.0));
        s->spacehalfcycle = (int64_t)(s->samplerate * (1.0 / s->spacefreq) / 2.0) - 1;
        s->spacemargin = (int64_t)(s->spacehalfcycle * (s->marginpct / 100.0));
    }

    while (1) {
        s->twoedgesagosample = s->oneedgeagosample;
        s->oneedgeagosample = s->currentedgesample;
        s->lastcycletype = s->cycletype;

        /* Wait for any edge */
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK) return;

        s->currentedgesample = di_samplenum(di);
        int64_t length = (int64_t)(s->currentedgesample - s->oneedgeagosample);

        /* Determine half-cycle type */
        if (length >= (s->spacehalfcycle - s->spacemargin) &&
            length <= (s->spacehalfcycle + s->spacemargin)) {
            s->cycletype = CYCLE_SPACE;
        } else if (length >= (s->markhalfcycle - s->markmargin) &&
                   length <= (s->markhalfcycle + s->markmargin)) {
            s->cycletype = CYCLE_MARK;
        } else {
            s->cycletype = CYCLE_ERROR;
        }

        /* State transitions */
        if (s->cycletype == CYCLE_SPACE && s->lastcycletype == CYCLE_SPACE) {
            s->lastbit = 0;
            c_put(di, s->twoedgesagosample, s->currentedgesample, s->out_ann, ANN_BIT_RAW,
                  s->lastbit ? "1" : "0");
            c_proto(di, s->twoedgesagosample, s->currentedgesample, s->out_proto,
                    "BIT", C_U8(s->lastbit), C_END);
            s->cycletype = CYCLE_PROCESSED;
        } else if (s->cycletype == CYCLE_MARK && s->lastcycletype == CYCLE_MARK) {
            s->lastbit = 1;
            c_put(di, s->twoedgesagosample, s->currentedgesample, s->out_ann, ANN_BIT_RAW,
                  s->lastbit ? "1" : "0");
            c_proto(di, s->twoedgesagosample, s->currentedgesample, s->out_proto,
                    "BIT", C_U8(s->lastbit), C_END);
            s->cycletype = CYCLE_PROCESSED;
        } else if (s->cycletype == CYCLE_ERROR) {
            s->lastbit = 2;
            c_put(di, s->oneedgeagosample, s->currentedgesample, s->out_ann, ANN_BIT_ERROR,
                  "Error: Invalid cycle", "Error", "Err", "E");
            c_proto(di, s->oneedgeagosample, s->currentedgesample, s->out_proto,
                    "ERROR", C_STR("INVALID"), C_END);
        } else if ((s->cycletype == CYCLE_SPACE && s->lastcycletype == CYCLE_MARK) ||
                   (s->cycletype == CYCLE_MARK && s->lastcycletype == CYCLE_SPACE)) {
            s->lastbit = 2;
            c_put(di, s->oneedgeagosample, s->currentedgesample, s->out_ann, ANN_BIT_PHASE,
                  "Phase error: Resyncing", "Phase error", "Phase", "P");
            c_proto(di, s->oneedgeagosample, s->currentedgesample, s->out_proto,
                    "ERROR", C_STR("PHASE"), C_END);
        }
    }
}

struct srd_c_decoder afsk_c_def = {
    .id = "afsk_c",
    .name = "AFSK(C)",
    .longname = "Audio Frequency Shift Keying (C)",
    .desc = "Audio Frequency Shift Keying",
    .license = "gplv2+",
    .channels = afsk_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = afsk_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = afsk_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = afsk_ann_rows,
    .inputs = afsk_inputs,
    .num_inputs = 1,
    .outputs = afsk_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = afsk_tags,
    .num_tags = 1,
    .state_size = sizeof(afsk_s),
    .reset = afsk_reset,
    .start = afsk_start,
    .decode = afsk_decode,
    .destroy = afsk_destroy,
    .metadata = afsk_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    afsk_options[0].def = g_variant_new_int32(2000);
    afsk_options[1].def = g_variant_new_int32(4000);
    afsk_options[2].def = g_variant_new_int32(40);
    return &afsk_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}