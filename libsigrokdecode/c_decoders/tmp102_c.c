#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    /* AnnAddrs (0-4) */
    ANN_ADDR_GC = 0,
    ANN_ADDR_GND,
    ANN_ADDR_VCC,
    ANN_ADDR_SDA,
    ANN_ADDR_SCL,
    /* AnnRegs (5-9) */
    ANN_REG_RESET,
    ANN_REG_CONF,
    ANN_REG_TEMP,
    ANN_REG_TLOW,
    ANN_REG_THIGH,
    /* AnnBits (10-20) */
    ANN_BIT_RESERVED,
    ANN_BIT_DATA,
    ANN_BIT_EM,
    ANN_BIT_AL,
    ANN_BIT_CR0,
    ANN_BIT_SD,
    ANN_BIT_TM,
    ANN_BIT_POL,
    ANN_BIT_F0,
    ANN_BIT_R0,
    ANN_BIT_OS,
    /* AnnInfo (21-34) */
    ANN_INFO_WARN,
    ANN_INFO_BADADD,
    ANN_INFO_GRST,
    ANN_INFO_CHECK,
    ANN_INFO_WRITE,
    ANN_INFO_READ,
    ANN_INFO_SELECT,
    ANN_INFO_CUSTOM,
    ANN_INFO_PWRUP,
    ANN_INFO_CONF,
    ANN_INFO_TEMP,
    ANN_INFO_TLOW,
    ANN_INFO_THIGH,
    NUM_ANN,
};

enum tmp102_state {
    TMP102_IDLE,
    TMP102_ADDRESS_SLAVE,
    TMP102_REGISTER_ADDRESS,
    TMP102_REGISTER_DATA,
};

/* General Call constants */
#define GC_ADDRESS  0x00
#define GC_RESET    0x06

/* Slave addresses */
#define ADDR_GND    0x48
#define ADDR_VCC    0x49
#define ADDR_SDA    0x4A
#define ADDR_SCL    0x4B

/* Register addresses */
#define REG_TEMP    0x00
#define REG_CONF    0x01
#define REG_TLOW    0x02
#define REG_THIGH   0x03

/* Power-up default config */
#define PWRUP_CONF  0x60a0

typedef struct {
    enum tmp102_state state;
    int addr;
    int reg;
    int em;             /* Extended mode flag */
    int write;          /* Write operation flag */
    uint8_t bytes[4];
    int bytes_len;
    uint64_t ssd;       /* Data block start sample */
    uint64_t ssb;       /* Block start sample */
    
    int out_ann;
    int radix;          /* 0=Hex, 1=Dec, 2=Oct, 3=Bin */
    int units;          /* 0=Celsius, 1=Fahrenheit, 2=Kelvin */
    uint64_t ss;
    uint64_t es;
} tmp102_state;

static const char *tmp102_inputs[] = {"i2c", NULL};
static const char *tmp102_outputs[] = {"tmp102", NULL};
static const char *tmp102_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option tmp102_options[] = {
    {"radix", "dec_tmp102_opt_radix", "Number format", NULL, NULL},
    {"units", "dec_tmp102_opt_units", "Temperature unit", NULL, NULL},
};

static const char *tmp102_ann_labels[][3] = {
    /* AnnAddrs (0-4) */
    {"", "gc", "General call"},
    {"", "gnd", "ADD0 grounded"},
    {"", "vcc", "ADD0 powered"},
    {"", "sda", "ADD0 to SDA"},
    {"", "scl", "ADD0 to SCL"},
    /* AnnRegs (5-9) */
    {"", "reset", "Reset register"},
    {"", "conf", "Configuration register"},
    {"", "temp", "Temperature register"},
    {"", "tlow", "Low alert register"},
    {"", "thigh", "High alert register"},
    /* AnnBits (10-20) */
    {"", "bit_reserved", "Reserved"},
    {"", "bit_data", "Data"},
    {"", "bit_em", "Extended mode"},
    {"", "bit_al", "Alert"},
    {"", "bit_cr0", "Conversion rate"},
    {"", "bit_sd", "Shutdown mode"},
    {"", "bit_tm", "Thermostat mode"},
    {"", "bit_pol", "Polarity"},
    {"", "bit_f0", "Consecutive faults"},
    {"", "bit_r0", "Converter resolution"},
    {"", "bit_os", "One-shot conversion"},
    /* AnnInfo (21-34) */
    {"", "warn", "Warnings"},
    {"", "badadd", "Unknown slave address"},
    {"", "grst", "General reset"},
    {"", "check", "Slave presence check"},
    {"", "write", "Write"},
    {"", "read", "Read"},
    {"", "select", "Select"},
    {"", "custom", "Custom"},
    {"", "pwrup", "Power-up reset"},
    {"", "info_conf", "Configuration"},
    {"", "info_temp", "Measured temperature"},
    {"", "info_tlow", "Low temperature limit"},
    {"", "info_thigh", "High temperature limit"},
};

static const int tmp102_row_bits_classes[] = {
    ANN_BIT_RESERVED, ANN_BIT_DATA, ANN_BIT_EM, ANN_BIT_AL, ANN_BIT_CR0,
    ANN_BIT_SD, ANN_BIT_TM, ANN_BIT_POL, ANN_BIT_F0, ANN_BIT_R0, ANN_BIT_OS
};
static const int tmp102_row_regs_classes[] = {
    ANN_ADDR_GC, ANN_ADDR_GND, ANN_ADDR_VCC, ANN_ADDR_SDA, ANN_ADDR_SCL,
    ANN_REG_RESET, ANN_REG_CONF, ANN_REG_TEMP, ANN_REG_TLOW, ANN_REG_THIGH
};
static const int tmp102_row_info_classes[] = {
    ANN_INFO_GRST, ANN_INFO_CHECK, ANN_INFO_WRITE, ANN_INFO_READ,
    ANN_INFO_SELECT, ANN_INFO_CUSTOM, ANN_INFO_PWRUP,
    ANN_INFO_CONF, ANN_INFO_TEMP, ANN_INFO_TLOW, ANN_INFO_THIGH
};
static const int tmp102_row_warnings_classes[] = {ANN_INFO_WARN, ANN_INFO_BADADD};

static const struct srd_c_ann_row tmp102_ann_rows[] = {
    {"bits", "Bits", tmp102_row_bits_classes, 11},
    {"regs", "Registers", tmp102_row_regs_classes, 10},
    {"info", "Info", tmp102_row_info_classes, 11},
    {"warnings", "Warnings", tmp102_row_warnings_classes, 2},
};

static const char *rates[] = {"0.25", "1", "4", "8"};
static const char *faults[] = {"1", "2", "4", "6"};

static int tmp102_check_addr(int addr, int check_gencall)
{
    if (addr == ADDR_GND || addr == ADDR_VCC || addr == ADDR_SDA || addr == ADDR_SCL)
        return 1;
    if (check_gencall && addr == GC_ADDRESS)
        return 1;
    return 0;
}

static double tmp102_calculate_temperature(tmp102_state *s, int rawdata)
{
    if (rawdata & (1 << 0))
        s->em = 1;

    if (s->em) {
        /* Extended mode (13-bit) */
        rawdata >>= 3;
        if (rawdata > 0x0fff)
            rawdata |= 0xe000;  /* 2's complement sign extension */
    } else {
        /* Normal mode (12-bit) */
        rawdata >>= 4;
        if (rawdata > 0x07ff)
            rawdata |= 0xf000;  /* 2's complement sign extension */
    }

    double temperature = (double)rawdata / 16.0;  /* Celsius */

    if (s->units == 1) {  /* Fahrenheit */
        temperature = temperature * 9.0 / 5.0 + 32.0;
    } else if (s->units == 2) {  /* Kelvin */
        temperature += 273.15;
    }

    return temperature;
}

static const char *tmp102_unit_str(int units)
{
    switch (units) {
    case 1: return "°F";
    case 2: return "K";
    default: return "°C";
    }
}

static void tmp102_handle_addr_ann(struct srd_decoder_inst *di, tmp102_state *s, int addr)
{
    int ann;
    switch (addr) {
    case GC_ADDRESS: ann = ANN_ADDR_GC; break;
    case ADDR_GND:   ann = ANN_ADDR_GND; break;
    case ADDR_VCC:   ann = ANN_ADDR_VCC; break;
    case ADDR_SDA:   ann = ANN_ADDR_SDA; break;
    case ADDR_SCL:   ann = ANN_ADDR_SCL; break;
    default: return;
    }
    const char *labels[] = {
        "General call", "GEN_CALL", "GC", "G",
        "ADD0 grounded", "ADD0_GND", "AG",
        "ADD0 powered", "ADD0_VCC", "AV",
        "ADD0 to SDA", "ADD0_SDA", "AD",
        "ADD0 to SCL", "ADD0_SCL", "AC",
    };
    int idx = ann * 3;
    if (ann <= ANN_ADDR_SCL)
        c_put(di, s->ss, s->es, s->out_ann, ann, labels[idx], labels[idx+1], labels[idx+2]);
}

static void tmp102_handle_reg_ann(struct srd_decoder_inst *di, tmp102_state *s, int reg)
{
    int ann;
    const char *name;
    switch (reg) {
    case REG_TEMP: ann = ANN_REG_TEMP; name = "Temperature"; break;
    case REG_CONF: ann = ANN_REG_CONF; name = "Configuration"; break;
    case REG_TLOW: ann = ANN_REG_TLOW; name = "Low alert"; break;
    case REG_THIGH: ann = ANN_REG_THIGH; name = "High alert"; break;
    default: return;
    }
    c_put(di, s->ss, s->es, s->out_ann, ann, name);
}

static void tmp102_handle_datareg_0x00(struct srd_decoder_inst *di, tmp102_state *s, int dataword)
{
    double temp = tmp102_calculate_temperature(s, dataword);
    const char *unit = tmp102_unit_str(s->units);
    char buf[64];
    snprintf(buf, sizeof(buf), "Temperature: %.2f %s", temp, unit);
    c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_TEMP, buf);
    /* Register annotation */
    snprintf(buf, sizeof(buf), "Temperature register: 0x%04X", dataword);
    c_put(di, s->ssd, s->es, s->out_ann, ANN_REG_TEMP, buf);
}

static void tmp102_handle_datareg_0x01(struct srd_decoder_inst *di, tmp102_state *s, int dataword)
{
    /* Configuration register */
    int os = (dataword >> 15) & 1;
    int r = (dataword >> 13) & 3;
    int flt = (dataword >> 11) & 3;
    int pol = (dataword >> 10) & 1;
    int tm = (dataword >> 9) & 1;
    int sd = (dataword >> 8) & 1;
    int cr = (dataword >> 6) & 3;
    int al = (dataword >> 5) & 1;
    int em = (dataword >> 4) & 1;
    s->em = em;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "Configuration: OS=%s R=%s F=%s POL=%s TM=%s SD=%s CR=%sHz AL=%d EM=%s",
             os ? "enabled" : "disabled",
             (r == 3) ? "12bit" : "?",
             faults[flt],
             pol ? "high" : "low",
             tm ? "interrupt" : "comparator",
             sd ? "enabled" : "disabled",
             rates[cr],
             al,
             em ? "enabled" : "disabled");
    c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_CONF, buf);

    /* Check for power-up default */
    if (dataword == PWRUP_CONF) {
        c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_PWRUP, "Power-up reset");
    } else {
        c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_CUSTOM, "Custom");
    }

    /* Register annotation */
    char reg_buf[64];
    snprintf(reg_buf, sizeof(reg_buf), "Configuration register: 0x%04X", dataword);
    c_put(di, s->ssd, s->es, s->out_ann, ANN_REG_CONF, reg_buf);
}

static void tmp102_handle_datareg_0x02(struct srd_decoder_inst *di, tmp102_state *s, int dataword)
{
    double temp = tmp102_calculate_temperature(s, dataword);
    const char *unit = tmp102_unit_str(s->units);
    char buf[64];
    const char *rw = s->write ? "Write" : "Read";
    snprintf(buf, sizeof(buf), "Low alert: %.2f %s (%s)", temp, unit, rw);
    c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_TLOW, buf);
    snprintf(buf, sizeof(buf), "Low alert register: 0x%04X", dataword);
    c_put(di, s->ssd, s->es, s->out_ann, ANN_REG_TLOW, buf);
}

static void tmp102_handle_datareg_0x03(struct srd_decoder_inst *di, tmp102_state *s, int dataword)
{
    double temp = tmp102_calculate_temperature(s, dataword);
    const char *unit = tmp102_unit_str(s->units);
    char buf[64];
    const char *rw = s->write ? "Write" : "Read";
    snprintf(buf, sizeof(buf), "High alert: %.2f %s (%s)", temp, unit, rw);
    c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_THIGH, buf);
    snprintf(buf, sizeof(buf), "High alert register: 0x%04X", dataword);
    c_put(di, s->ssd, s->es, s->out_ann, ANN_REG_THIGH, buf);
}

static void tmp102_handle_datareg_0x06(struct srd_decoder_inst *di, tmp102_state *s)
{
    c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_GRST, "General reset");
}

static void tmp102_handle_data(struct srd_decoder_inst *di, tmp102_state *s)
{
    if (s->bytes_len < 2)
        return;

    int dataword = (s->bytes[0] << 8) | s->bytes[1];

    if (s->addr == GC_ADDRESS) {
        if (s->reg == GC_RESET)
            tmp102_handle_datareg_0x06(di, s);
        return;
    }

    switch (s->reg) {
    case REG_TEMP: tmp102_handle_datareg_0x00(di, s, dataword); break;
    case REG_CONF: tmp102_handle_datareg_0x01(di, s, dataword); break;
    case REG_TLOW: tmp102_handle_datareg_0x02(di, s, dataword); break;
    case REG_THIGH: tmp102_handle_datareg_0x03(di, s, dataword); break;
    }
}

static void tmp102_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tmp102_state *s = (tmp102_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    /* Ignore BITS packets */
    if (strcmp(cmd, "BITS") == 0)
        return;

    if (s->state == TMP102_IDLE) {
        if (strcmp(cmd, "START") == 0) {
            s->ssb = start_sample;
            s->bytes_len = 0;
            s->state = TMP102_ADDRESS_SLAVE;
        }
    } else if (s->state == TMP102_ADDRESS_SLAVE) {
        if (strcmp(cmd, "ADDRESS WRITE") == 0 || strcmp(cmd, "ADDRESS READ") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (tmp102_check_addr(addr, 1)) {
                s->addr = addr;
                tmp102_handle_addr_ann(di, s, addr);
                if (strcmp(cmd, "ADDRESS READ") == 0) {
                    s->write = 0;
                    s->state = TMP102_REGISTER_DATA;
                } else {
                    s->write = 1;
                    s->state = TMP102_REGISTER_ADDRESS;
                }
                s->bytes_len = 0;
            } else {
                /* Use s->addr (default address) for BADADD text, matching Python's
                 * check_addr() which uses self.addr and compose_annot for variants */
                char buf1[64], buf2[64], buf3[48], buf4[32], buf5[16];
                snprintf(buf1, sizeof(buf1), "Unknown slave address: 0x%02X", s->addr);
                snprintf(buf2, sizeof(buf2), "Unknown address: 0x%02X", s->addr);
                snprintf(buf3, sizeof(buf3), "Unknown: 0x%02X", s->addr);
                snprintf(buf4, sizeof(buf4), "Unk: 0x%02X", s->addr);
                snprintf(buf5, sizeof(buf5), "U: 0x%02X", s->addr);
                c_put(di, s->ss, s->es, s->out_ann, ANN_INFO_BADADD,
                          buf1, buf2, buf3, buf4, buf5, "Unk", "U");
                s->state = TMP102_IDLE;
            }
        }
    } else if (s->state == TMP102_REGISTER_ADDRESS) {
        if (strcmp(cmd, "DATA WRITE") == 0 || strcmp(cmd, "DATA READ") == 0) {
            s->reg = (n_fields > 0) ? fields[0].u8 : 0;
            tmp102_handle_reg_ann(di, s, s->reg);
            s->state = TMP102_REGISTER_DATA;
            s->bytes_len = 0;
        } else if (strcmp(cmd, "STOP") == 0 || strcmp(cmd, "START REPEAT") == 0) {
            c_put(di, s->ssb, s->es, s->out_ann, ANN_INFO_CHECK, "Slave presence check");
            s->state = TMP102_IDLE;
        }
    } else if (s->state == TMP102_REGISTER_DATA) {
        if (strcmp(cmd, "DATA WRITE") == 0 || strcmp(cmd, "DATA READ") == 0) {
            uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
            if (s->bytes_len == 0) {
                s->ssd = start_sample;
                s->bytes[s->bytes_len++] = databyte;
            } else if (s->bytes_len < 2) {
                s->bytes[s->bytes_len++] = databyte;
            }
        } else if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = TMP102_ADDRESS_SLAVE;
            s->bytes_len = 0;
        } else if (strcmp(cmd, "STOP") == 0) {
            tmp102_handle_data(di, s);
            s->state = TMP102_IDLE;
            s->bytes_len = 0;
        }
    }
}

static void tmp102_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tmp102_state)));
    }
    tmp102_state *s = (tmp102_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tmp102_state));
    s->state = TMP102_IDLE;
    s->addr = ADDR_GND;
    s->reg = REG_TEMP;
    s->write = 1;

    const char *radix_str = c_opt_str(di, "radix", "Hex");
    if (radix_str && strcmp(radix_str, "Dec") == 0) s->radix = 1;
    else if (radix_str && strcmp(radix_str, "Oct") == 0) s->radix = 2;
    else if (radix_str && strcmp(radix_str, "Bin") == 0) s->radix = 3;
    else s->radix = 0;

    const char *units_str = c_opt_str(di, "units", "Celsius");
    if (units_str && strcmp(units_str, "Fahrenheit") == 0) s->units = 1;
    else if (units_str && strcmp(units_str, "Kelvin") == 0) s->units = 2;
    else s->units = 0;
}

static void tmp102_start(struct srd_decoder_inst *di)
{
    tmp102_state *s = (tmp102_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tmp102");

    const char *radix_str = c_opt_str(di, "radix", "Hex");
    if (radix_str && strcmp(radix_str, "Dec") == 0) s->radix = 1;
    else if (radix_str && strcmp(radix_str, "Oct") == 0) s->radix = 2;
    else if (radix_str && strcmp(radix_str, "Bin") == 0) s->radix = 3;
    else s->radix = 0;

    const char *units_str = c_opt_str(di, "units", "Celsius");
    if (units_str && strcmp(units_str, "Fahrenheit") == 0) s->units = 1;
    else if (units_str && strcmp(units_str, "Kelvin") == 0) s->units = 2;
    else s->units = 0;
}

static void tmp102_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tmp102_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder tmp102_c_decoder = {
    .id = "tmp102_c",
    .name = "TMP102(C)",
    .longname = "Digital temperature sensor TMP102 (C)",
    .desc = "Low power digital temperature sensor. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = tmp102_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = tmp102_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = tmp102_ann_rows,
    .inputs = tmp102_inputs,
    .num_inputs = 1,
    .outputs = tmp102_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = tmp102_tags,
    .num_tags = 1,
    .reset = tmp102_reset,
    .start = tmp102_start,
    .decode = tmp102_decode,
    .destroy = tmp102_destroy,
    .decode_upper = tmp102_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    tmp102_options[0].def = g_variant_new_string("Hex");
    GSList *radix_vals = NULL;
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Hex"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Dec"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Oct"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Bin"));
    tmp102_options[0].values = radix_vals;

    tmp102_options[1].def = g_variant_new_string("Celsius");
    GSList *units_vals = NULL;
    units_vals = g_slist_append(units_vals, g_variant_new_string("Celsius"));
    units_vals = g_slist_append(units_vals, g_variant_new_string("Fahrenheit"));
    units_vals = g_slist_append(units_vals, g_variant_new_string("Kelvin"));
    tmp102_options[1].values = units_vals;

    return &tmp102_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}