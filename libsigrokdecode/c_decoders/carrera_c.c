#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_CONTROLLER_0 = 0,
    ANN_CONTROLLER_1,
    ANN_CONTROLLER_2,
    ANN_CONTROLLER_3,
    ANN_CONTROLLER_4,
    ANN_CONTROLLER_5,
    ANN_CONTROLLER_SC,     /* 6 */
    ANN_CONTROLLER_PROG,   /* 7 */
    ANN_CONTROLLER_ACTIVE, /* 8 */
    ANN_BIT,               /* 9 */
    ANN_QUITTIERUNG,       /* 10 */
    ANN_PROG_GAS,          /* 11 */
    ANN_PROG_GENERAL,      /* 12 */
    ANN_PROG_BREMSE,       /* 13 */
    ANN_PROG_TANK,         /* 14 */
    ANN_PROG_WERTE,        /* 15 */
    ANN_PROG_TANKEN,       /* 16 */
    ANN_PROG_POSITION,     /* 17 */
    ANN_PROG_FINISH,       /* 18 */
    ANN_PROG_FINISHLINE,   /* 19 */
    ANN_PROG_FUEL,         /* 20 */
    ANN_PROG_JUMPSTART,    /* 21 */
    ANN_PROG_TRAFFIC_LIGHT,/* 22 */
    ANN_PROG_LAPCOUNT,     /* 23 */
    ANN_PROG_RESET,        /* 24 */
    ANN_PROG_PITLANE,      /* 25 */
    ANN_PROG_PERFORMANCE,  /* 26 */
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;
    int invert;         /* 0=nein, 1=ja */
    int format;         /* 0=hex, 1=dec, 2=oct, 3=bin */
    double currentMicros;
    double previousMicros;
    double intervalMicros;
    uint64_t wordStart;
    uint64_t wordEnd;
    uint64_t bitStart;
    uint32_t dataWord;
    uint64_t beginDataWord;
    uint64_t endDataWord;
    int next_could_be_active_data_word;
} carrera_state;

static struct srd_channel carrera_channels[] = {
    { "data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option carrera_options[] = {
    { "invert", NULL, "Signal ist invertiert", NULL, NULL },
    { "format", NULL, "Data format", NULL, NULL },
};

static const char* carrera_ann_labels[][3] = {
    { "", "controller_0", "Reglerwort ID 0" },
    { "", "controller_1", "Reglerwort ID 1" },
    { "", "controller_2", "Reglerwort ID 2" },
    { "", "controller_3", "Reglerwort ID 3" },
    { "", "controller_4", "Reglerwort ID 4" },
    { "", "controller_5", "Reglerwort ID 5" },
    { "", "controller_sc", "Reglerwort SC/Ghost" },
    { "", "controller_prog", "Programmierwort" },
    { "", "controller_active", "Aktivdatenwort" },
    { "", "bit", "Bit" },
    { "", "quittierung", "Quittierungswort" },
    { "", "prog_gas", "prog_gas" },
    { "", "prog_general", "Programmierdatenwort" },
    { "", "prog_bremse", "prog_bremse" },
    { "", "prog_tank", "prog_tank" },
    { "", "prog_werte", "prog_werte" },
    { "", "prog_tanken", "prog_tanken" },
    { "", "prog_position", "prog_position" },
    { "", "prog_finish", "prog_finish" },
    { "", "prog_finishline", "prog_finishline" },
    { "", "prog_fuel", "prog_fuel" },
    { "", "prog_jumpstart", "prog_jumpstart" },
    { "", "prog_traffic_light", "prog_traffic_light" },
    { "", "prog_lapcount", "prog_lapcount" },
    { "", "prog_reset", "prog_reset" },
    { "", "prog_pitlaneadapter", "prog_pitlaneadapter" },
    { "", "prog_performance", "prog_performance" },
};

static const int carrera_row_bits_classes[] = {ANN_BIT};
static const int carrera_row_controller_classes[] = {ANN_CONTROLLER_0, ANN_CONTROLLER_1, ANN_CONTROLLER_2, ANN_CONTROLLER_3, ANN_CONTROLLER_4, ANN_CONTROLLER_5, ANN_CONTROLLER_SC};
static const int carrera_row_active_quit_classes[] = {ANN_CONTROLLER_ACTIVE, ANN_QUITTIERUNG};
static const int carrera_row_prog_classes[] = {ANN_PROG_GAS, ANN_PROG_GENERAL, ANN_PROG_BREMSE, ANN_PROG_TANK, ANN_PROG_WERTE, ANN_PROG_TANKEN, ANN_PROG_POSITION, ANN_PROG_FINISH, ANN_PROG_FINISHLINE, ANN_PROG_FUEL, ANN_PROG_JUMPSTART, ANN_PROG_TRAFFIC_LIGHT, ANN_PROG_LAPCOUNT, ANN_PROG_RESET, ANN_PROG_PITLANE, ANN_PROG_PERFORMANCE};

static const struct srd_c_ann_row carrera_ann_rows[] = {
    { "word_bit_value", "Bits", carrera_row_bits_classes, 1 },
    { "word_controller", "Reglerwort", carrera_row_controller_classes, 7 },
    { "active_quit", "Aktiv-/Quittierungswort", carrera_row_active_quit_classes, 2 },
    { "prog_word", "Programmierdatenwort", carrera_row_prog_classes, 16 },
};

static const char* carrera_inputs[] = { "logic" };
static const char* carrera_tags[] = { "C Digital" };

static uint32_t carrera_flip_bits(uint32_t value, int bitCount)
{
    bitCount--;
    uint32_t result = 0;
    while (bitCount >= 0 && value) {
        if (value & 1)
            result |= (1 << bitCount);
        value >>= 1;
        bitCount--;
    }
    return result;
}

static uint32_t carrera_get_value(uint32_t dataWord, int bitsToShift, int bitWidth)
{
    uint32_t mask = (1 << bitWidth) - 1;
    return (dataWord >> bitsToShift) & mask;
}

static double carrera_get_usec(uint64_t samplenum, uint64_t samplerate)
{
    return (double)samplenum * 1000000.0 / (double)samplerate;
}

static void carrera_format_data(char *buf, int bufsize, uint32_t value, int bit_width, int format)
{
    switch (format) {
    case 0: /* hex */
    {
        int width = (bit_width + 3) / 4;
        if (width < 2) width = 2;
        snprintf(buf, bufsize, "%0*x", width, value & ((1 << bit_width) - 1));
    }
    break;
    case 1: /* dec */
        snprintf(buf, bufsize, "%u", value);
        break;
    case 2: /* oct */
    {
        int width = (bit_width + 2) / 3;
        if (width < 3) width = 3;
        snprintf(buf, bufsize, "%0*o", width, value & ((1 << bit_width) - 1));
    }
    break;
    case 3: /* bin */
    {
        uint32_t mask = 1 << (bit_width - 1);
        int pos = 0;
        for (int i = 0; i < bit_width && pos < bufsize - 1; i++) {
            buf[pos++] = (value & mask) ? '1' : '0';
            mask >>= 1;
        }
        buf[pos] = '\0';
    }
    break;
    default:
        snprintf(buf, bufsize, "%u", value);
        break;
    }
}

static void carrera_print_reglerdatenwort(struct srd_decoder_inst *di, carrera_state *s)
{
    int regler_id = carrera_get_value(s->dataWord, 6, 3);
    int ta = carrera_get_value(s->dataWord, 0, 1);

    if (regler_id == 2 || regler_id == 7)
        s->next_could_be_active_data_word = 1;

    char desc_long[128];
    char desc_short[16];
    char desc[32];
    int ann_idx;

    if (regler_id == 7) {
        ann_idx = 6; /* ANN_CONTROLLER_SC */
        int pc = carrera_get_value(s->dataWord, 1, 1);
        int nh = carrera_get_value(s->dataWord, 2, 1);
        int fr = carrera_get_value(s->dataWord, 3, 1);
        int tk = carrera_get_value(s->dataWord, 4, 1);
        int kfr = carrera_get_value(s->dataWord, 5, 1);
        snprintf(desc_long, sizeof(desc_long), "KFR:%d TK:%d FR:%d NH:%d PC:%d TA:%d", kfr, tk, fr, nh, pc, ta);
        snprintf(desc_short, sizeof(desc_short), "R SC");
        snprintf(desc, sizeof(desc), "Regler SC");
    } else {
        ann_idx = regler_id; /* 0-5 */
        int gas = (s->dataWord >> 1) & 0x0f;
        int wt = carrera_get_value(s->dataWord, 5, 1);
        snprintf(desc_long, sizeof(desc_long), "ID:%d G: %d WT:%d TA:%d", regler_id, gas, wt, ta);
        snprintf(desc_short, sizeof(desc_short), "R %d", regler_id);
        snprintf(desc, sizeof(desc), "Regler %d", regler_id);
    }

    c_put(di, s->beginDataWord, s->endDataWord, s->out_ann, ann_idx, desc_short, desc, desc_long);
}

static void carrera_print_aktivdatenwort(struct srd_decoder_inst *di, carrera_state *s)
{
    int ie = carrera_get_value(s->dataWord, 0, 1);
    int r5 = carrera_get_value(s->dataWord, 1, 1);
    int r4 = carrera_get_value(s->dataWord, 2, 1);
    int r3 = carrera_get_value(s->dataWord, 3, 1);
    int r2 = carrera_get_value(s->dataWord, 4, 1);
    int r1 = carrera_get_value(s->dataWord, 5, 1);
    int r0 = carrera_get_value(s->dataWord, 6, 1);

    char desc_short[16];
    snprintf(desc_short, sizeof(desc_short), "IE:%d", ie);
    char desc_long[128];
    snprintf(desc_long, sizeof(desc_long), "R0:%d R1:%d R2:%d R3:%d R4:%d R5:%d IE:%d", r0, r1, r2, r3, r4, r5, ie);

    c_put(di, s->beginDataWord, s->endDataWord, s->out_ann, ANN_CONTROLLER_ACTIVE, desc_short, desc_long);
}

static void carrera_print_quittierungswort(struct srd_decoder_inst *di, carrera_state *s)
{
    int s0 = carrera_get_value(s->dataWord, 7, 1);
    int s1 = carrera_get_value(s->dataWord, 6, 1);
    int s2 = carrera_get_value(s->dataWord, 5, 1);
    int s3 = carrera_get_value(s->dataWord, 4, 1);
    int s4 = carrera_get_value(s->dataWord, 3, 1);
    int s5 = carrera_get_value(s->dataWord, 2, 1);
    int s6 = carrera_get_value(s->dataWord, 1, 1);
    int s7 = carrera_get_value(s->dataWord, 0, 1);

    char desc_long[128];
    snprintf(desc_long, sizeof(desc_long), "S0:%d S1:%d S2:%d S3:%d S4:%d S5:%d S6:%d S7:%d", s0, s1, s2, s3, s4, s5, s6, s7);

    c_put(di, s->beginDataWord, s->endDataWord, s->out_ann, ANN_QUITTIERUNG, "Q", "Quitt.", desc_long);
}

static void carrera_print_programmierdatenwort(struct srd_decoder_inst *di, carrera_state *s)
{
    uint32_t wert_raw = carrera_get_value(s->dataWord, 8, 4);
    uint32_t befehl_raw = carrera_get_value(s->dataWord, 3, 5);
    uint32_t regler_raw = carrera_get_value(s->dataWord, 0, 3);

    uint32_t wert = carrera_flip_bits(wert_raw, 4);
    uint32_t befehl = carrera_flip_bits(befehl_raw, 5);
    uint32_t regler = carrera_flip_bits(regler_raw, 3);

    char befehl_str[32], regler_str[32], wert_str[32];
    carrera_format_data(befehl_str, sizeof(befehl_str), befehl, 5, s->format);
    carrera_format_data(regler_str, sizeof(regler_str), regler, 3, s->format);
    carrera_format_data(wert_str, sizeof(wert_str), wert, 4, s->format);

    char desc[128];
    snprintf(desc, sizeof(desc), "Befehl: %s, Regler: %s, Wert: %s", befehl_str, regler_str, wert_str);

    int ann_idx = 11 + (int)befehl;
    if (ann_idx >= NUM_ANN) ann_idx = ANN_PROG_GENERAL;

    c_put(di, s->beginDataWord, s->endDataWord, s->out_ann, ann_idx, desc);
}

static void carrera_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(carrera_state)));
    }
    carrera_state *s = (carrera_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(carrera_state));
    s->out_ann = -1;
    s->dataWord = 1;
}

static void carrera_start(struct srd_decoder_inst *di)
{
    carrera_state *s = (carrera_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "carrera");

    const char *invert_str = c_opt_str(di, "invert", "nein");
    s->invert = (strcmp(invert_str, "ja") == 0) ? 1 : 0;

    const char *fmt_str = c_opt_str(di, "format", "hex");
    if (strcmp(fmt_str, "hex") == 0)
        s->format = 0;
    else if (strcmp(fmt_str, "dec") == 0)
        s->format = 1;
    else if (strcmp(fmt_str, "oct") == 0)
        s->format = 2;
    else if (strcmp(fmt_str, "bin") == 0)
        s->format = 3;
    else
        s->format = 0;

    s->dataWord = 1;
}

static void carrera_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    carrera_state *s = (carrera_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void carrera_decode(struct srd_decoder_inst *di)
{
    carrera_state *s = (carrera_state *)c_decoder_get_private(di);
    int ret;
    int bit_val = s->invert ? 1 : 0;

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    while (1) {
        ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        s->currentMicros = carrera_get_usec(di_samplenum(di), s->samplerate);
        s->intervalMicros = s->currentMicros - s->previousMicros;

        if (s->intervalMicros < 200.0) {
            s->endDataWord = di_samplenum(di);
        }

        if (s->intervalMicros >= 75.0 && s->intervalMicros <= 125.0) {
            /* Valid bit */
            s->previousMicros = s->currentMicros;
            s->dataWord <<= 1;

            uint8_t pin = c_pin(di, 0);
            if (pin == bit_val) {
                s->dataWord |= 1;
                c_put(di, s->bitStart, di_samplenum(di), s->out_ann, ANN_BIT, "1");
            } else {
                c_put(di, s->bitStart, di_samplenum(di), s->out_ann, ANN_BIT, "0");
            }
            s->bitStart = di_samplenum(di);
        } else if (s->intervalMicros > 6000.0) {
            /* Word gap */
            if (s->next_could_be_active_data_word) {
                if (s->dataWord > 127 && s->dataWord < 256) {
                    carrera_print_aktivdatenwort(di, s);
                } else if (s->dataWord < 512) {
                    carrera_print_quittierungswort(di, s);
                }
                s->next_could_be_active_data_word = 0;
            } else if (s->dataWord < 1024) {
                carrera_print_reglerdatenwort(di, s);
            } else {
                carrera_print_programmierdatenwort(di, s);
            }
            s->dataWord = 1;
            s->previousMicros = s->currentMicros;
            s->beginDataWord = di_samplenum(di);
            s->bitStart = di_samplenum(di);
        }
    }
}

static void carrera_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder carrera_c_decoder = {
    .id = "carrera_c",
    .name = "Carrera Digital Decoder(C)",
    .longname = "Carrera Digital (C)",
    .desc = "was macht der wohl?",
    .license = "gplv2+",
    .channels = carrera_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = carrera_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = carrera_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = carrera_ann_rows,
    .inputs = carrera_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = carrera_tags,
    .num_tags = 1,
    .reset = carrera_reset,
    .start = carrera_start,
    .decode = carrera_decode,
    .destroy = carrera_destroy,
    .state_size = 0,
    .metadata = carrera_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &carrera_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}