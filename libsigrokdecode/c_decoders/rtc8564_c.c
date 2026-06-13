/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * Epson RTC-8564 JE/NB realtime clock module protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation 枚举 ===== */
enum {
    ANN_REG_0x00 = 0,
    ANN_REG_0x01,
    ANN_REG_0x02,
    ANN_REG_0x03,
    ANN_REG_0x04,
    ANN_REG_0x05,
    ANN_REG_0x06,
    ANN_REG_0x07,
    ANN_REG_0x08,
    ANN_READ,
    ANN_WRITE,
    ANN_BIT_RESERVED,
    ANN_BIT_VL,
    ANN_BIT_CENTURY,
    ANN_REG_READ,
    ANN_REG_WRITE,
    NUM_ANN,
};

/* ===== 状态枚举 ===== */
enum rtc8564_state {
    RTC8564_IDLE,
    RTC8564_GET_SLAVE_ADDR,
    RTC8564_GET_REG_ADDR,
    RTC8564_WRITE_RTC_REGS,
    RTC8564_READ_RTC_REGS,
    RTC8564_READ_RTC_REGS2,
};

/* ===== 私有数据结构 ===== */
typedef struct {
    enum rtc8564_state state;
    int reg;
    int hours, minutes, seconds;
    int days, weekdays, months, years;
    uint64_t ss, es, ss_block;
    int out_ann;
} rtc8564_state;

/* ===== 静态数据 ===== */
static const char *rtc8564_inputs[] = {"i2c", NULL};
static const char *rtc8564_tags[] = {"Clock/timing", NULL};

static const char *rtc8564_ann_labels[][3] = {
    {"", "reg-0x00", "Register 0x00"},
    {"", "reg-0x01", "Register 0x01"},
    {"", "reg-0x02", "Register 0x02"},
    {"", "reg-0x03", "Register 0x03"},
    {"", "reg-0x04", "Register 0x04"},
    {"", "reg-0x05", "Register 0x05"},
    {"", "reg-0x06", "Register 0x06"},
    {"", "reg-0x07", "Register 0x07"},
    {"", "reg-0x08", "Register 0x08"},
    {"", "read", "Read date/time"},
    {"", "write", "Write date/time"},
    {"", "bit-reserved", "Reserved bit"},
    {"", "bit-vl", "VL bit"},
    {"", "bit-century", "Century bit"},
    {"", "reg-read", "Register read"},
    {"", "reg-write", "Register write"},
};

static const int rtc8564_row_bits_classes[] = {
    ANN_REG_0x00, ANN_REG_0x01, ANN_REG_0x02, ANN_REG_0x03,
    ANN_REG_0x04, ANN_REG_0x05, ANN_REG_0x06, ANN_REG_0x07,
    ANN_REG_0x08, ANN_BIT_RESERVED, ANN_BIT_VL, ANN_BIT_CENTURY, -1
};
static const int rtc8564_row_regs_classes[] = {ANN_REG_READ, ANN_REG_WRITE, -1};
static const int rtc8564_row_datetime_classes[] = {ANN_READ, ANN_WRITE, -1};

static const struct srd_c_ann_row rtc8564_ann_rows[] = {
    {"bits", "Bits", rtc8564_row_bits_classes, 12},
    {"regs", "Register access", rtc8564_row_regs_classes, 2},
    {"date-time", "Date/time", rtc8564_row_datetime_classes, 2},
};

/* ===== 辅助函数 ===== */
static int bcd2int(uint8_t b)
{
    return ((b >> 4) & 0x0f) * 10 + (b & 0x0f);
}

static void rtc8564_handle_reg(struct srd_decoder_inst *di,
    rtc8564_state *s, uint8_t reg, uint8_t b)
{
    char buf[512];

    switch (reg) {
    case 0x00: /* Control register 1 */
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x00, "Control 1");
        break;
    case 0x01: { /* Control register 2 */
        int ti_tp = (b >> 4) & 1;
        int af = (b >> 3) & 1;
        int tf = (b >> 2) & 1;
        int aie = (b >> 1) & 1;
        int tie = b & 1;
        const char *s1 = ti_tp ? "repeated" : "single-shot";
        const char *s2 = af ? "" : "no ";
        const char *s3 = tf ? "" : "no ";
        const char *s4 = aie ? "enabled" : "prohibited";
        const char *s5 = tie ? "enabled" : "prohibited";
        snprintf(buf, sizeof(buf),
            "TI/TP=%d: %s\nAF=%d: %salarm\nTF=%d: %stimer\nAIE=%d: INT# %s\nTIE=%d: INT# %s",
            ti_tp, s1, af, s2, tf, s3, aie, s4, tie, s5);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x01, buf);
        break;
    }
    case 0x02: { /* Seconds / VL bit */
        int vl = (b >> 7) & 1;
        int sec = bcd2int(b & 0x7f);
        s->seconds = sec;
        snprintf(buf, sizeof(buf), "Voltage low: %d", vl);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_VL, buf);
        snprintf(buf, sizeof(buf), "Second: %d", sec);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x02, buf);
        break;
    }
    case 0x03: { /* Minutes */
        int min = bcd2int(b & 0x7f);
        s->minutes = min;
        snprintf(buf, sizeof(buf), "Minute: %d", min);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x03, buf);
        break;
    }
    case 0x04: { /* Hours */
        int h = bcd2int(b & 0x3f);
        s->hours = h;
        snprintf(buf, sizeof(buf), "Hour: %d", h);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x04, buf);
        break;
    }
    case 0x05: { /* Days */
        int d = bcd2int(b & 0x3f);
        s->days = d;
        snprintf(buf, sizeof(buf), "Day: %d", d);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x05, buf);
        break;
    }
    case 0x06: { /* Weekdays */
        int w = bcd2int(b & 0x07);
        s->weekdays = w;
        snprintf(buf, sizeof(buf), "Weekday: %d", w);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x06, buf);
        break;
    }
    case 0x07: { /* Months / Century bit */
        int c = (b >> 7) & 1;
        int m = bcd2int(b & 0x1f);
        s->months = m;
        snprintf(buf, sizeof(buf), "Century bit: %d", c);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_CENTURY, buf);
        snprintf(buf, sizeof(buf), "Month: %d", m);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x07, buf);
        break;
    }
    case 0x08: { /* Years */
        int y = bcd2int(b);
        s->years = y;
        snprintf(buf, sizeof(buf), "Year: %d", y);
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_0x08, buf);
        break;
    }
    default:
        break;
    }
}

/* ===== recv_proto ===== */
static void rtc8564_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    rtc8564_state *s = (rtc8564_state *)c_decoder_get_private(di);
    if (!s) return;
    s->ss = start_sample;
    s->es = end_sample;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "BITS") == 0) return;

    switch (s->state) {
    case RTC8564_IDLE:
        if (strcmp(cmd, "START") == 0) {
            s->state = RTC8564_GET_SLAVE_ADDR;
            s->ss_block = start_sample;
        }
        break;
    case RTC8564_GET_SLAVE_ADDR:
        if (strcmp(cmd, "ADDRESS WRITE") == 0)
            s->state = RTC8564_GET_REG_ADDR;
        break;
    case RTC8564_GET_REG_ADDR:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg = databyte;
            s->state = RTC8564_WRITE_RTC_REGS;
        }
        break;
    case RTC8564_WRITE_RTC_REGS:
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = RTC8564_READ_RTC_REGS;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Write register %02X: %02X", s->reg, databyte);
            c_put(di, s->ss, s->es, s->out_ann, ANN_REG_WRITE, buf);
            rtc8564_handle_reg(di, s, s->reg, databyte);
            s->reg++;
        } else if (strcmp(cmd, "STOP") == 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Write date/time: %02d.%02d.%02d %02d:%02d:%02d",
                s->days, s->months, s->years, s->hours, s->minutes, s->seconds);
            c_put(di, s->ss_block, end_sample, s->out_ann, ANN_WRITE, buf);
            s->state = RTC8564_IDLE;
        }
        break;
    case RTC8564_READ_RTC_REGS:
        if (strcmp(cmd, "ADDRESS READ") == 0)
            s->state = RTC8564_READ_RTC_REGS2;
        break;
    case RTC8564_READ_RTC_REGS2:
        if (strcmp(cmd, "DATA READ") == 0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Read register %02X: %02X", s->reg, databyte);
            c_put(di, s->ss, s->es, s->out_ann, ANN_REG_READ, buf);
            rtc8564_handle_reg(di, s, s->reg, databyte);
            s->reg++;
        } else if (strcmp(cmd, "STOP") == 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Read date/time: %02d.%02d.%02d %02d:%02d:%02d",
                s->days, s->months, s->years, s->hours, s->minutes, s->seconds);
            c_put(di, s->ss_block, end_sample, s->out_ann, ANN_READ, buf);
            s->state = RTC8564_IDLE;
        }
        break;
    }
}

/* ===== 生命周期回调 ===== */
static void rtc8564_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(rtc8564_state)));
    }
    rtc8564_state *s = (rtc8564_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(rtc8564_state));
    s->hours = -1;
    s->minutes = -1;
    s->seconds = -1;
    s->days = -1;
    s->weekdays = -1;
    s->months = -1;
    s->years = -1;
    s->ss_block = (uint64_t)-1;
}

static void rtc8564_start(struct srd_decoder_inst *di)
{
    rtc8564_state *s = (rtc8564_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "rtc8564");
}

static void rtc8564_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void rtc8564_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== 解码器结构体 ===== */
struct srd_c_decoder rtc8564_c_decoder = {
    .id = "rtc8564_c",
    .name = "RTC-8564(C)",
    .longname = "Epson RTC-8564 JE/NB",
    .desc = "Epson RTC-8564 JE/NB realtime clock module protocol.",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = rtc8564_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = rtc8564_ann_rows,
    .inputs = rtc8564_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rtc8564_tags,
    .num_tags = 1,
    .reset = rtc8564_reset,
    .start = rtc8564_start,
    .decode = rtc8564_decode,
    .destroy = rtc8564_destroy,
    .decode_upper = rtc8564_recv_proto,
    .state_size = 0,
};

/* ===== 导出函数 ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &rtc8564_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}