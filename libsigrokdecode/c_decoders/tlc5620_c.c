#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TLC5620_MAX_BITS 16

enum tlc5620_ann {
    ANN_DAC_SELECT = 0,
    ANN_GAIN,
    ANN_VALUE,
    ANN_DATA_LATCH,
    ANN_LDAC_FALL,
    ANN_BIT,
    ANN_REG_WRITE,
    ANN_VOLTAGE_UPDATE,
    ANN_VOLTAGE_UPDATE_ALL,
    ANN_INVALID_CMD,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;

    /* Bit acquisition */
    int bits_count;
    int bits_value[TLC5620_MAX_BITS];
    uint64_t bits_ss[TLC5620_MAX_BITS];
    uint64_t bits_es[TLC5620_MAX_BITS];

    /* 11-bit frame parse results */
    uint64_t ss_dac_first;
    uint64_t ss_dac, es_dac;
    uint64_t ss_gain, es_gain;
    uint64_t ss_value, es_value;
    uint64_t clock_width;
    int dac_select;     /* 0=A, 1=B, 2=C, 3=D */
    int gain;           /* 1 or 2 */
    int dac_value;      /* 0-255 */
    int ldac;           /* LDAC current level */

    /* DAC state */
    int dacval[4];      /* A/B/C/D values, -1 = unknown */
    int gains[4];       /* A/B/C/D gains */

    /* Options */
    double vref[4];     /* A/B/C/D reference voltage */

    /* Channel presence flags */
    int have_load;
    int have_ldac;

    /* Condition indices for di_matched(di) bitmask */
    int cond_idx_load;
    int cond_idx_ldac;
} tlc5620_state;

static struct srd_channel tlc5620_channels[] = {
    {"clk", "CLK", "Serial interface clock", 0, SRD_CHANNEL_SCLK, NULL},
    {"data", "DATA", "Serial interface data", 1, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_channel tlc5620_optional_channels[] = {
    {"load", "LOAD", "Serial interface load control", 2, SRD_CHANNEL_COMMON, NULL},
    {"ldac", "LDAC", "Load DAC", 3, SRD_CHANNEL_COMMON, NULL},
};

static struct srd_decoder_option tlc5620_options[] = {
    {"vref_a", NULL, "Reference voltage DACA (V)", NULL, NULL},
    {"vref_b", NULL, "Reference voltage DACB (V)", NULL, NULL},
    {"vref_c", NULL, "Reference voltage DACC (V)", NULL, NULL},
    {"vref_d", NULL, "Reference voltage DACD (V)", NULL, NULL},
};

static const char *tlc5620_ann_labels[][3] = {
    {"", "DAC select", "DAC select"},
    {"", "Gain", "Gain"},
    {"", "DAC value", "DAC value"},
    {"", "Data latch", "Data latch point"},
    {"", "LDAC fall", "LDAC falling edge"},
    {"", "Bit", "Bit"},
    {"", "Register write", "Register write"},
    {"", "Voltage update", "Voltage update"},
    {"", "Voltage update all", "Voltage update (all DACs)"},
    {"", "Invalid command", "Invalid command"},
};

static const int tlc5620_row_bits_classes[] = {ANN_BIT, -1};
static const int tlc5620_row_fields_classes[] = {ANN_DAC_SELECT, ANN_GAIN, ANN_VALUE, -1};
static const int tlc5620_row_registers_classes[] = {ANN_REG_WRITE, ANN_VOLTAGE_UPDATE, -1};
static const int tlc5620_row_voltage_updates_classes[] = {ANN_VOLTAGE_UPDATE_ALL, -1};
static const int tlc5620_row_events_classes[] = {ANN_DATA_LATCH, ANN_LDAC_FALL, -1};
static const int tlc5620_row_errors_classes[] = {ANN_INVALID_CMD, -1};

static const struct srd_c_ann_row tlc5620_ann_rows[] = {
    {"bits", "Bits", tlc5620_row_bits_classes, 1},
    {"fields", "Fields", tlc5620_row_fields_classes, 3},
    {"registers", "Registers", tlc5620_row_registers_classes, 2},
    {"voltage-updates", "Voltage updates", tlc5620_row_voltage_updates_classes, 1},
    {"events", "Events", tlc5620_row_events_classes, 2},
    {"errors", "Errors", tlc5620_row_errors_classes, 1},
};

static const char *tlc5620_inputs[] = {"logic"};
static const char *tlc5620_tags[] = {"IC", "Analog/digital"};

static void tlc5620_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(tlc5620_state)));
    tlc5620_state *s = (tlc5620_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tlc5620_state));
    s->out_ann = -1;
    s->ss_dac_first = (uint64_t)-1;
    for (int i = 0; i < 4; i++) {
        s->dacval[i] = -1;
        s->gains[i] = 1;
    }
    s->vref[0] = 3.3;
    s->vref[1] = 3.3;
    s->vref[2] = 3.3;
    s->vref[3] = 3.3;
}

static void tlc5620_start(struct srd_decoder_inst *di)
{
    tlc5620_state *s = (tlc5620_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tlc5620");

    s->have_load = c_has_ch(di, 2);
    s->have_ldac = c_has_ch(di, 3);

    s->vref[0] = c_opt_dbl(di, "vref_a", 3.3);
    s->vref[1] = c_opt_dbl(di, "vref_b", 3.3);
    s->vref[2] = c_opt_dbl(di, "vref_c", 3.3);
    s->vref[3] = c_opt_dbl(di, "vref_d", 3.3);

    /* Get initial LDAC level */
    if (s->have_ldac) {
        uint8_t init_ldac = c_init_pin(di, 3);
        if (init_ldac != 0xFF)
            s->ldac = init_ldac;
        else
            s->ldac = 1;
    }

    /* Pre-compute condition indices for di_matched(di) bitmask */
    int idx = 1; /* condition 0 is always CLK falling */
    s->cond_idx_load = -1;
    s->cond_idx_ldac = -1;
    if (s->have_load) {
        s->cond_idx_load = idx;
        idx++;
    }
    if (s->have_ldac) {
        s->cond_idx_ldac = idx;
        idx++;
    }
}

static void tlc5620_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    tlc5620_state *s = (tlc5620_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static int tlc5620_handle_11bits(struct srd_decoder_inst *di, tlc5620_state *s)
{
    /* Truncate to last 11 bits */
    if (s->bits_count > 11) {
        int skip = s->bits_count - 11;
        for (int i = 0; i < 11; i++) {
            s->bits_value[i] = s->bits_value[i + skip];
            s->bits_ss[i] = s->bits_ss[i + skip];
            s->bits_es[i] = s->bits_es[i + skip];
        }
        s->bits_count = 11;
    }

    /* Not enough bits */
    if (s->bits_count < 11) {
        uint64_t ss = s->bits_ss[0];
        uint64_t es = s->bits_es[s->bits_count - 1];
        if (s->bits_count >= 2) {
            uint64_t cw = s->bits_es[1] - s->bits_ss[1];
            es = s->bits_es[s->bits_count - 1] + cw;
        }
        c_put(di, ss, es, s->out_ann, ANN_INVALID_CMD, "Command too short");
        s->bits_count = 0;
        return 0;
    }

    /* Parse DAC select (bit 0-1) */
    s->ss_dac = s->bits_ss[0];
    s->es_dac = s->bits_ss[2];
    s->ss_gain = s->bits_ss[2];
    s->es_gain = s->bits_ss[3];
    s->ss_value = s->bits_ss[3];
    s->clock_width = s->es_gain - s->ss_gain;
    s->es_value = s->bits_ss[10] + s->clock_width;

    if (s->ss_dac_first == (uint64_t)-1)
        s->ss_dac_first = s->ss_dac;

    int dac_idx = (s->bits_value[0] << 1) | s->bits_value[1];
    const char *dac_names[] = {"DACA", "DACB", "DACC", "DACD"};
    s->dac_select = dac_idx;

    char dac_str[32];
    snprintf(dac_str, sizeof(dac_str), "DAC select: %s", dac_names[dac_idx]);
    c_put(di, s->ss_dac, s->es_dac, s->out_ann, ANN_DAC_SELECT,
              dac_str, dac_names[dac_idx]);

    /* Parse gain (bit 2) */
    s->gain = 1 + s->bits_value[2];
    char gain_str[32];
    snprintf(gain_str, sizeof(gain_str), "Gain: x%d", s->gain);
    char gain_short[8];
    snprintf(gain_short, sizeof(gain_short), "x%d", s->gain);
    c_put(di, s->ss_gain, s->es_gain, s->out_ann, ANN_GAIN,
              gain_str, gain_short);

    /* Parse DAC value (bit 3-10, MSB first) */
    int val = 0;
    for (int i = 3; i < 11; i++)
        val = (val << 1) | s->bits_value[i];
    s->dac_value = val;

    char val_str[32];
    snprintf(val_str, sizeof(val_str), "DAC value: %d", val);
    char val_short[16];
    snprintf(val_short, sizeof(val_short), "%d", val);
    c_put(di, s->ss_value, s->es_value, s->out_ann, ANN_VALUE,
              val_str, val_short);

    /* Output per-bit annotations */
    for (int i = 1; i < 11; i++) {
        char bstr[4];
        snprintf(bstr, sizeof(bstr), "%d", s->bits_value[i - 1]);
        c_put(di, s->bits_ss[i - 1], s->bits_ss[i], s->out_ann, ANN_BIT, bstr);
    }
    char bstr_last[4];
    snprintf(bstr_last, sizeof(bstr_last), "%d", s->bits_value[10]);
    c_put(di, s->bits_ss[10], s->bits_ss[10] + s->clock_width, s->out_ann, ANN_BIT, bstr_last);

    s->bits_count = 0;
    return 1;
}

static void tlc5620_handle_load_fall(struct srd_decoder_inst *di, tlc5620_state *s, uint64_t samplenum)
{
    (void)samplenum;
    if (!tlc5620_handle_11bits(di, s))
        return;

    const char *dac_names[] = {"DACA", "DACB", "DACC", "DACD"};
    c_put(di, samplenum, samplenum, s->out_ann, ANN_DATA_LATCH,
              "Falling edge on LOAD", "LOAD fall", "F");

    double vref = s->vref[s->dac_select];
    double voltage = vref * ((double)s->dac_value / 256.0) * s->gain;
    char v_str[32];
    snprintf(v_str, sizeof(v_str), "%.2fV", voltage);

    if (s->ldac == 0) {
        char ann_str[64];
        snprintf(ann_str, sizeof(ann_str), "Setting %s voltage to %s", dac_names[s->dac_select], v_str);
        char short_str[48];
        snprintf(short_str, sizeof(short_str), "%s=%s", dac_names[s->dac_select], v_str);
        c_put(di, s->ss_dac, s->es_value, s->out_ann, ANN_VOLTAGE_UPDATE,
                  ann_str, short_str);
    } else {
        char ann_str[64];
        snprintf(ann_str, sizeof(ann_str), "Setting %s register value to %s", dac_names[s->dac_select], v_str);
        char short_str[48];
        snprintf(short_str, sizeof(short_str), "%s=%s", dac_names[s->dac_select], v_str);
        c_put(di, s->ss_dac, s->es_value, s->out_ann, ANN_REG_WRITE,
                  ann_str, short_str);
    }

    s->dacval[s->dac_select] = s->dac_value;
    s->gains[s->dac_select] = s->gain;
}

static void tlc5620_handle_ldac_fall(struct srd_decoder_inst *di, tlc5620_state *s, uint64_t samplenum)
{
    (void)samplenum;
    c_put(di, samplenum, samplenum, s->out_ann, ANN_LDAC_FALL,
              "Falling edge on LDAC", "LDAC fall", "LDAC", "L");

    if (s->ss_dac_first == (uint64_t)-1)
        return;

    const char *dac_names[] = {"A", "B", "C", "D"};
    char full_str[128];
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        if (s->dacval[i] < 0) {
            pos += snprintf(full_str + pos, sizeof(full_str) - pos, "DAC%s=? ", dac_names[i]);
        } else {
            double v = s->vref[i] * ((double)s->dacval[i] / 256.0) * s->gains[i];
            pos += snprintf(full_str + pos, sizeof(full_str) - pos, "DAC%s=%.2fV ", dac_names[i], v);
        }
    }
    if (pos > 0 && full_str[pos - 1] == ' ')
        full_str[pos - 1] = '\0';

    char short_str[128];
    int spos = 0;
    for (int i = 0; i < 4; i++) {
        if (s->dacval[i] < 0) {
            spos += snprintf(short_str + spos, sizeof(short_str) - spos, "%s=? ", dac_names[i]);
        } else {
            double v = s->vref[i] * ((double)s->dacval[i] / 256.0) * s->gains[i];
            spos += snprintf(short_str + spos, sizeof(short_str) - spos, "%s=%.2fV ", dac_names[i], v);
        }
    }
    if (spos > 0 && short_str[spos - 1] == ' ')
        short_str[spos - 1] = '\0';

    char update_str[256];
    snprintf(update_str, sizeof(update_str), "Updating voltages: %s", full_str);
    c_put(di, s->ss_dac_first, di_samplenum(di), s->out_ann, ANN_VOLTAGE_UPDATE_ALL,
              update_str, full_str, short_str);

    s->ss_dac_first = (uint64_t)-1;
}

static void tlc5620_decode(struct srd_decoder_inst *di)
{
    tlc5620_state *s = (tlc5620_state *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
    }

    while (1) {
        int ret;
        if (s->have_load && s->have_ldac)
            ret = c_wait(di, CW_F(0), CW_OR, CW_F(2), CW_OR, CW_F(3), CW_END);
        else if (s->have_load)
            ret = c_wait(di, CW_F(0), CW_OR, CW_F(2), CW_END);
        else if (s->have_ldac)
            ret = c_wait(di, CW_F(0), CW_OR, CW_F(3), CW_END);
        else
            ret = c_wait(di, CW_F(0), CW_END);
        if (ret != SRD_OK)
            return;

        /* CLK falling edge: sample DATA pin */
        if (di_matched(di) & (1ULL << 0)) {
            int data = c_pin(di, 1);

            /* Track LDAC level */
            if (s->have_ldac)
                s->ldac = c_pin(di, 3);

            if (s->bits_count < TLC5620_MAX_BITS) {
                s->bits_value[s->bits_count] = data;
                s->bits_ss[s->bits_count] = di_samplenum(di);
                s->bits_es[s->bits_count] = di_samplenum(di);
                if (s->bits_count > 0)
                    s->bits_es[s->bits_count - 1] = di_samplenum(di);
            }
            s->bits_count++;
        }

        /* LOAD falling edge */
        if (s->have_load && s->cond_idx_load >= 0 &&
            (di_matched(di) & (1ULL << s->cond_idx_load))) {
            tlc5620_handle_load_fall(di, s, di_samplenum(di));
        }

        /* LDAC falling edge */
        if (s->have_ldac && s->cond_idx_ldac >= 0 &&
            (di_matched(di) & (1ULL << s->cond_idx_ldac))) {
            s->ldac = 0;
            tlc5620_handle_ldac_fall(di, s, di_samplenum(di));
        }
    }
}

static void tlc5620_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder tlc5620_c_decoder = {
    .id = "tlc5620_c",
    .name = "TLC5620(C)",
    .longname = "Texas Instruments TLC5620 (C)",
    .desc = "Texas Instruments TLC5620 8-bit quad DAC. (C implementation)",
    .license = "gplv2+",
    .channels = tlc5620_channels,
    .num_channels = 2,
    .optional_channels = tlc5620_optional_channels,
    .num_optional_channels = 2,
    .options = tlc5620_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = tlc5620_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = tlc5620_ann_rows,
    .inputs = tlc5620_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = tlc5620_tags,
    .num_tags = 2,
    .reset = tlc5620_reset,
    .start = tlc5620_start,
    .decode = tlc5620_decode,
    .destroy = tlc5620_destroy,
    .state_size = 0,
    .metadata = tlc5620_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    tlc5620_options[0].id = "vref_a";
    tlc5620_options[0].idn = "dec_tlc5620_opt_vref_a";
    tlc5620_options[0].desc = "Reference voltage DACA (V)";
    tlc5620_options[0].def = g_variant_new_double(3.3);
    tlc5620_options[0].values = NULL;

    tlc5620_options[1].id = "vref_b";
    tlc5620_options[1].idn = "dec_tlc5620_opt_vref_b";
    tlc5620_options[1].desc = "Reference voltage DACB (V)";
    tlc5620_options[1].def = g_variant_new_double(3.3);
    tlc5620_options[1].values = NULL;

    tlc5620_options[2].id = "vref_c";
    tlc5620_options[2].idn = "dec_tlc5620_opt_vref_c";
    tlc5620_options[2].desc = "Reference voltage DACC (V)";
    tlc5620_options[2].def = g_variant_new_double(3.3);
    tlc5620_options[2].values = NULL;

    tlc5620_options[3].id = "vref_d";
    tlc5620_options[3].idn = "dec_tlc5620_opt_vref_d";
    tlc5620_options[3].desc = "Reference voltage DACD (V)";
    tlc5620_options[3].def = g_variant_new_double(3.3);
    tlc5620_options[3].values = NULL;

    return &tlc5620_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}