#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CELSIUS = 0,
    ANN_KELVIN,
    ANN_TEXT_VERBOSE,
    ANN_TEXT,
    ANN_WARN,
    NUM_ANN,
};

enum lm75_state {
    LM75_IDLE,
    LM75_GET_SLAVE_ADDR,
    LM75_READ_REGS,
    LM75_WRITE_REGS,
};

static const int lm75_resolution_map[] = {9, 10, 11, 12};
static const int lm75_ft_map[] = {1, 2, 4, 6};

typedef struct {
    enum lm75_state state;
    int reg;
    uint8_t databytes[2];
    int num_databytes;
    uint64_t ss;
    uint64_t es;
    uint64_t ss_block;
    uint64_t es_block;
    int out_ann;
    int sensor_is_lm75;
    int resolution;
} lm75_state;

static const char *lm75_inputs[] = {"i2c", NULL};
static const char *lm75_tags[] = {"Sensor", NULL};

static struct srd_decoder_option lm75_options[] = {
    {"sensor", "dec_lm75_opt_sensor", "Sensor type", NULL, NULL},
    {"resolution", "dec_lm75_opt_resolution", "Resolution (bits)", NULL, NULL},
};

static const char *lm75_ann_labels[][3] = {
    {"", "celsius", "Temperature in degrees Celsius"},
    {"", "kelvin", "Temperature in Kelvin"},
    {"", "text-verbose", "Human-readable text (verbose)"},
    {"", "text", "Human-readable text"},
    {"", "warnings", "Human-readable warnings"},
};

static const int lm75_row_celsius_classes[] = {ANN_CELSIUS};
static const int lm75_row_kelvin_classes[] = {ANN_KELVIN};
static const int lm75_row_text_verbose_classes[] = {ANN_TEXT_VERBOSE};
static const int lm75_row_text_classes[] = {ANN_TEXT};
static const int lm75_row_warnings_classes[] = {ANN_WARN};
static const struct srd_c_ann_row lm75_ann_rows[] = {
    {"celsius", "Temperature in degrees Celsius", lm75_row_celsius_classes, 1},
    {"kelvin", "Temperature in Kelvin", lm75_row_kelvin_classes, 1},
    {"text-verbose", "Human-readable text (verbose)", lm75_row_text_verbose_classes, 1},
    {"text", "Human-readable text", lm75_row_text_classes, 1},
    {"warnings", "Human-readable warnings", lm75_row_warnings_classes, 1},
};

static void lm75_putx(struct srd_decoder_inst *di, lm75_state *s, int cls, const char *text)
{
    c_put(di, s->ss, s->es, s->out_ann, cls, text);
}

static void lm75_putb(struct srd_decoder_inst *di, lm75_state *s, int cls, const char *text)
{
    c_put(di, s->ss_block, s->es_block, s->out_ann, cls, text);
}

static void lm75_warn_upon_invalid_slave(struct srd_decoder_inst *di, lm75_state *s, int addr)
{
    if (addr < 0x48 || addr > 0x4f) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Warning: I²C slave 0x%02x not an LM75 compatible sensor.", addr);
        lm75_putx(di, s, ANN_WARN, buf);
    }
}

static void lm75_output_temperature(struct srd_decoder_inst *di, lm75_state *s, const char *reg_name, const char *rw)
{
    (void)rw;
    int before = s->databytes[0];
    int after = (s->databytes[1] >> 7) * 5;
    double celsius = (double)before + (double)after / 10.0;
    double kelvin = celsius + 273.15;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s: %.1f °C", reg_name, celsius);
    lm75_putb(di, s, ANN_CELSIUS, buf);

    snprintf(buf, sizeof(buf), "%s: %.1f K", reg_name, kelvin);
    lm75_putb(di, s, ANN_KELVIN, buf);

    if (strcmp(reg_name, "Temperature") == 0 && strcmp(rw, "WRITE") == 0) {
        lm75_putb(di, s, ANN_WARN, "Warning: The temperature register is read-only!");
    }
}

static void lm75_handle_temperature_reg(struct srd_decoder_inst *di, lm75_state *s, const char *reg_name, const char *rw, uint8_t b)
{
    if (s->num_databytes == 0) {
        s->ss_block = s->ss;
        s->databytes[s->num_databytes++] = b;
        return;
    }
    s->databytes[s->num_databytes++] = b;
    s->es_block = s->es;
    lm75_output_temperature(di, s, reg_name, rw);
    s->num_databytes = 0;
}

static void lm75_handle_reg_0x00(struct srd_decoder_inst *di, lm75_state *s, uint8_t b, const char *rw)
{
    (void)rw;
    lm75_handle_temperature_reg(di, s, "Temperature", rw, b);
}

static void lm75_handle_reg_0x01(struct srd_decoder_inst *di, lm75_state *s, uint8_t b, const char *rw)
{
    (void)rw;
    int sd = b & (1 << 0);
    const char *tmp = (sd == 0) ? "normal operation" : "shutdown mode";
    char s_buf[256];
    int pos = 0;
    pos += snprintf(s_buf + pos, sizeof(s_buf) - pos, "SD = %d: %s\n", sd, tmp);
    char s2[128];
    int pos2 = 0;
    pos2 += snprintf(s2 + pos2, sizeof(s2) - pos2, "SD = %s, ", tmp);

    int cmp_int = b & (1 << 1);
    tmp = (cmp_int == 0) ? "comparator" : "interrupt";
    pos += snprintf(s_buf + pos, sizeof(s_buf) - pos, "CMP/INT = %d: %s mode\n", cmp_int, tmp);
    pos2 += snprintf(s2 + pos2, sizeof(s2) - pos2, "CMP/INT = %s, ", tmp);

    int pol = b & (1 << 2);
    tmp = (pol == 0) ? "low" : "high";
    pos += snprintf(s_buf + pos, sizeof(s_buf) - pos, "POL = %d: OS polarity is active-%s\n", pol, tmp);
    pos2 += snprintf(s2 + pos2, sizeof(s2) - pos2, "POL = active-%s, ", tmp);

    int bits = (b & ((1 << 4) | (1 << 3))) >> 3;
    pos += snprintf(s_buf + pos, sizeof(s_buf) - pos, "Fault tolerance setting: %d bit(s)\n", lm75_ft_map[bits]);
    pos2 += snprintf(s2 + pos2, sizeof(s2) - pos2, "FT = %d", lm75_ft_map[bits]);

    if (!s->sensor_is_lm75) {
        bits = (b & ((1 << 6) | (1 << 5))) >> 5;
        pos += snprintf(s_buf + pos, sizeof(s_buf) - pos, "Resolution: %d bits\n", lm75_resolution_map[bits]);
        pos2 += snprintf(s2 + pos2, sizeof(s2) - pos2, ", resolution = %d", lm75_resolution_map[bits]);
    }

    lm75_putx(di, s, ANN_TEXT_VERBOSE, s_buf);
    lm75_putx(di, s, ANN_TEXT, s2);
}

static void lm75_handle_reg_0x02(struct srd_decoder_inst *di, lm75_state *s, uint8_t b, const char *rw)
{
    (void)rw;
    lm75_handle_temperature_reg(di, s, "T_HYST trip temperature", rw, b);
}

static void lm75_handle_reg_0x03(struct srd_decoder_inst *di, lm75_state *s, uint8_t b, const char *rw)
{
    (void)rw;
    lm75_handle_temperature_reg(di, s, "T_OS trip temperature", rw, b);
}

typedef void (*lm75_handle_reg_fn)(struct srd_decoder_inst *, lm75_state *, uint8_t, const char *);

static const lm75_handle_reg_fn lm75_reg_handlers[4] = {
    lm75_handle_reg_0x00,
    lm75_handle_reg_0x01,
    lm75_handle_reg_0x02,
    lm75_handle_reg_0x03,
};

static void lm75_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    lm75_state *s = (lm75_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (s->state == LM75_IDLE) {
        if (strcmp(cmd, "START") != 0)
            return;
        s->state = LM75_GET_SLAVE_ADDR;
    } else if (s->state == LM75_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS READ") == 0 || strcmp(cmd, "ADDRESS WRITE") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            lm75_warn_upon_invalid_slave(di, s, addr);
            if (strcmp(cmd, "ADDRESS READ") == 0)
                s->state = LM75_READ_REGS;
            else
                s->state = LM75_WRITE_REGS;
            s->num_databytes = 0;
        }
    } else if (s->state == LM75_READ_REGS || s->state == LM75_WRITE_REGS) {
        const char *rw = (s->state == LM75_READ_REGS) ? "READ" : "WRITE";

        if (strcmp(cmd, "DATA READ") == 0 || strcmp(cmd, "DATA WRITE") == 0) {
            uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
            if (s->reg >= 0 && s->reg < 4) {
                lm75_reg_handlers[s->reg](di, s, databyte, rw);
            }
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = LM75_IDLE;
        }
    }
}

static void lm75_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(lm75_state)));
    }
    lm75_state *s = (lm75_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(lm75_state));
    s->state = LM75_IDLE;
    s->reg = 0x00;
    s->sensor_is_lm75 = 1;
    s->resolution = 9;
}

static void lm75_start(struct srd_decoder_inst *di)
{
    lm75_state *s = (lm75_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "lm75");

    const char *sensor = c_opt_str(di, "sensor", "lm75");
    s->sensor_is_lm75 = (sensor && strcmp(sensor, "lm75") == 0) ? 1 : 0;
    s->resolution = (int)c_opt_int(di, "resolution", 9);
}

static void lm75_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void lm75_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder lm75_c_decoder = {
    .id = "lm75_c",
    .name = "Lm75(C)",
    .longname = "National LM75 (C)",
    .desc = "National LM75 (and compatibles) temperature sensor. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = lm75_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = lm75_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = lm75_ann_rows,
    .inputs = lm75_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = lm75_tags,
    .num_tags = 1,
    .reset = lm75_reset,
    .start = lm75_start,
    .decode = lm75_decode,
    .destroy = lm75_destroy,
    .decode_upper = lm75_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    lm75_options[0].def = g_variant_new_string("lm75");
    GSList *sensor_vals = NULL;
    sensor_vals = g_slist_append(sensor_vals, g_variant_new_string("lm75"));
    lm75_options[0].values = sensor_vals;

    lm75_options[1].def = g_variant_new_int64(9);
    GSList *res_vals = NULL;
    res_vals = g_slist_append(res_vals, g_variant_new_int64(9));
    res_vals = g_slist_append(res_vals, g_variant_new_int64(10));
    res_vals = g_slist_append(res_vals, g_variant_new_int64(11));
    res_vals = g_slist_append(res_vals, g_variant_new_int64(12));
    lm75_options[1].values = res_vals;

    return &lm75_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}