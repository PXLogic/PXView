#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_REG_SECONDS = 0,
    ANN_REG_MINUTES,
    ANN_REG_HOURS,
    ANN_REG_DAY,
    ANN_REG_DATE,
    ANN_REG_MONTH,
    ANN_REG_YEAR,
    ANN_REG_CONTROL,
    ANN_REG_RAM,
    ANN_BIT_CLOCK_HALT,
    ANN_BIT_SECONDS,
    ANN_BIT_RESERVED,
    ANN_BIT_MINUTES,
    ANN_BIT_12_24_HOURS,
    ANN_BIT_AM_PM,
    ANN_BIT_HOURS,
    ANN_BIT_DAY,
    ANN_BIT_DATE,
    ANN_BIT_MONTH,
    ANN_BIT_YEAR,
    ANN_BIT_OUT,
    ANN_BIT_SQWE,
    ANN_BIT_RS,
    ANN_BIT_RAM,
    ANN_READ_DATE_TIME,
    ANN_WRITE_DATE_TIME,
    ANN_READ_REG,
    ANN_WRITE_REG,
    ANN_WARNING,
    NUM_ANN,
};

static const char *days_of_week[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday",
};

static int bcd2int(unsigned char b)
{
    return ((b >> 4) & 0x0f) * 10 + (b & 0x0f);
}

typedef struct {
    int out_ann;
    int state;
    int hours;
    int minutes;
    int seconds;
    int days;
    int date;
    int months;
    int years;
    int reg;
    uint64_t ss_block;
    uint64_t ss;
    uint64_t es;
} ds1307_state;

enum {
    STATE_IDLE,
    STATE_GET_SLAVE_ADDR,
    STATE_GET_REG_ADDR,
    STATE_WRITE_RTC_REGS,
    STATE_READ_RTC_REGS,
    STATE_READ_RTC_REGS2,
};

#define DS1307_I2C_ADDRESS 0x68

static const char *ds1307_inputs[] = {"i2c", NULL};
static const char *ds1307_outputs[] = {NULL};
static const char *ds1307_tags[] = {"Clock/timing", "IC", NULL};

static const char *ds1307_ann_labels[][3] = {
    {"", "reg_seconds", "Seconds register"},
    {"", "reg_minutes", "Minutes register"},
    {"", "reg_hours", "Hours register"},
    {"", "reg_day", "Day register"},
    {"", "reg_date", "Date register"},
    {"", "reg_month", "Month register"},
    {"", "reg_year", "Year register"},
    {"", "reg_control", "Control register"},
    {"", "reg_ram", "RAM register"},
    {"", "bit_clock_halt", "Clock halt bit"},
    {"", "bit_seconds", "Seconds bit"},
    {"", "bit_reserved", "Reserved bit"},
    {"", "bit_minutes", "Minutes bit"},
    {"", "bit_12_24_hours", "12/24 hours bit"},
    {"", "bit_am_pm", "AM/PM bit"},
    {"", "bit_hours", "Hours bit"},
    {"", "bit_day", "Day bit"},
    {"", "bit_date", "Date bit"},
    {"", "bit_month", "Month bit"},
    {"", "bit_year", "Year bit"},
    {"", "bit_out", "OUT bit"},
    {"", "bit_sqwe", "SQWE bit"},
    {"", "bit_rs", "RS bit"},
    {"", "bit_ram", "RAM bit"},
    {"", "read_date_time", "Read date/time"},
    {"", "write_date_time", "Write date/time"},
    {"", "read_reg", "Register read"},
    {"", "write_reg", "Register write"},
    {"", "warning", "Warning"},
};

static const int ds1307_row_bits_classes[] = {
    ANN_BIT_CLOCK_HALT, ANN_BIT_SECONDS, ANN_BIT_RESERVED, ANN_BIT_MINUTES,
    ANN_BIT_12_24_HOURS, ANN_BIT_AM_PM, ANN_BIT_HOURS, ANN_BIT_DAY,
    ANN_BIT_DATE, ANN_BIT_MONTH, ANN_BIT_YEAR, ANN_BIT_OUT, ANN_BIT_SQWE,
    ANN_BIT_RS, ANN_BIT_RAM, -1
};
static const int ds1307_row_regs_classes[] = {
    ANN_REG_SECONDS, ANN_REG_MINUTES, ANN_REG_HOURS, ANN_REG_DAY,
    ANN_REG_DATE, ANN_REG_MONTH, ANN_REG_YEAR, ANN_REG_CONTROL, ANN_REG_RAM, -1
};
static const int ds1307_row_datetime_classes[] = {
    ANN_READ_DATE_TIME, ANN_WRITE_DATE_TIME, ANN_READ_REG, ANN_WRITE_REG, -1
};
static const int ds1307_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row ds1307_ann_rows[] = {
    {"bits", "Bits", ds1307_row_bits_classes, 15},
    {"regs", "Registers", ds1307_row_regs_classes, 9},
    {"date_time", "Date/time", ds1307_row_datetime_classes, 4},
    {"warnings", "Warnings", ds1307_row_warnings_classes, 1},
};

static void ds1307_output_datetime(struct srd_decoder_inst *di, ds1307_state *s,
                                    int cls, const char *rw)
{
    (void)rw;
    if (s->days < 1 || s->date < 0 || s->months < 0 || s->years < 0 ||
        s->hours < 0 || s->minutes < 0 || s->seconds < 0)
        return;
    const char *ws = days_of_week[(s->days - 1) % 7];
    char t[128];
    snprintf(t, sizeof(t), "%s date/time: %s, %02d.%02d.%4d %02d:%02d:%02d",
             rw, ws, s->date, s->months, s->years, s->hours, s->minutes, s->seconds);
    c_put(di, s->ss_block, s->es, s->out_ann, cls, t);
}

static void ds1307_handle_reg(struct srd_decoder_inst *di, ds1307_state *s,
                               uint8_t b, uint64_t ss, uint64_t es)
{
    int r = (s->reg < 8) ? s->reg : 0x3f;

    switch (r) {
    case 0x00: {
        c_put(di, ss, es, s->out_ann, ANN_REG_SECONDS, "Seconds", "Sec", "S");
        int ch = (b & (1 << 7)) ? 1 : 0;
        char t[32];
        snprintf(t, sizeof(t), "Clock halt: %d", ch);
        c_put(di, ss, es, s->out_ann, ANN_BIT_CLOCK_HALT, t);
        s->seconds = bcd2int(b & 0x7f);
        snprintf(t, sizeof(t), "Second: %d", s->seconds);
        c_put(di, ss, es, s->out_ann, ANN_BIT_SECONDS, t);
        break;
    }
    case 0x01: {
        c_put(di, ss, es, s->out_ann, ANN_REG_MINUTES, "Minutes", "Min", "M");
        c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
        s->minutes = bcd2int(b & 0x7f);
        char t[32];
        snprintf(t, sizeof(t), "Minute: %d", s->minutes);
        c_put(di, ss, es, s->out_ann, ANN_BIT_MINUTES, t);
        break;
    }
    case 0x02: {
        c_put(di, ss, es, s->out_ann, ANN_REG_HOURS, "Hours", "H");
        c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
        int ampm_mode = (b & (1 << 6)) ? 1 : 0;
        if (ampm_mode) {
            c_put(di, ss, es, s->out_ann, ANN_BIT_12_24_HOURS, "12-hour mode", "12h mode", "12h");
            const char *a = (b & (1 << 5)) ? "PM" : "AM";
            c_put(di, ss, es, s->out_ann, ANN_BIT_AM_PM, a);
            s->hours = bcd2int(b & 0x1f);
            char t[32];
            snprintf(t, sizeof(t), "Hour: %d", s->hours);
            c_put(di, ss, es, s->out_ann, ANN_BIT_HOURS, t);
        } else {
            c_put(di, ss, es, s->out_ann, ANN_BIT_12_24_HOURS, "24-hour mode", "24h mode", "24h");
            s->hours = bcd2int(b & 0x3f);
            char t[32];
            snprintf(t, sizeof(t), "Hour: %d", s->hours);
            c_put(di, ss, es, s->out_ann, ANN_BIT_HOURS, t);
        }
        break;
    }
    case 0x03: {
        c_put(di, ss, es, s->out_ann, ANN_REG_DAY, "Day of week", "Day", "D");
        c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
        s->days = bcd2int(b & 0x07);
        if (s->days >= 1 && s->days <= 7) {
            const char *ws = days_of_week[s->days - 1];
            char t[64];
            snprintf(t, sizeof(t), "Weekday: %s", ws);
            c_put(di, ss, es, s->out_ann, ANN_BIT_DAY, t);
        }
        break;
    }
    case 0x04: {
        c_put(di, ss, es, s->out_ann, ANN_REG_DATE, "Date", "D");
        c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
        s->date = bcd2int(b & 0x3f);
        char t[32];
        snprintf(t, sizeof(t), "Date: %d", s->date);
        c_put(di, ss, es, s->out_ann, ANN_BIT_DATE, t);
        break;
    }
    case 0x05: {
        c_put(di, ss, es, s->out_ann, ANN_REG_MONTH, "Month", "Mon", "M");
        c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
        s->months = bcd2int(b & 0x1f);
        char t[32];
        snprintf(t, sizeof(t), "Month: %d", s->months);
        c_put(di, ss, es, s->out_ann, ANN_BIT_MONTH, t);
        break;
    }
    case 0x06: {
        c_put(di, ss, es, s->out_ann, ANN_REG_YEAR, "Year", "Y");
        s->years = bcd2int(b & 0xff);
        s->years += 2000;
        char t[32];
        snprintf(t, sizeof(t), "Year: %d", s->years);
        c_put(di, ss, es, s->out_ann, ANN_BIT_YEAR, t);
        break;
    }
    case 0x07: {
        c_put(di, ss, es, s->out_ann, ANN_REG_CONTROL, "Control", "Ctrl", "C");
        c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
        int o = (b & (1 << 7)) ? 1 : 0;
        int sqwe_en = (b & (1 << 4)) ? 1 : 0;
        const char *s2 = sqwe_en ? "en" : "dis";
        const char *rates[] = {"1Hz", "4096Hz", "8192Hz", "32768Hz"};
        const char *r = rates[b & 0x03];
        char t[64];
        snprintf(t, sizeof(t), "Output control: %d", o);
        c_put(di, ss, es, s->out_ann, ANN_BIT_OUT, t);
        snprintf(t, sizeof(t), "Square wave output: %sabled", s2);
        c_put(di, ss, es, s->out_ann, ANN_BIT_SQWE, t);
        snprintf(t, sizeof(t), "Square wave output rate: %s", r);
        c_put(di, ss, es, s->out_ann, ANN_BIT_RS, t);
        break;
    }
    case 0x3f: {
        c_put(di, ss, es, s->out_ann, ANN_REG_RAM, "RAM", "R");
        char t[32];
        snprintf(t, sizeof(t), "SRAM: 0x%02X", b);
        c_put(di, ss, es, s->out_ann, ANN_BIT_RAM, t);
        break;
    }
    }

    s->reg++;
    if (s->reg > 0x3f)
        s->reg = 0;
}

static void ds1307_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ds1307_state *s = (ds1307_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "START") == 0) {
        s->state = STATE_GET_SLAVE_ADDR;
        s->ss_block = start_sample;
    } else if (strcmp(cmd, "START REPEAT") == 0) {
        if (s->state == STATE_WRITE_RTC_REGS) {
            s->state = STATE_READ_RTC_REGS;
        } else {
            s->state = STATE_GET_SLAVE_ADDR;
            s->ss_block = start_sample;
        }
    } else if (s->state == STATE_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            if (databyte != DS1307_I2C_ADDRESS) {
                if (databyte != 0x7F) {
                    char t[64];
                    snprintf(t, sizeof(t), "Ignoring non-DS1307 data (slave 0x%02X)", databyte);
                    c_put(di, s->ss_block, end_sample, s->out_ann, ANN_WARNING, t);
                }
                s->state = STATE_IDLE;
                return;
            }
            s->state = STATE_GET_REG_ADDR;
        } else if (strcmp(cmd, "ADDRESS READ") == 0) {
            if (databyte != DS1307_I2C_ADDRESS) {
                if (databyte != 0x7F) {
                    char t[64];
                    snprintf(t, sizeof(t), "Ignoring non-DS1307 data (slave 0x%02X)", databyte);
                    c_put(di, s->ss_block, end_sample, s->out_ann, ANN_WARNING, t);
                }
                s->state = STATE_IDLE;
                return;
            }
            s->state = STATE_READ_RTC_REGS;
        }
    } else if (s->state == STATE_GET_REG_ADDR) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg = databyte;
            s->state = STATE_WRITE_RTC_REGS;
        }
    } else if (s->state == STATE_WRITE_RTC_REGS) {
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = STATE_READ_RTC_REGS;
            return;
        }
        if (strcmp(cmd, "DATA WRITE") == 0) {
            ds1307_handle_reg(di, s, databyte, start_sample, end_sample);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WRITE_REG,
                      "Register write");
        } else if (strcmp(cmd, "STOP") == 0) {
            ds1307_output_datetime(di, s, ANN_WRITE_DATE_TIME, "Written");
            s->state = STATE_IDLE;
        }
    } else if (s->state == STATE_READ_RTC_REGS) {
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            if (databyte != DS1307_I2C_ADDRESS) {
                if (databyte != 0x7F) {
                    char t[64];
                    snprintf(t, sizeof(t), "Ignoring non-DS1307 data (slave 0x%02X)", databyte);
                    c_put(di, s->ss_block, end_sample, s->out_ann, ANN_WARNING, t);
                }
                s->state = STATE_IDLE;
                return;
            }
            s->state = STATE_READ_RTC_REGS2;
        }
    } else if (s->state == STATE_READ_RTC_REGS2) {
        if (strcmp(cmd, "DATA READ") == 0) {
            ds1307_handle_reg(di, s, databyte, start_sample, end_sample);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_READ_REG,
                      "Register read");
        } else if (strcmp(cmd, "STOP") == 0) {
            ds1307_output_datetime(di, s, ANN_READ_DATE_TIME, "Read");
            s->state = STATE_IDLE;
        }
    }

    if (strcmp(cmd, "STOP") == 0) {
        s->state = STATE_IDLE;
    }
}

static void ds1307_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ds1307_state)));
    }
    ds1307_state *s = (ds1307_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ds1307_state));
    s->hours = -1;
    s->minutes = -1;
    s->seconds = -1;
    s->days = -1;
    s->date = -1;
    s->months = -1;
    s->years = -1;
}

static void ds1307_start(struct srd_decoder_inst *di)
{
    ds1307_state *s = (ds1307_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ds1307");
}

static void ds1307_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ds1307_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ds1307_c_decoder = {
    .id = "ds1307_c",
    .name = "Ds1307(C)",
    .longname = "Dallas DS1307 (C)",
    .desc = "Dallas DS1307 realtime clock module protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ds1307_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ds1307_ann_rows,
    .inputs = ds1307_inputs,
    .num_inputs = 1,
    .outputs = ds1307_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ds1307_tags,
    .num_tags = 2,
    .reset = ds1307_reset,
    .start = ds1307_start,
    .decode = ds1307_decode,
    .destroy = ds1307_destroy,
    .decode_upper = ds1307_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ds1307_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}