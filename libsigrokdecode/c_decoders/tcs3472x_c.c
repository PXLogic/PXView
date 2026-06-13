/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * TCS3472x color light-to-digital converter with IR filter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation 枚举 ===== */
enum {
    ANN_REGISTER = 0,
    NUM_ANN,
};

/* ===== 状态枚举 ===== */
enum tcs3472x_state {
    TCS3472X_INITIAL,
    TCS3472X_START,
    TCS3472X_ADDR_WRITE,
    TCS3472X_ACK_ADDR_WRITE,
    TCS3472X_ACK_DATA_WRITE,
    TCS3472X_ADDR_READ,
    TCS3472X_ACK_ADDR_READ,
    TCS3472X_ACK_DATA_READ,
    TCS3472X_DATA_WRITE_CMD,
    TCS3472X_DATA_WRITE,
    TCS3472X_DATA_READ,
};

/* ===== 寄存器定义 ===== */
typedef struct {
    uint8_t addr;
    const char *name;
    int width; /* 1 or 2 bytes */
} tcs3472x_register;

static const tcs3472x_register tcs3472x_regs[] = {
    {0x00, "ENABLE", 1},
    {0x01, "ATIME", 1},
    {0x03, "WTIME", 1},
    {0x04, "AILTL", 2},
    {0x05, "AILTH", 1},
    {0x06, "AIHTL", 2},
    {0x07, "AIHTH", 1},
    {0x0C, "PERS", 1},
    {0x0D, "CONFIG", 1},
    {0x0F, "CONTROL", 1},
    {0x12, "ID", 1},
    {0x13, "STATUS", 1},
    {0x14, "CDATAL", 2},
    {0x15, "CDATAH", 1},
    {0x16, "RDATAL", 2},
    {0x17, "RDATAH", 1},
    {0x18, "GDATAL", 2},
    {0x19, "GDATAH", 1},
    {0x1A, "BDATAL", 2},
    {0x1B, "BDATAH", 1},
    {0xFF, NULL, 0}, /* sentinel */
};

/* ===== 私有数据结构 ===== */
typedef struct {
    enum tcs3472x_state state;
    int register_id;
    int auto_increment;
    uint8_t byte_values[8];
    int num_bytes;
    uint64_t sequence_start, sequence_end;
    int out_ann;
    int device_address;
} tcs3472x_state;

/* ===== 静态数据 ===== */
static const char *tcs3472x_inputs[] = {"i2c", NULL};
static const char *tcs3472x_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option tcs3472x_options[] = {
    {"device_address", "dec_tcs3472x_opt_addr", "I2C device address", NULL, NULL},
};

static const char *tcs3472x_ann_labels[][3] = {
    {"", "register", "Register"},
};

static const int tcs3472x_row_registers_classes[] = {ANN_REGISTER, -1};

static const struct srd_c_ann_row tcs3472x_ann_rows[] = {
    {"registers", "Data", tcs3472x_row_registers_classes, 1},
};

/* ===== 辅助函数 ===== */
static const tcs3472x_register *tcs3472x_find_reg(uint8_t addr)
{
    for (int i = 0; tcs3472x_regs[i].name != NULL; i++) {
        if (tcs3472x_regs[i].addr == addr)
            return &tcs3472x_regs[i];
    }
    return NULL;
}

static void tcs3472x_interpret_byte_values(struct srd_decoder_inst *di,
    tcs3472x_state *s)
{
    char buf[512];

    if (s->num_bytes == 0) {
        /* Command only, no data */
        if (s->register_id == 0x66) {
            snprintf(buf, sizeof(buf), "CLEAR channel interrupt clear");
        } else {
            const tcs3472x_register *reg = tcs3472x_find_reg(s->register_id);
            if (reg)
                snprintf(buf, sizeof(buf), "Select register 0x%02X %s", s->register_id, reg->name);
            else
                snprintf(buf, sizeof(buf), "Select register 0x%02X", s->register_id);
        }
        c_put(di, s->sequence_start, s->sequence_end, s->out_ann, ANN_REGISTER, buf);
        return;
    }

    /* Evaluate byte values */
    int value;
    if (s->num_bytes == 1) {
        value = s->byte_values[0];
    } else {
        value = (s->byte_values[1] << 8) + s->byte_values[0];
    }

    const tcs3472x_register *reg = tcs3472x_find_reg(s->register_id);
    const char *reg_name = reg ? reg->name : "UNKNOWN";

    switch (s->register_id) {
    case 0x00: { /* ENABLE */
        const char *masks[] = {"PON", "AEN", "WEN", "AIEN"};
        char opts[128] = "(";
        int pos = 1;
        for (int i = 0; i < 4; i++) {
            uint8_t mask = (1 << (i == 0 ? 0 : i == 1 ? 1 : i == 2 ? 3 : 4));
            if (!(s->byte_values[0] & mask))
                pos += snprintf(opts + pos, sizeof(opts) - pos, "~");
            pos += snprintf(opts + pos, sizeof(opts) - pos, "%s,", masks[i]);
        }
        snprintf(opts + pos, sizeof(opts) - pos, ")");
        snprintf(buf, sizeof(buf), "Register 0x%02x %s=%s", s->register_id, reg_name, opts);
        break;
    }
    case 0x0C: { /* PERS */
        int persistence = s->byte_values[0] & 0x0F;
        if (persistence > 3)
            persistence = (persistence - 3) * 5;
        char opt_buf[64];
        if (persistence == 0)
            snprintf(opt_buf, sizeof(opt_buf), "Int after each RGBC cycle");
        else
            snprintf(opt_buf, sizeof(opt_buf), "Int after %d out of range", persistence);
        snprintf(buf, sizeof(buf), "Register 0x%02x %s=%s", s->register_id, reg_name, opt_buf);
        break;
    }
    case 0x0D: { /* CONFIG */
        const char *option = (s->byte_values[0] & 0x02) ? "Wait 12x" : "Wait 1x";
        snprintf(buf, sizeof(buf), "Register 0x%02x %s=%s", s->register_id, reg_name, option);
        break;
    }
    case 0x0F: { /* CONTROL */
        int again_val = s->byte_values[0] & 0x03;
        const char *again[] = {"1x", "4x", "16x", "60x"};
        snprintf(buf, sizeof(buf), "Register 0x%02x %s AGAIN=%s",
            s->register_id, reg_name, again[again_val]);
        break;
    }
    case 0x12: { /* ID */
        const char *option;
        if (s->byte_values[0] == 0x44)
            option = "(TCS34721/TCS34725)";
        else if (s->byte_values[0] == 0x4D)
            option = "(TCS34723/TCS34727)";
        else
            option = "unknown";
        snprintf(buf, sizeof(buf), "Chip id=0x%02x %s", s->byte_values[0], option);
        break;
    }
    case 0x13: { /* STATUS */
        char opt_buf[128] = "";
        if (s->byte_values[0] & 0x10)
            strcat(opt_buf, "CLEAR valid, ");
        if (s->byte_values[0] & 0x01)
            strcat(opt_buf, "Integration complete");
        snprintf(buf, sizeof(buf), "Register 0x%02x %s=%s", s->register_id, reg_name, opt_buf);
        break;
    }
    case 0x14: case 0x15: { /* CDATA */
        snprintf(buf, sizeof(buf), "Clear Channel=%d", value);
        break;
    }
    case 0x16: case 0x17: { /* RDATA */
        snprintf(buf, sizeof(buf), "Red Channel=%d", value);
        break;
    }
    case 0x18: case 0x19: { /* GDATA */
        snprintf(buf, sizeof(buf), "Green Channel=%d", value);
        break;
    }
    case 0x1A: case 0x1B: { /* BDATA */
        snprintf(buf, sizeof(buf), "Blue Channel=%d", value);
        break;
    }
    default: {
        snprintf(buf, sizeof(buf), "Register 0x%02x %s=%d", s->register_id, reg_name, value);
        break;
    }
    }

    c_put(di, s->sequence_start, s->sequence_end, s->out_ann, ANN_REGISTER, buf);
}

/* ===== recv_proto ===== */
static void tcs3472x_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tcs3472x_state *s = (tcs3472x_state *)c_decoder_get_private(di);
    if (!s) return;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "BITS") == 0) return;

    switch (s->state) {
    case TCS3472X_INITIAL:
        /* Interpret previous byte values when returning to initial */
        if (strcmp(cmd, "START") == 0) {
            s->state = TCS3472X_START;
        }
        break;
    case TCS3472X_START:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            if (databyte != s->device_address) {
                s->state = TCS3472X_INITIAL;
                return;
            }
            s->sequence_start = start_sample;
            s->state = TCS3472X_ADDR_WRITE;
        } else if (strcmp(cmd, "ADDRESS READ") == 0) {
            if (databyte != s->device_address) {
                s->state = TCS3472X_INITIAL;
                return;
            }
            s->sequence_start = start_sample;
            s->state = TCS3472X_ADDR_READ;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    case TCS3472X_ADDR_WRITE:
        if (strcmp(cmd, "ACK") == 0)
            s->state = TCS3472X_ACK_ADDR_WRITE;
        else
            s->state = TCS3472X_INITIAL;
        break;
    case TCS3472X_ACK_ADDR_WRITE:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            /* Command byte: bit7=R/W, bit6=auto-increment, bit5-0=reg addr */
            s->auto_increment = (databyte >> 6) & 1;
            s->register_id = databyte & 0x3F;
            s->num_bytes = 0;
            s->sequence_end = end_sample;
            s->state = TCS3472X_DATA_WRITE_CMD;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    case TCS3472X_DATA_WRITE_CMD:
        if (strcmp(cmd, "ACK") == 0)
            s->state = TCS3472X_ACK_DATA_WRITE;
        else
            s->state = TCS3472X_INITIAL;
        break;
    case TCS3472X_ACK_DATA_WRITE:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->sequence_end = end_sample;
            if (s->num_bytes < (int)sizeof(s->byte_values))
                s->byte_values[s->num_bytes++] = databyte;
            s->state = TCS3472X_DATA_WRITE;
        } else if (strcmp(cmd, "STOP") == 0) {
            tcs3472x_interpret_byte_values(di, s);
            s->num_bytes = 0;
            s->state = TCS3472X_INITIAL;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    case TCS3472X_DATA_WRITE:
        if (strcmp(cmd, "ACK") == 0) {
            s->state = TCS3472X_ACK_DATA_WRITE;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    case TCS3472X_ADDR_READ:
        if (strcmp(cmd, "ACK") == 0)
            s->state = TCS3472X_ACK_ADDR_READ;
        else
            s->state = TCS3472X_INITIAL;
        break;
    case TCS3472X_ACK_ADDR_READ:
        if (strcmp(cmd, "DATA READ") == 0) {
            s->sequence_end = end_sample;
            if (s->num_bytes < (int)sizeof(s->byte_values))
                s->byte_values[s->num_bytes++] = databyte;
            s->state = TCS3472X_DATA_READ;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    case TCS3472X_DATA_READ:
        if (strcmp(cmd, "ACK") == 0) {
            s->state = TCS3472X_ACK_DATA_READ;
        } else if (strcmp(cmd, "NACK") == 0) {
            /* Read complete, wait for STOP */
        } else if (strcmp(cmd, "STOP") == 0) {
            tcs3472x_interpret_byte_values(di, s);
            s->num_bytes = 0;
            s->state = TCS3472X_INITIAL;
        } else if (strcmp(cmd, "DATA READ") == 0) {
            s->sequence_end = end_sample;
            if (s->num_bytes < (int)sizeof(s->byte_values))
                s->byte_values[s->num_bytes++] = databyte;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    case TCS3472X_ACK_DATA_READ:
        if (strcmp(cmd, "STOP") == 0) {
            tcs3472x_interpret_byte_values(di, s);
            s->num_bytes = 0;
            s->state = TCS3472X_INITIAL;
        } else if (strcmp(cmd, "DATA READ") == 0) {
            s->sequence_end = end_sample;
            if (s->num_bytes < (int)sizeof(s->byte_values))
                s->byte_values[s->num_bytes++] = databyte;
            s->state = TCS3472X_DATA_READ;
        } else {
            s->state = TCS3472X_INITIAL;
        }
        break;
    }
}

/* ===== 生命周期回调 ===== */
static void tcs3472x_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tcs3472x_state)));
    }
    tcs3472x_state *s = (tcs3472x_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tcs3472x_state));
    s->device_address = 0x29;
}

static void tcs3472x_start(struct srd_decoder_inst *di)
{
    tcs3472x_state *s = (tcs3472x_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tcs3472x");

    const char *addr_str = c_opt_str(di, "device_address", "0x29");
    s->device_address = (int)strtol(addr_str, NULL, 0);
}

static void tcs3472x_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tcs3472x_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== 解码器结构体 ===== */
struct srd_c_decoder tcs3472x_c_decoder = {
    .id = "tcs3472x_c",
    .name = "TCS3472X(C)",
    .longname = "TCS3472X",
    .desc = "Color light-to-digital converter with IR filter",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = tcs3472x_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = tcs3472x_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = tcs3472x_ann_rows,
    .inputs = tcs3472x_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = tcs3472x_tags,
    .num_tags = 1,
    .reset = tcs3472x_reset,
    .start = tcs3472x_start,
    .decode = tcs3472x_decode,
    .destroy = tcs3472x_destroy,
    .decode_upper = tcs3472x_recv_proto,
    .state_size = 0,
};

/* ===== 导出函数 ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    tcs3472x_options[0].def = g_variant_new_string("0x29");
    GSList *addr_vals = NULL;
    addr_vals = g_slist_append(addr_vals, g_variant_new_string("0x29"));
    addr_vals = g_slist_append(addr_vals, g_variant_new_string("0x39"));
    tcs3472x_options[0].values = addr_vals;

    return &tcs3472x_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}