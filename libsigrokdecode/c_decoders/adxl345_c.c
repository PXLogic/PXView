#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_READ = 0,
    ANN_WRITE,
    ANN_MB,
    ANN_REG_ADDRESS,
    ANN_REG_DATA,
    ANN_WARNING,
    NUM_ANN,
};

enum adxl345_state {
    ADXL345_IDLE,
    ADXL345_GET_FIRST_BYTE,
    ADXL345_GET_DATA,
};

/* Rate code mapping */
static const double rate_code[] = {
    0.1, 0.2, 0.39, 0.78, 1.56, 3.13, 6.25, 12.5,
    25, 50, 100, 200, 400, 800, 1600, 3200,
};

/* FIFO modes */
static const char *fifo_modes[] = {"Bypass", "FIFO", "Stream", "Trigger"};

/* Register names */
static const char *register_names[] = {
    "DEVID",     /* 0x00 */
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    "THRESH_TAP", /* 0x1D */
    "OFSX",       /* 0x1E */
    "OFSY",       /* 0x1F */
    "OFSZ",       /* 0x20 */
    "DUR",        /* 0x21 */
    "Latent",     /* 0x22 */
    "Window",     /* 0x23 */
    "THRESH_ACT", /* 0x24 */
    "THRESH_INACT", /* 0x25 */
    "TIME_INACT",   /* 0x26 */
    "ACT_INACT_CTL",/* 0x27 */
    "THRESH_FF",    /* 0x28 */
    "TIME_FF",      /* 0x29 */
    "TAP_AXES",     /* 0x2A */
    "ACT_TAP_STATUS",/* 0x2B */
    "BW_RATE",      /* 0x2C */
    "POWER_CTL",    /* 0x2D */
    "INT_ENABLE",   /* 0x2E */
    "INT_MAP",      /* 0x2F */
    "INT_SOURCE",   /* 0x30 */
    "DATA_FORMAT",  /* 0x31 */
    "DATAX0",       /* 0x32 */
    "DATAX1",       /* 0x33 */
    "DATAY0",       /* 0x34 */
    "DATAY1",       /* 0x35 */
    "DATAZ0",       /* 0x36 */
    "DATAZ1",       /* 0x37 */
    "FIFO_CTL",     /* 0x38 */
    "FIFO_STATUS",  /* 0x39 */
};

typedef struct {
    enum adxl345_state state;
    int is_read_op;
    int is_multi;
    int address;
    int data_lo;
    int has_data_lo;
    uint64_t ss_data_lo;
    uint64_t ss, es;
    
    int out_ann;
} adxl345_state;

static const char *adxl345_inputs[] = {"spi", NULL};
static const char *adxl345_tags[] = {"IC", "Sensor", NULL};

static const char *adxl345_ann_labels[][3] = {
    {"", "read", "Read"},
    {"", "write", "Write"},
    {"", "mb", "Multiple bytes"},
    {"", "reg-address", "Register address"},
    {"", "reg-data", "Register data"},
    {"", "warning", "Warning"},
};

static const int adxl345_row_reg_classes[] = {ANN_READ, ANN_WRITE, ANN_MB, ANN_REG_ADDRESS, -1};
static const int adxl345_row_data_classes[] = {ANN_REG_DATA, ANN_WARNING, -1};

static const struct srd_c_ann_row adxl345_ann_rows[] = {
    {"reg", "Registers", adxl345_row_reg_classes, 4},
    {"data", "Data", adxl345_row_data_classes, 2},
};

static void adxl345_putx(struct srd_decoder_inst *di, adxl345_state *s, int cls, const char *text)
{
    c_put(di, s->ss, s->es, s->out_ann, cls, text);
}

static void adxl345_handle_reg_with_scaling(struct srd_decoder_inst *di, adxl345_state *s,
    uint8_t data, double factor, const char *name, const char *unit, const char *error_msg)
{
    if (data == 0 && error_msg) {
        adxl345_putx(di, s, ANN_WARNING, error_msg);
    } else {
        double result = (data * factor) / 1000.0;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: %f %s", name, result, unit);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
    }
}

static void adxl345_handle_register_data(struct srd_decoder_inst *di, adxl345_state *s, uint8_t data)
{
    int addr = s->address;
    if (addr < 0x00 || addr > 0x39) {
        return;
    }

    /* Output register name for address */
    if (addr >= 0x1D && register_names[addr]) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_ADDRESS, register_names[addr]);
    } else if (addr < 0x1D) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", addr);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_ADDRESS, buf);
        char buf2[32];
        snprintf(buf2, sizeof(buf2), "%d", data);
        adxl345_putx(di, s, ANN_REG_DATA, buf2);
        return;
    }

    switch (addr) {
    case 0x1D: /* THRESH_TAP */
        adxl345_handle_reg_with_scaling(di, s, data, 62.5, "Threshold", "g", "Undesirable behavior");
        break;
    case 0x1E: /* OFSX */
        adxl345_handle_reg_with_scaling(di, s, data, 15.6, "OFSX", "g", NULL);
        break;
    case 0x1F: /* OFSY */
        adxl345_handle_reg_with_scaling(di, s, data, 15.6, "OFSY", "g", NULL);
        break;
    case 0x20: /* OFSZ */
        adxl345_handle_reg_with_scaling(di, s, data, 15.6, "OFSZ", "g", NULL);
        break;
    case 0x21: /* DUR */
        adxl345_handle_reg_with_scaling(di, s, data, 0.625, "Duration", "s", "Disable single/double tap");
        break;
    case 0x22: /* Latent */
        adxl345_handle_reg_with_scaling(di, s, data, 1.25, "Latency", "s", "Disable double tap");
        break;
    case 0x23: /* Window */
        adxl345_handle_reg_with_scaling(di, s, data, 1.25, "Window", "s", "Disable double tap");
        break;
    case 0x24: /* THRESH_ACT */
        adxl345_handle_reg_with_scaling(di, s, data, 62.5, "Threshold", "g", "Undesirable behavior");
        break;
    case 0x25: /* THRESH_INACT */
        adxl345_handle_reg_with_scaling(di, s, data, 62.5, "Threshold", "g", "Undesirable behavior");
        break;
    case 0x26: /* TIME_INACT */
        adxl345_handle_reg_with_scaling(di, s, data, 1000, "Time", "s", "Interrupt");
        break;
    case 0x27: /* ACT_INACT_CTL */
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT: %s, ", (data & 0x80) ? "ac" : "dc");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT_X: %s, ", (data & 0x40) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT_Y: %s, ", (data & 0x20) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT_Z: %s, ", (data & 0x10) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "INACT: %s, ", (data & 0x08) ? "ac" : "dc");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "INACT_X: %s, ", (data & 0x04) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "INACT_Y: %s, ", (data & 0x02) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "INACT_Z: %s", (data & 0x01) ? "Enable" : "Disable");
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x28: /* THRESH_FF */
        adxl345_handle_reg_with_scaling(di, s, data, 62.5, "Threshold", "g", "Undesirable behavior");
        break;
    case 0x29: /* TIME_FF */
        adxl345_handle_reg_with_scaling(di, s, data, 5, "Time", "s", "Undesirable behavior");
        break;
    case 0x2A: /* TAP_AXES */
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Suppressed: %s, ", (data & 0x10) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "TAP_X: %s, ", (data & 0x04) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "TAP_Y: %s, ", (data & 0x02) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "TAP_Z: %s", (data & 0x01) ? "Enable" : "Disable");
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x2B: /* ACT_TAP_STATUS */
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT_X: %s, ", (data & 0x40) ? "Involved" : "Not involved");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT_Y: %s, ", (data & 0x20) ? "Involved" : "Not involved");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "ACT_Z: %s, ", (data & 0x10) ? "Involved" : "Not involved");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Asleep: %s, ", (data & 0x08) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "TAP_X: %s, ", (data & 0x04) ? "Involved" : "Not involved");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "TAP_Y: %s, ", (data & 0x02) ? "Involved" : "Not involved");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "TAP_Z: %s", (data & 0x01) ? "Involved" : "Not involved");
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x2C: /* BW_RATE */
    {
        int low_power = (data >> 3) & 1;
        int rate_idx = data & 0x0F;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s, Rate: %f Hz",
                 low_power ? "Reduce power" : "Normal operation",
                 (rate_idx < 16) ? rate_code[rate_idx] : 0.0);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x2D: /* POWER_CTL */
    {
        char buf[512];
        int pos = 0;
        int wakeup = data & 0x03;
        double freq = 1 << (3 - wakeup);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Link: %s, ", (data & 0x20) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "AUTO_SLEEP: %s, ", (data & 0x10) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Mode: %s, ", (data & 0x08) ? "Measurement" : "Standby");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Sleep: %s, ", (data & 0x04) ? "Sleep" : "Normal");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Wakeup: %.0f Hz", freq);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x2E: /* INT_ENABLE */
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "DATA_READY: %s, ", (data & 0x80) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "SINGLE_TAP: %s, ", (data & 0x40) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "DOUBLE_TAP: %s, ", (data & 0x20) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Activity: %s, ", (data & 0x10) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Inactivity: %s, ", (data & 0x08) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "FREE_FALL: %s, ", (data & 0x04) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Watermark: %s, ", (data & 0x02) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Overrun: %s", (data & 0x01) ? "Enable" : "Disable");
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x2F: /* INT_MAP */
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "DATA_READY: %s, ", (data & 0x80) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "SINGLE_TAP: %s, ", (data & 0x40) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "DOUBLE_TAP: %s, ", (data & 0x20) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Activity: %s, ", (data & 0x10) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Inactivity: %s, ", (data & 0x08) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "FREE_FALL: %s, ", (data & 0x04) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Watermark: %s, ", (data & 0x02) ? "INT2" : "INT1");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Overrun: %s", (data & 0x01) ? "INT2" : "INT1");
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x30: /* INT_SOURCE */
    {
        char buf[512];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "DATA_READY: %s, ", (data & 0x80) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "SINGLE_TAP: %s, ", (data & 0x40) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "DOUBLE_TAP: %s, ", (data & 0x20) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Activity: %s, ", (data & 0x10) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Inactivity: %s, ", (data & 0x08) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "FREE_FALL: %s, ", (data & 0x04) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Watermark: %s, ", (data & 0x02) ? "Yes" : "No");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Overrun: %s", (data & 0x01) ? "Yes" : "No");
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x31: /* DATA_FORMAT */
    {
        char buf[512];
        int pos = 0;
        int range_g = (data >> 0) & 0x03;
        int result = 1 << (range_g + 1);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "SELF_TEST: %s, ", (data & 0x80) ? "Enable" : "Disable");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "SPI: %s, ", (data & 0x40) ? "3-wire" : "4-wire");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "INT: %s, ", (data & 0x20) ? "Active low" : "Active high");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Resolution: %s, ", (data & 0x08) ? "Full" : "10-bit");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Justify: %s, ", (data & 0x04) ? "MSB" : "LSB");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Range: +/- %d g", result);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x32: /* DATAX0 */
    case 0x34: /* DATAY0 */
    case 0x36: /* DATAZ0 */
        s->data_lo = data;
        s->has_data_lo = 1;
        s->ss_data_lo = s->ss;
        {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", data);
            adxl345_putx(di, s, ANN_REG_DATA, buf);
        }
        break;
    case 0x33: /* DATAX1 */
    case 0x35: /* DATAY1 */
    case 0x37: /* DATAZ1 */
    {
        const char *axis = (addr == 0x33) ? "X" : (addr == 0x35) ? "Y" : "Z";
        if (s->has_data_lo) {
            int val = (data << 8) | s->data_lo;
            char buf[64];
            snprintf(buf, sizeof(buf), "%s: 0x%04X", axis, val);
            c_put(di, s->ss_data_lo, s->es, s->out_ann, ANN_REG_DATA, buf);
            s->has_data_lo = 0;
        } else {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", data);
            adxl345_putx(di, s, ANN_REG_DATA, buf);
        }
        break;
    }
    case 0x38: /* FIFO_CTL */
    {
        int fifo_mode = (data >> 6) & 0x03;
        int samples = data & 0x1F;
        char buf[256];
        snprintf(buf, sizeof(buf), "Mode: %s, Trigger: INT%d, Samples: %d",
                 (fifo_mode < 4) ? fifo_modes[fifo_mode] : "Unknown",
                 (data & 0x20) ? 2 : 1, samples);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    case 0x39: /* FIFO_STATUS */
    {
        int entries = data & 0x3F;
        int triggered = (data >> 7) & 1;
        char buf[256];
        snprintf(buf, sizeof(buf), "Triggered: %s, Entries: %d",
                 triggered ? "Yes" : "No", entries);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    default:
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", data);
        adxl345_putx(di, s, ANN_REG_DATA, buf);
        break;
    }
    }
}

static void adxl345_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    adxl345_state *s = (adxl345_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        uint8_t new_cs = (n_fields > 1) ? fields[1].u8 : 0;
        if (new_cs == 0) {
            /* CS asserted (active low) - start new transfer */
            s->state = ADXL345_GET_FIRST_BYTE;
            s->has_data_lo = 0;
        } else {
            /* CS deasserted - end transfer */
            s->state = ADXL345_IDLE;
        }
        return;
    }

    if (strcmp(cmd, "BITS") == 0)
        return;

    if (strcmp(cmd, "DATA") == 0 && n_fields >= 17) {
        /* SPI DATA format: fields[0].u8=flags, data[1..8]=mosi_val(LE), data[9..16]=miso_val(LE) */
        uint8_t flags = fields[0].u8;
        int have_mosi = (flags & 1) ? 1 : 0;
        int have_miso = (flags & 2) ? 1 : 0;
        
        
        uint64_t mosi_val = 0, miso_val = 0;
        for (int i = 0; i < 8; i++) {
            mosi_val |= (uint64_t)fields[1 + i].u8 << (8 * i);
            miso_val |= (uint64_t)fields[9 + i].u8 << (8 * i);
        }

        switch (s->state) {
        case ADXL345_GET_FIRST_BYTE:
        {
            /* First byte is address byte (MOSI direction) */
            if (have_mosi) {
                uint8_t reg_byte = (uint8_t)mosi_val;
                s->is_read_op = (reg_byte >> 7) & 1;
                s->is_multi = (reg_byte >> 6) & 1;
                s->address = reg_byte & 0x3F;
                /* Output R/W and MB annotations */
                c_put(di, s->ss, s->es, s->out_ann,
                    s->is_read_op ? ANN_READ : ANN_WRITE,
                    s->is_read_op ? "READ REG" : "WRITE REG");
                c_put(di, s->ss, s->es, s->out_ann, ANN_MB,
                    s->is_multi ? "MULTIPLE BYTES" : "SINGLE BYTE");
                s->state = ADXL345_GET_DATA;
            }
            break;
        }
        case ADXL345_GET_DATA:
            if (s->is_read_op && have_miso) {
                adxl345_handle_register_data(di, s, (uint8_t)miso_val);
                if (s->is_multi) s->address++;
            } else if (!s->is_read_op && have_mosi) {
                adxl345_handle_register_data(di, s, (uint8_t)mosi_val);
                if (s->is_multi) s->address++;
            }
            break;
        default:
            break;
        }
    }
}

static void adxl345_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(adxl345_state)));
    }
    adxl345_state *s = (adxl345_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(adxl345_state));
    s->state = ADXL345_IDLE;
}

static void adxl345_start(struct srd_decoder_inst *di)
{
    adxl345_state *s = (adxl345_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "adxl345");
}

static void adxl345_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void adxl345_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder adxl345_c_decoder = {
    .id = "adxl345_c",
    .name = "ADXL345(C)",
    .longname = "Analog Devices ADXL345 (C)",
    .desc = "Analog Devices ADXL345 3-axis accelerometer. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = adxl345_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = adxl345_ann_rows,
    .inputs = adxl345_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = adxl345_tags,
    .num_tags = 2,
    .reset = adxl345_reset,
    .start = adxl345_start,
    .decode = adxl345_decode,
    .destroy = adxl345_destroy,
    .decode_upper = adxl345_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &adxl345_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}