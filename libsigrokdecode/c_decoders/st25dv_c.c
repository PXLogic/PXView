/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: mit
 *
 * ST25DV NFC EEPROM protocol decoder.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation 枚举 ===== */
enum {
    ANN_SYS = 0,
    ANN_DATA,
    ANN_READ,
    ANN_WRITE,
    ANN_ERROR,
    NUM_ANN,
};

/* ===== 状态枚举 ===== */
enum st25dv_step {
    ST25DV_STEP_BEFORE_START = 0,
    ST25DV_STEP_BEFORE_ADDRESS = 1,
    ST25DV_STEP_AFTER_ADDR_ACK = 2,
    ST25DV_STEP_BEFORE_REG_MSB = 3,
    ST25DV_STEP_AFTER_REG_MSB_ACK = 4,
    ST25DV_STEP_BEFORE_REG_LSB = 5,
    ST25DV_STEP_AFTER_REG_LSB_ACK = 6,
    ST25DV_STEP_BEFORE_FIRST_DATA = 7,
    ST25DV_STEP_BEFORE_SECOND_DATA = 8,
};

/* ===== 寄存器定义 ===== */
typedef struct {
    uint16_t addr;
    const char *short_name;
    const char *long_name;
    int length;
} st25dv_register;

static const st25dv_register st25dv_regs[] = {
    {0x0000, "GPO", "GPO", 1},
    {0x0001, "ITTIME", "IT duration", 1},
    {0x0002, "EH_MODE", "Energy Harvesting", 1},
    {0x0003, "RF_MNGT", "RF management", 1},
    {0x0004, "RFZ1SS", "Area 1 security", 1},
    {0x0005, "END1", "Area 1 end address", 1},
    {0x0006, "RFZ2SS", "Area 2 security", 1},
    {0x0007, "END2", "Area 2 end address", 1},
    {0x0008, "RFZ3SS", "Area 3 security", 1},
    {0x0009, "END3", "Area 3 end address", 1},
    {0x000A, "RFZ4SS", "Area 4 security", 1},
    {0x000B, "I2CZSS", "I2C security", 1},
    {0x000C, "LOCKCCFILE", "Capability Container lock", 1},
    {0x000D, "MB_MODE", "Mailbox mode", 1},
    {0x000E, "MB_WDG", "Mailbox Watchdog", 1},
    {0x000F, "LOCKCFG", "Configuration lock", 1},
    {0x0010, "LOCKDSFID", "DSFID lock", 1},
    {0x0011, "LOCKAFI", "AFI lock", 1},
    {0x0012, "DSFID", "DSFID", 1},
    {0x0013, "AFI", "AFI", 1},
    {0x0014, "MEM_SIZE", "Memory size", 1},
    {0x0017, "ICREF", "ICref", 1},
    {0x0018, "UID", "UID", 1},
    {0x0020, "ICREV", "IC revision", 1},
    {0x0900, "I2CPASSWD", "I2C password", 17},
    {0x2000, "GPO_DYN", "GPO dynamic", 1},
    {0x2002, "EH_CTRL_DYN", "Energy Harvesting control dynamic", 1},
    {0x2003, "RF_MNGT_DYN", "RF management dynamic", 1},
    {0x2004, "I2C_SSO_DYN", "I2C secure session opened dynamic", 1},
    {0x2005, "ITSTS_DYN", "Interrupt status dynamic", 1},
    {0x2006, "MB_CTRL_DYN", "Mailbox control dynamic", 1},
    {0x2007, "MB_LEN_DYN", "Mailbox message length dynamic", 1},
    {0x2008, "MAILBOX_RAM", "Mailbox", 256},
    {0xFFFF, NULL, NULL, 0}, /* sentinel */
};

/* ===== 私有数据结构 ===== */
typedef struct {
    enum st25dv_step step;
    uint8_t address;
    uint16_t reg_address;
    const char *op;
    uint8_t data[256];
    int data_len;
    
    uint64_t reg_start_sample, reg_end_sample;
    uint64_t data_start_sample;
    int out_ann;
    int out_proto;
    int cur_idx;
    uint64_t ss;
    uint64_t es;
} st25dv_state;

#define ST25DV_DATA_ADDR   0x53  /* 0xA6 >> 1 */
#define ST25DV_SYSTEM_ADDR 0x57  /* 0xAE >> 1 */

/* ===== 静态数据 ===== */
static const char *st25dv_inputs[] = {"i2c", NULL};
static const char *st25dv_outputs[] = {"st25dv", NULL};
static const char *st25dv_tags[] = {"Embedded/industrial", NULL};

static const char *st25dv_ann_labels[][3] = {
    {"", "sys", "System"},
    {"", "data", "Data"},
    {"", "read", "Read"},
    {"", "write", "Write"},
    {"", "error", "Error"},
};

static const int st25dv_row_regs_classes[] = {ANN_SYS, ANN_DATA, ANN_READ, ANN_WRITE, ANN_ERROR, -1};

static const struct srd_c_ann_row st25dv_ann_rows[] = {
    {"regs", "Register access", st25dv_row_regs_classes, 5},
};

/* ===== 辅助函数 ===== */
static const st25dv_register *st25dv_find_reg(uint16_t addr)
{
    for (int i = 0; st25dv_regs[i].short_name != NULL; i++) {
        if (st25dv_regs[i].addr == addr)
            return &st25dv_regs[i];
    }
    return NULL;
}

static void st25dv_annotate_device_address(struct srd_decoder_inst *di,
    st25dv_state *s, uint64_t ss, uint64_t es, uint8_t addr)
{
    if (addr == ST25DV_DATA_ADDR) {
        c_put(di, ss, es, s->out_ann, ANN_DATA, "ST25DV DATA", "DATA");
    } else if (addr == ST25DV_SYSTEM_ADDR) {
        c_put(di, ss, es, s->out_ann, ANN_SYS, "ST25DV SYSTEM", "SYS");
    } else {
        c_put(di, ss, es, s->out_ann, ANN_ERROR, "ST25DV ERROR: Unknown Address", "ERROR");
    }
}

static void st25dv_annotate_register_address(struct srd_decoder_inst *di,
    st25dv_state *s)
{
    int ann_code = (strcmp(s->op, "READ") == 0) ? ANN_READ : ANN_WRITE;
    const char *op_str = (strcmp(s->op, "READ") == 0) ? "Read" : "Write";

    const st25dv_register *reg = st25dv_find_reg(s->reg_address);
    char buf[256];
    if (reg) {
        snprintf(buf, sizeof(buf), "%s: %04X: %s", op_str, s->reg_address, reg->short_name);
        c_put(di, s->reg_start_sample, s->reg_end_sample, s->out_ann, ann_code, buf);
    } else {
        snprintf(buf, sizeof(buf), "%s: %04X", op_str, s->reg_address);
        c_put(di, s->reg_start_sample, s->reg_end_sample, s->out_ann, ann_code, buf);
    }
}

static void st25dv_annotate_register_value(struct srd_decoder_inst *di,
    st25dv_state *s, uint64_t ss, uint64_t es, uint8_t byte)
{
    int ann_code = (strcmp(s->op, "READ") == 0) ? ANN_READ : ANN_WRITE;

    s->data[s->data_len++] = byte;

    const st25dv_register *reg = st25dv_find_reg(s->reg_address);
    if (!reg) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%02X", byte);
        c_put(di, ss, es, s->out_ann, ann_code, buf);
    } else if (reg->length == 1 && s->data_len == 1) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: %02X", reg->long_name, byte);
        c_put(di, ss, es, s->out_ann, ann_code, buf);
        s->data_len = 0;
    } else if (reg->length > 1 && s->data_len == reg->length) {
        char buf[512];
        int pos = snprintf(buf, sizeof(buf), "%s: ", reg->long_name);
        for (int i = 0; i < reg->length && pos < (int)sizeof(buf) - 4; i++)
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", s->data[i]);
        c_put(di, s->data_start_sample, es, s->out_ann, ann_code, buf);
        s->data_len = 0;
        s->data_start_sample = (uint64_t)-1;
    } else if (s->data_start_sample == (uint64_t)-1) {
        s->data_start_sample = ss;
    }
}

/* ===== recv_proto ===== */
static void st25dv_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    st25dv_state *s = (st25dv_state *)c_decoder_get_private(di);
    if (!s) return;
    s->ss = start_sample;
    s->es = end_sample;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "BITS") == 0) return;

    /* Annotate device address for any ADDRESS WRITE/READ */
    if (strcmp(cmd, "ADDRESS WRITE") == 0 || strcmp(cmd, "ADDRESS READ") == 0) {
        st25dv_annotate_device_address(di, s, start_sample, end_sample, databyte);
    }

    switch (s->step) {
    case ST25DV_STEP_BEFORE_START:
        if (strcmp(cmd, "START") == 0 || strcmp(cmd, "START REPEAT") == 0) {
            s->step = ST25DV_STEP_BEFORE_ADDRESS;
        } else {
            s->step = ST25DV_STEP_BEFORE_START;
        }
        break;
    case ST25DV_STEP_BEFORE_ADDRESS:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            s->address = databyte;
            s->step = ST25DV_STEP_AFTER_ADDR_ACK;
        } else {
            s->step = ST25DV_STEP_BEFORE_START;
        }
        break;
    case ST25DV_STEP_AFTER_ADDR_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->step = ST25DV_STEP_BEFORE_REG_MSB;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->step = ST25DV_STEP_BEFORE_START;
        }
        break;
    case ST25DV_STEP_BEFORE_REG_MSB:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg_start_sample = start_sample;
            s->reg_address = databyte;
            s->step = ST25DV_STEP_AFTER_REG_MSB_ACK;
        }
        break;
    case ST25DV_STEP_AFTER_REG_MSB_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->step = ST25DV_STEP_BEFORE_REG_LSB;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->step = ST25DV_STEP_BEFORE_START;
        }
        break;
    case ST25DV_STEP_BEFORE_REG_LSB:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg_end_sample = end_sample;
            s->reg_address = (s->reg_address << 8) | databyte;
            s->step = ST25DV_STEP_AFTER_REG_LSB_ACK;
        }
        break;
    case ST25DV_STEP_AFTER_REG_LSB_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->step = ST25DV_STEP_BEFORE_FIRST_DATA;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->step = ST25DV_STEP_BEFORE_START;
        }
        break;
    case ST25DV_STEP_BEFORE_FIRST_DATA:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->op = "WRITE";
            s->data_len = 0;
            s->data_start_sample = (uint64_t)-1;
            st25dv_annotate_register_address(di, s);
            st25dv_annotate_register_value(di, s, start_sample, end_sample, databyte);
            s->step = ST25DV_STEP_BEFORE_SECOND_DATA;
        } else if (strcmp(cmd, "START REPEAT") == 0) {
            s->op = "READ";
            s->data_len = 0;
            s->data_start_sample = (uint64_t)-1;
            st25dv_annotate_register_address(di, s);
            s->step = ST25DV_STEP_BEFORE_SECOND_DATA;
        }
        break;
    case ST25DV_STEP_BEFORE_SECOND_DATA:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            st25dv_annotate_register_value(di, s, start_sample, end_sample, databyte);
        } else if (strcmp(cmd, "DATA READ") == 0) {
            st25dv_annotate_register_value(di, s, start_sample, end_sample, databyte);
        } else if (strcmp(cmd, "STOP") == 0) {
            s->step = ST25DV_STEP_BEFORE_START;
        } else if (strcmp(cmd, "START REPEAT") == 0) {
            s->step = ST25DV_STEP_BEFORE_ADDRESS;
        }
        break;
    }
}

/* ===== 生命周期回调 ===== */
static void st25dv_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(st25dv_state)));
    }
    st25dv_state *s = (st25dv_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(st25dv_state));
    s->data_start_sample = (uint64_t)-1;
}

static void st25dv_start(struct srd_decoder_inst *di)
{
    st25dv_state *s = (st25dv_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "st25dv");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "st25dv");
}

static void st25dv_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void st25dv_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== 解码器结构体 ===== */
struct srd_c_decoder st25dv_c_decoder = {
    .id = "st25dv_c",
    .name = "ST25DV(C)",
    .longname = "ST25DV",
    .desc = "ST25DV NFC EEPROM",
    .license = "mit",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = st25dv_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = st25dv_ann_rows,
    .inputs = st25dv_inputs,
    .num_inputs = 1,
    .outputs = st25dv_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = st25dv_tags,
    .num_tags = 1,
    .reset = st25dv_reset,
    .start = st25dv_start,
    .decode = st25dv_decode,
    .destroy = st25dv_destroy,
    .decode_upper = st25dv_recv_proto,
    .state_size = 0,
};

/* ===== 导出函数 ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &st25dv_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}