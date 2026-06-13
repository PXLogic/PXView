#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_ADDR_GND = 0,
    ANN_ADDR_VCC,
    ANN_PWRDOWN,
    ANN_PWRUP,
    ANN_RESET,
    ANN_MTHIGH,
    ANN_MTLOW,
    ANN_MCHIGH,
    ANN_MCHIGH2,
    ANN_MCLOW,
    ANN_MOHIGH,
    ANN_MOHIGH2,
    ANN_MOLOW,
    ANN_DATA,
    ANN_RESERVED,
    ANN_DATA_MT,
    ANN_WARN,
    ANN_BADADD,
    ANN_CHECK,
    ANN_WRITE,
    ANN_READ,
    ANN_SENSE,
    ANN_LIGHT,
    ANN_MTREG,
    ANN_MTIME,
    NUM_ANN,
};

enum bh1750_state {
    BH1750_IDLE,
    BH1750_ADDRESS_SLAVE,
    BH1750_REGISTER_ADDRESS,
    BH1750_REGISTER_DATA,
};

/* Slave addresses */
#define BH1750_ADDR_GND 0x23
#define BH1750_ADDR_VCC 0x5C

/* Register/command values */
#define BH1750_REG_PWRDOWN  0x00
#define BH1750_REG_PWRUP    0x01
#define BH1750_REG_RESET    0x07
#define BH1750_REG_MTHIGH   0x40
#define BH1750_REG_MTLOW    0x60
#define BH1750_REG_MCHIGH   0x10
#define BH1750_REG_MCHIGH2  0x11
#define BH1750_REG_MCLOW    0x13
#define BH1750_REG_MOHIGH   0x20
#define BH1750_REG_MOHIGH2  0x21
#define BH1750_REG_MOLOW    0x23

/* Parameters */
#define BH1750_MTREG_TYP   69
#define BH1750_ACCURACY_TYP 1.20
#define BH1750_ACCURACY_MAX 1.44
#define BH1750_ACCURACY_MIN 0.96

typedef struct {
    enum bh1750_state state;
    int addr;
    int is_write;
    int reg;
    int mode;
    int mtreg;
    uint8_t data_bytes[4];
    int num_data;
    
    uint64_t ssb;  /* start sample of block */
    uint64_t ssd;  /* start sample of data */
    uint64_t ss, es;
    int out_ann;
    int out_proto;
    int radix;    /* 0=Hex, 1=Dec, 2=Oct, 3=Bin */
    int params;   /* 0=Typical, 1=Maximal, 2=Minimal */
} bh1750_state;

static const char *bh1750_inputs[] = {"i2c", NULL};
static const char *bh1750_outputs[] = {"bh1750", NULL};
static const char *bh1750_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option bh1750_options[] = {
    {"radix", "dec_bh1750_opt_radix", "Number format", NULL, NULL},
    {"params", "dec_bh1750_opt_params", "Datasheet parameter used", NULL, NULL},
};

static const char *bh1750_ann_labels[][3] = {
    {"", "addr_gnd", "ADDR grounded"},             /* 0 */
    {"", "addr_vcc", "ADDR powered"},              /* 1 */
    {"", "pwrdwn", "Power down"},                  /* 2 */
    {"", "pwrup", "Power up"},                     /* 3 */
    {"", "reset", "Reset light register"},         /* 4 */
    {"", "mthigh", "Measurement time high bits"},  /* 5 */
    {"", "mtlow", "Measurement time low bits"},    /* 6 */
    {"", "mchigh", "Continuous measurement high resolution"}, /* 7 */
    {"", "mchigh2", "Continuous measurement double high res"}, /* 8 */
    {"", "mclow", "Continuous measurement low resolution"}, /* 9 */
    {"", "mohigh", "One time measurement high resolution"}, /* 10 */
    {"", "mohigh2", "One time measurement double high res"}, /* 11 */
    {"", "molow", "One time measurement low resolution"}, /* 12 */
    {"", "data", "Illuminance data register"},     /* 13 */
    {"", "reserved", "Reserved"},                  /* 14 */
    {"", "data_mt", "Measurement time"},           /* 15 */
    {"", "warn", "Warnings"},                      /* 16 */
    {"", "badadd", "Unknown slave address"},       /* 17 */
    {"", "check", "Slave presence check"},         /* 18 */
    {"", "write", "Write"},                        /* 19 */
    {"", "read", "Read"},                          /* 20 */
    {"", "sense", "Sensitivity"},                  /* 21 */
    {"", "light", "Ambient light"},                /* 22 */
    {"", "mtreg", "Measurement time register"},    /* 23 */
    {"", "mtime", "Measurement time"},             /* 24 */
};

static const int bh1750_row_bits_classes[] = {ANN_RESERVED, ANN_DATA_MT, -1};
static const int bh1750_row_regs_classes[] = {
    ANN_ADDR_GND, ANN_ADDR_VCC, ANN_PWRDOWN, ANN_PWRUP, ANN_RESET,
    ANN_MTHIGH, ANN_MTLOW, ANN_MCHIGH, ANN_MCHIGH2, ANN_MCLOW,
    ANN_MOHIGH, ANN_MOHIGH2, ANN_MOLOW, ANN_DATA, -1
};
static const int bh1750_row_info_classes[] = {
    ANN_CHECK, ANN_WRITE, ANN_READ, ANN_SENSE, ANN_LIGHT, ANN_MTREG, ANN_MTIME, -1
};
static const int bh1750_row_warnings_classes[] = {ANN_WARN, ANN_BADADD, -1};

static const struct srd_c_ann_row bh1750_ann_rows[] = {
    {"bits", "Bits", bh1750_row_bits_classes, 2},
    {"regs", "Registers", bh1750_row_regs_classes, 14},
    {"info", "Info", bh1750_row_info_classes, 7},
    {"warnings", "Warnings", bh1750_row_warnings_classes, 2},
};

static double bh1750_calculate_sensitivity(bh1750_state *s)
{
    double accuracy;
    switch (s->params) {
    case 1: accuracy = BH1750_ACCURACY_MAX; break;
    case 2: accuracy = BH1750_ACCURACY_MIN; break;
    default: accuracy = BH1750_ACCURACY_TYP; break;
    }
    double sensitivity = (1.0 / accuracy) * BH1750_MTREG_TYP / s->mtreg;
    if (s->mode == BH1750_REG_MCHIGH2 || s->mode == BH1750_REG_MOHIGH2)
        sensitivity /= 2.0;
    return sensitivity;
}

static void bh1750_handle_data(struct srd_decoder_inst *di, bh1750_state *s)
{
    if (s->is_write) {
        /* Write mode: output MTreg or sensitivity info */
        if (s->reg == BH1750_REG_MTHIGH || s->reg == BH1750_REG_MTLOW) {
            char buf[64];
            snprintf(buf, sizeof(buf), "MTreg: %d", s->mtreg);
            c_put(di, s->ssb, s->es, s->out_ann, ANN_MTREG, buf);
        }
        if (s->mode >= BH1750_REG_MCHIGH && s->mode <= BH1750_REG_MOLOW) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Sensitivity: %.2f lux/cnt", bh1750_calculate_sensitivity(s));
            c_put(di, s->ssb, s->es, s->out_ann, ANN_SENSE, buf);
        }
    } else {
        /* Read mode: output illuminance data */
        if (s->num_data >= 2) {
            int rawdata = (s->data_bytes[1] << 8) | s->data_bytes[0];
            /* Registers row */
            c_put(di, s->ssd, s->es, s->out_ann, ANN_DATA,
                "Illuminance data register", "Illuminance", "Light", "L");
            /* Info row: light value */
            double light = rawdata * bh1750_calculate_sensitivity(s);
            char buf[64];
            snprintf(buf, sizeof(buf), "Ambient light: %.2f lux", light);
            c_put(di, s->ssb, s->es, s->out_ann, ANN_LIGHT, buf);
        }
    }
    /* Clear data cache */
    s->num_data = 0;
}

static void bh1750_handle_register(struct srd_decoder_inst *di, bh1750_state *s, uint8_t reg)
{
    s->reg = reg;

    /* Handle measurement time registers */
    if ((reg & 0xE0) == BH1750_REG_MTHIGH) {
        /* MTHIGH: bits 5-6 are high bits of MTreg */
        int mtreg_high = (reg >> 3) & 0x03;
        s->mtreg = (s->mtreg & 0x1F) | (mtreg_high << 5);
        s->reg = BH1750_REG_MTHIGH;
        c_put(di, s->ss, s->es, s->out_ann, ANN_MTHIGH,
            "Measurement time high bits", "Mtime Hbits", "MTH", "H");
        return;
    }
    if ((reg & 0xE0) == BH1750_REG_MTLOW) {
        /* MTLOW: bits 0-4 are low bits of MTreg */
        s->mtreg = (s->mtreg & 0xE0) | (reg & 0x1F);
        s->reg = BH1750_REG_MTLOW;
        c_put(di, s->ss, s->es, s->out_ann, ANN_MTLOW,
            "Measurement time low bits", "Mtime Lbits", "MTL", "L");
        return;
    }

    /* Detect measurement mode */
    if (reg >= BH1750_REG_MCHIGH && reg <= BH1750_REG_MOLOW) {
        s->mode = reg;
    }

    /* Output register annotation */
    switch (reg) {
    case BH1750_REG_PWRDOWN:
        c_put(di, s->ss, s->es, s->out_ann, ANN_PWRDOWN, "Power down", "Pwr Dwn", "Off", "D");
        break;
    case BH1750_REG_PWRUP:
        c_put(di, s->ss, s->es, s->out_ann, ANN_PWRUP, "Power up", "Pwr Up", "On", "U");
        break;
    case BH1750_REG_RESET:
        c_put(di, s->ss, s->es, s->out_ann, ANN_RESET, "Reset light register", "Reset light", "Reset", "Rst", "R");
        break;
    case BH1750_REG_MCHIGH:
        c_put(di, s->ss, s->es, s->out_ann, ANN_MCHIGH, "Continuous measurement high resolution", "Cont high", "CH");
        break;
    case BH1750_REG_MCHIGH2:
        c_put(di, s->ss, s->es, s->out_ann, ANN_MCHIGH2, "Continuous measurement double high res", "Cont double", "CH2");
        break;
    case BH1750_REG_MCLOW:
        c_put(di, s->ss, s->es, s->out_ann, ANN_MCLOW, "Continuous measurement low resolution", "Cont low", "CL");
        break;
    case BH1750_REG_MOHIGH:
        c_put(di, s->ss, s->es, s->out_ann, ANN_MOHIGH, "One time measurement high resolution", "One high", "OH");
        break;
    case BH1750_REG_MOHIGH2:
        c_put(di, s->ss, s->es, s->out_ann, ANN_MOHIGH2, "One time measurement double high res", "One double", "OH2");
        break;
    case BH1750_REG_MOLOW:
        c_put(di, s->ss, s->es, s->out_ann, ANN_MOLOW, "One time measurement low resolution", "One low", "OL");
        break;
    default:
        c_put(di, s->ss, s->es, s->out_ann, ANN_RESERVED, "Reserved", "Rsvd", "R");
        break;
    }
}

static void bh1750_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    bh1750_state *s = (bh1750_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "BITS") == 0)
        return;

    switch (s->state) {
    case BH1750_IDLE:
        if (strcmp(cmd, "START") == 0) {
            s->ssb = start_sample;
            s->state = BH1750_ADDRESS_SLAVE;
            s->num_data = 0;
        }
        break;
    case BH1750_ADDRESS_SLAVE:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr == BH1750_ADDR_GND || addr == BH1750_ADDR_VCC) {
                s->addr = addr;
                s->is_write = 1;
                int ann = (addr == BH1750_ADDR_GND) ? ANN_ADDR_GND : ANN_ADDR_VCC;
                const char *label = (addr == BH1750_ADDR_GND) ? "ADDR grounded" : "ADDR powered";
                c_put(di, s->ss, s->es, s->out_ann, ann, label);
                c_put(di, s->ss, s->es, s->out_ann, ANN_WRITE, "Write", "Wr", "W");
                s->state = BH1750_REGISTER_ADDRESS;
            } else {
                /* Use s->addr (default address) for BADADD text, matching Python's
                 * check_addr() which uses self.addr rather than the received databyte.
                 * Python's compose_annot generates multiple text variants with address. */
                char buf1[64], buf2[64], buf3[48], buf4[32], buf5[16];
                snprintf(buf1, sizeof(buf1), "Unknown slave address: 0x%02X", s->addr);
                snprintf(buf2, sizeof(buf2), "Unknown address: 0x%02X", s->addr);
                snprintf(buf3, sizeof(buf3), "Unknown: 0x%02X", s->addr);
                snprintf(buf4, sizeof(buf4), "Unk: 0x%02X", s->addr);
                snprintf(buf5, sizeof(buf5), "U: 0x%02X", s->addr);
                c_put(di, s->ss, s->es, s->out_ann, ANN_BADADD,
                          buf1, buf2, buf3, buf4, buf5, "Unk", "U");
                s->state = BH1750_IDLE;
            }
        } else if (strcmp(cmd, "ADDRESS READ") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr == BH1750_ADDR_GND || addr == BH1750_ADDR_VCC) {
                s->addr = addr;
                s->is_write = 0;
                int ann = (addr == BH1750_ADDR_GND) ? ANN_ADDR_GND : ANN_ADDR_VCC;
                const char *label = (addr == BH1750_ADDR_GND) ? "ADDR grounded" : "ADDR powered";
                c_put(di, s->ss, s->es, s->out_ann, ann, label);
                c_put(di, s->ss, s->es, s->out_ann, ANN_READ, "Read", "Rd", "R");
                s->state = BH1750_REGISTER_ADDRESS;
            } else {
                char buf1[64], buf2[64], buf3[48], buf4[32], buf5[16];
                snprintf(buf1, sizeof(buf1), "Unknown slave address: 0x%02X", s->addr);
                snprintf(buf2, sizeof(buf2), "Unknown address: 0x%02X", s->addr);
                snprintf(buf3, sizeof(buf3), "Unknown: 0x%02X", s->addr);
                snprintf(buf4, sizeof(buf4), "Unk: 0x%02X", s->addr);
                snprintf(buf5, sizeof(buf5), "U: 0x%02X", s->addr);
                c_put(di, s->ss, s->es, s->out_ann, ANN_BADADD,
                          buf1, buf2, buf3, buf4, buf5, "Unk", "U");
                s->state = BH1750_IDLE;
            }
        }
        break;
    case BH1750_REGISTER_ADDRESS:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            uint8_t reg = (n_fields > 0) ? fields[0].u8 : 0;
            bh1750_handle_register(di, s, reg);
            s->state = BH1750_REGISTER_DATA;
        } else if (strcmp(cmd, "DATA READ") == 0) {
            uint8_t b = (n_fields > 0) ? fields[0].u8 : 0;
            s->ssd = start_sample;
            s->data_bytes[0] = b;
            s->num_data = 1;
            s->state = BH1750_REGISTER_DATA;
        } else if (strcmp(cmd, "STOP") == 0 || strcmp(cmd, "START REPEAT") == 0) {
            /* No data transfer - slave presence check */
            c_put(di, s->ssb, s->es, s->out_ann, ANN_CHECK, "Slave presence check", "Slave check", "Check", "Chk", "C");
            if (strcmp(cmd, "START REPEAT") == 0) {
                s->ssb = start_sample;
                s->state = BH1750_ADDRESS_SLAVE;
            } else {
                s->state = BH1750_IDLE;
            }
        }
        break;
    case BH1750_REGISTER_DATA:
        if (strcmp(cmd, "DATA WRITE") == 0 || strcmp(cmd, "DATA READ") == 0) {
            uint8_t b = (n_fields > 0) ? fields[0].u8 : 0;
            if (s->is_write) {
                /* Write mode data - typically no additional data for BH1750 */
            } else {
                /* Read mode data */
                if (s->num_data == 0) s->ssd = start_sample;
                if (s->num_data < 4) {
                    s->data_bytes[s->num_data++] = b;
                }
            }
        } else if (strcmp(cmd, "START REPEAT") == 0) {
            bh1750_handle_data(di, s);
            s->ssb = start_sample;
            s->state = BH1750_ADDRESS_SLAVE;
        } else if (strcmp(cmd, "STOP") == 0) {
            bh1750_handle_data(di, s);
            s->state = BH1750_IDLE;
        }
        break;
    }
}

static void bh1750_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(bh1750_state)));
    }
    bh1750_state *s = (bh1750_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(bh1750_state));
    s->state = BH1750_IDLE;
    s->addr = BH1750_ADDR_GND;
    s->mode = BH1750_REG_MCHIGH;
    s->mtreg = BH1750_MTREG_TYP;
    s->radix = 0;
    s->params = 0;
}

static void bh1750_start(struct srd_decoder_inst *di)
{
    bh1750_state *s = (bh1750_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "bh1750");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "bh1750");

    const char *radix_str = c_opt_str(di, "radix", "Hex");
    if (radix_str && strcmp(radix_str, "Dec") == 0) s->radix = 1;
    else if (radix_str && strcmp(radix_str, "Oct") == 0) s->radix = 2;
    else if (radix_str && strcmp(radix_str, "Bin") == 0) s->radix = 3;
    else s->radix = 0;

    const char *params_str = c_opt_str(di, "params", "Typical");
    if (params_str && strcmp(params_str, "Maximal") == 0) s->params = 1;
    else if (params_str && strcmp(params_str, "Minimal") == 0) s->params = 2;
    else s->params = 0;
}

static void bh1750_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void bh1750_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder bh1750_c_decoder = {
    .id = "bh1750_c",
    .name = "BH1750(C)",
    .longname = "Digital ambient light sensor BH1750 (C)",
    .desc = "Digital 16bit Serial Output Type Ambient Light Sensor IC. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = bh1750_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = bh1750_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = bh1750_ann_rows,
    .inputs = bh1750_inputs,
    .num_inputs = 1,
    .outputs = bh1750_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = bh1750_tags,
    .num_tags = 1,
    .reset = bh1750_reset,
    .start = bh1750_start,
    .decode = bh1750_decode,
    .destroy = bh1750_destroy,
    .decode_upper = bh1750_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    bh1750_options[0].def = g_variant_new_string("Hex");
    GSList *radix_vals = NULL;
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Hex"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Dec"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Oct"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Bin"));
    bh1750_options[0].values = radix_vals;

    bh1750_options[1].def = g_variant_new_string("Typical");
    GSList *params_vals = NULL;
    params_vals = g_slist_append(params_vals, g_variant_new_string("Typical"));
    params_vals = g_slist_append(params_vals, g_variant_new_string("Maximal"));
    params_vals = g_slist_append(params_vals, g_variant_new_string("Minimal"));
    bh1750_options[1].values = params_vals;

    return &bh1750_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}