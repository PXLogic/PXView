#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_REG_DATE_TIME = 0,
    ANN_REG_ALARM1,
    ANN_REG_ALARM2,
    ANN_REG_CONTROL_STATUS,
    ANN_REG_AGEING,
    ANN_REG_TEMPERATURE,
    ANN_BIT_DATE_TIME,
    ANN_BIT_ALARM1,
    ANN_BIT_ALARM2,
    ANN_BIT_CONTROL_STATUS,
    ANN_BIT_AGEING,
    ANN_BIT_TEMPERATURE,
    ANN_BIT_RESERVED,
    ANN_REG_SET,
    ANN_READ_DATETIME,
    ANN_WRITE_DATETIME,
    ANN_READ_ALARM,
    ANN_WRITE_ALARM,
    ANN_READ_TEMP,
    ANN_WARNING,
    NUM_ANN,
};

static const char* days_of_week[] = {
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
};

static const char* rates[] = { "1Hz", "1.024kHz", "4.096kHz", "8.192kHz" };

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
    int temperature1;
    int temperature2;
    uint64_t ss_block;
    uint64_t ss;
    uint64_t es;
    int dayoffset;
    char asecond[8];
    char aminute[8];
    char ahour[8];
    char adaydate[32];
    char aampm[8];
} ds3231_state;

enum {
    STATE_IDLE,
    STATE_GET_SLAVE_ADDR,
    STATE_SET_REG_ADDR,
    STATE_WRITE_RTC_REGS,
    STATE_READ_RTC_REGS,
};

#define DS3231_I2C_ADDRESS 0x68

static struct srd_decoder_option ds3231_options[] = {
    { "day0", NULL, "First day of week", NULL, NULL },
};

static const char* ds3231_inputs[] = { "i2c", NULL };
static const char* ds3231_outputs[] = { NULL };
static const char* ds3231_tags[] = { "Clock/timing", "IC", NULL };

static const char* ds3231_ann_labels[][3] = {
    { "", "reg-date-time", "Date/Time register" },
    { "", "reg-alarm1", "Alarm1 register" },
    { "", "reg-alarm2", "Alarm2 register" },
    { "", "reg-control-status", "Control/Status register" },
    { "", "reg-ageing", "Ageing register" },
    { "", "reg-temperature", "Temperature register" },
    { "", "bit-date-time", "Date/Time bit" },
    { "", "bit-alarm1", "Alarm1 bit" },
    { "", "bit-alarm2", "Alarm2 bit" },
    { "", "bit-control-status", "Control/Status bit" },
    { "", "bit-ageing", "Ageing bit" },
    { "", "bit-temperature", "Temperature bit" },
    { "", "bit-reserved", "Reserved bit" },
    { "", "reg-set", "Set register" },
    { "", "read-datetime", "Read date/time" },
    { "", "write-datetime", "Write date/time" },
    { "", "read-alarm", "Read alarm" },
    { "", "write-alarm", "Write alarm" },
    { "", "read-temperature", "Read temperature" },
    { "", "warning", "Warning" },
};

static const int ds3231_row_regs_classes[] = {
    ANN_REG_DATE_TIME, ANN_REG_ALARM1, ANN_REG_ALARM2, ANN_REG_CONTROL_STATUS,
    ANN_REG_AGEING, ANN_REG_TEMPERATURE, ANN_REG_SET, -1
};
static const int ds3231_row_bits_classes[] = {
    ANN_BIT_DATE_TIME, ANN_BIT_ALARM1, ANN_BIT_ALARM2, ANN_BIT_CONTROL_STATUS,
    ANN_BIT_AGEING, ANN_BIT_TEMPERATURE, ANN_BIT_RESERVED, -1
};
static const int ds3231_row_datetime_classes[] = {
    ANN_READ_DATETIME, ANN_WRITE_DATETIME, ANN_READ_ALARM,
    ANN_WRITE_ALARM, ANN_READ_TEMP, -1
};
static const int ds3231_row_warnings_classes[] = { ANN_WARNING, -1 };

static const struct srd_c_ann_row ds3231_ann_rows[] = {
    { "regs", "Registers", ds3231_row_regs_classes, 7 },
    { "bits", "Bits", ds3231_row_bits_classes, 7 },
    { "date-time", "Date/time/temperature", ds3231_row_datetime_classes, 5 },
    { "warnings", "Warnings", ds3231_row_warnings_classes, 1 },
};

static const char* ds3231_ordinal(int n)
{
    n = n % 10;
    if (n == 1)
        return "st";
    if (n == 2)
        return "nd";
    if (n == 3)
        return "rd";
    return "th";
}

static void ds3231_output_datetime(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, const char* rw)
{
    if (s->ss_block == (uint64_t)-1 || s->days < 1)
        return;
    const char* ws = days_of_week[(s->days + s->dayoffset) % 7];
    char t[128], t2[256];
    snprintf(t, sizeof(t), "%s, %02d.%02d.%4d %02d:%02d:%02d",
        ws, s->date, s->months, s->years, s->hours, s->minutes, s->seconds);
    snprintf(t2, sizeof(t2), "%s date/time: %s", rw, t);
    c_put(di, s->ss_block, s->ss, s->out_ann, cls, t2, t);
}

static void ds3231_output_temperature(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, int integer)
{
    if (s->ss_block == (uint64_t)-1)
        return;
    char t[64];
    if (integer) {
        snprintf(t, sizeof(t), "Read temperature: %d \xc2\xb0"
                               "C",
            s->temperature1);
    } else {
        double temp = s->temperature1 + s->temperature2 / 256.0;
        snprintf(t, sizeof(t), "Read temperature: %.2f \xc2\xb0"
                               "C",
            temp);
    }
    c_put(di, s->ss_block, s->ss, s->out_ann, cls, t);
}

static void ds3231_output_alarm(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, const char* rw, int alm)
{
    if (s->ss_block == (uint64_t)-1)
        return;
    char s_buf[96];
    const char* sec_part = (alm == 2) ? "00" : s->asecond;
    snprintf(s_buf, sizeof(s_buf), "%s %s:%s:%s%s",
        s->adaydate, s->ahour, s->aminute, sec_part, s->aampm);
    if (strcmp(s_buf, "* **:**:**") == 0)
        strcat(s_buf, " (every second)");
    char t[128], t2[128];
    snprintf(t, sizeof(t), "%s Alarm %d: %s", rw, alm, s_buf);
    snprintf(t2, sizeof(t2), "A%d: %s", alm, s_buf);
    c_put(di, s->ss_block, s->ss, s->out_ann, cls, t, t2);
}

static void ds3231_handle_alarm_seconds(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, uint8_t b, uint64_t ss, uint64_t es)
{
    int sec = bcd2int(b & 0x7f);
    const char* mm;
    if (b & (1 << 7)) {
        mm = "Ignore second";
        strcpy(s->asecond, "**");
    } else {
        mm = "Alarm on second match";
        snprintf(s->asecond, sizeof(s->asecond), "%02d", sec);
    }
    c_put(di, ss, es, s->out_ann, cls, mm);
    char t[32];
    snprintf(t, sizeof(t), "Seconds=%d", sec);
    c_put(di, ss, es, s->out_ann, cls, t);
}

static void ds3231_handle_alarm_minutes(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, uint8_t b, uint64_t ss, uint64_t es)
{
    int ignore = b & (1 << 7);
    const char* mm = ignore ? "Ignore minute" : "Alarm on minute match";
    c_put(di, ss, es, s->out_ann, cls, mm);
    int m = bcd2int(b & 0x7f);
    char t[32];
    snprintf(t, sizeof(t), "Minutes=%d", m);
    c_put(di, ss, es, s->out_ann, cls, t);
    snprintf(s->aminute, sizeof(s->aminute), ignore ? "*" : "%02d", m);
}

static void ds3231_handle_alarm_hour(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, uint8_t b, uint64_t ss, uint64_t es)
{
    int ignore = b & (1 << 7);
    const char* mm = ignore ? "Ignore hour" : "Alarm on hour match";
    c_put(di, ss, es, s->out_ann, cls, mm);
    int ampm_mode = (b & (1 << 6)) ? 1 : 0;
    if (ampm_mode) {
        const char* ampm = (b & (1 << 5)) ? "pm" : "am";
        c_put(di, ss, es, s->out_ann, cls, ampm, ampm);
        int h = bcd2int(b & 0x1f);
        char t[32];
        snprintf(t, sizeof(t), "Hour=%d%s", h, ampm);
        c_put(di, ss, es, s->out_ann, cls, t);
        if (!ignore) {
            strcpy(s->aampm, ampm);
            snprintf(s->ahour, sizeof(s->ahour), "%02d", h);
        }
    } else {
        int h = bcd2int(b & 0x3f);
        char t[32];
        snprintf(t, sizeof(t), "Hour=%d", h);
        c_put(di, ss, es, s->out_ann, cls, t);
        s->aampm[0] = '\0';
        if (!ignore)
            snprintf(s->ahour, sizeof(s->ahour), "%02d", h);
    }
}

static void ds3231_handle_alarm_daydate(struct srd_decoder_inst* di, ds3231_state* s,
    int cls, uint8_t b, uint64_t ss, uint64_t es)
{
    int ignore = b & (1 << 7);
    int dydt = b & (1 << 6);
    c_put(di, ss, es, s->out_ann, ANN_BIT_ALARM2,
        dydt ? "Day Mode" : "Date Mode",
        dydt ? "Day" : "Date");
    if (dydt) {
        const char* mm = ignore ? "Ignore day" : "Alarm on day match";
        c_put(di, ss, es, s->out_ann, cls, mm);
        const char* ws = days_of_week[((b & 0x3f) + s->dayoffset) % 7];
        char t[32];
        snprintf(t, sizeof(t), "Day=%s", ws);
        c_put(di, ss, es, s->out_ann, cls, t);
        snprintf(s->adaydate, sizeof(s->adaydate), ignore ? "*" : "%s", ws);
    } else {
        const char* mm = ignore ? "Ignore date" : "Alarm on date match";
        c_put(di, ss, es, s->out_ann, cls, mm);
        int d = bcd2int(b & 0x3f);
        char t[32];
        snprintf(t, sizeof(t), "Date=%d%s of month", d, ds3231_ordinal(d));
        c_put(di, ss, es, s->out_ann, cls, t);
        snprintf(s->adaydate, sizeof(s->adaydate),
            ignore ? "*" : "%d%s of month", d, ds3231_ordinal(d));
    }
}

static void ds3231_handle_reg(struct srd_decoder_inst* di, ds3231_state* s,
    uint8_t b, uint64_t ss, uint64_t es)
{
    if (s->reg < 0x13) {
        switch (s->reg) {
        case 0x00: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Seconds", "Sec", "S");
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            s->seconds = bcd2int(b & 0x7f);
            char t[32];
            snprintf(t, sizeof(t), "Second: %d", s->seconds);
            c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            break;
        }
        case 0x01: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Minutes", "Min", "M");
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            s->minutes = bcd2int(b & 0x7f);
            char t[32];
            snprintf(t, sizeof(t), "Minute: %d", s->minutes);
            c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            break;
        }
        case 0x02: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Hours", "H");
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            int ampm_mode = (b & (1 << 6)) ? 1 : 0;
            if (ampm_mode) {
                c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, "12-hour mode", "12h mode", "12h");
                const char* a = (b & (1 << 5)) ? "PM" : "AM";
                c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, a);
                s->hours = bcd2int(b & 0x1f);
                char t[32];
                snprintf(t, sizeof(t), "Hour: %d", s->hours);
                c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            } else {
                c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, "24-hour mode", "24h mode", "24h");
                s->hours = bcd2int(b & 0x3f);
                char t[32];
                snprintf(t, sizeof(t), "Hour: %d", s->hours);
                c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            }
            break;
        }
        case 0x03: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Day of week", "Day", "D");
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            s->days = bcd2int(b & 0x07);
            if (s->days >= 1 && s->days <= 7) {
                const char* ws = days_of_week[(s->days + s->dayoffset) % 7];
                char t[64];
                snprintf(t, sizeof(t), "Weekday: %s", ws);
                c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            }
            break;
        }
        case 0x04: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Date", "D");
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            s->date = bcd2int(b & 0x3f);
            char t[32];
            snprintf(t, sizeof(t), "Date: %d%s", s->date, ds3231_ordinal(s->date));
            c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            break;
        }
        case 0x05: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Month", "Mon", "M");
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            s->months = bcd2int(b & 0x1f);
            char t[32];
            snprintf(t, sizeof(t), "Month: %d", s->months);
            c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            break;
        }
        case 0x06: {
            c_put(di, ss, es, s->out_ann, ANN_REG_DATE_TIME, "Year", "Y");
            s->years = bcd2int(b & 0xff);
            s->years += 2000;
            char t[32];
            snprintf(t, sizeof(t), "Year: %d", s->years);
            c_put(di, ss, es, s->out_ann, ANN_BIT_DATE_TIME, t);
            break;
        }
        case 0x07: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM1, "Alarm 1 Seconds", "A1Secs");
            ds3231_handle_alarm_seconds(di, s, ANN_BIT_ALARM1, b, ss, es);
            break;
        }
        case 0x08: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM1, "Alarm 1 Minutes", "A1Mins");
            ds3231_handle_alarm_minutes(di, s, ANN_BIT_ALARM1, b, ss, es);
            break;
        }
        case 0x09: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM1, "Alarm 1 Hour", "A1Hour", "A1Hr");
            ds3231_handle_alarm_hour(di, s, ANN_BIT_ALARM1, b, ss, es);
            break;
        }
        case 0x0a: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM1, "Alarm 1 Day/Date", "A1Day");
            ds3231_handle_alarm_daydate(di, s, ANN_BIT_ALARM1, b, ss, es);
            break;
        }
        case 0x0b: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM2, "Alarm 2 Minutes", "A2Mins");
            ds3231_handle_alarm_minutes(di, s, ANN_BIT_ALARM2, b, ss, es);
            break;
        }
        case 0x0c: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM2, "Alarm 2 Hour", "A2Hour", "A2Hr");
            ds3231_handle_alarm_hour(di, s, ANN_BIT_ALARM2, b, ss, es);
            break;
        }
        case 0x0d: {
            c_put(di, ss, es, s->out_ann, ANN_REG_ALARM2, "Alarm 2 Day/Date", "A2Day");
            ds3231_handle_alarm_daydate(di, s, ANN_BIT_ALARM2, b, ss, es);
            break;
        }
        case 0x0e: {
            c_put(di, ss, es, s->out_ann, ANN_REG_CONTROL_STATUS, "Control Register", "Ctrl", "C");
            const char* eosc = (b & (1 << 7)) ? "OFF" : "ON";
            char t[64];
            snprintf(t, sizeof(t), "Oscillator %s", eosc);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* bbsqw = (b & (1 << 6)) ? "ON" : "OFF";
            snprintf(t, sizeof(t), "Battery Backed Square Wave %s", bbsqw);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* conv = (b & (1 << 5)) ? "ON" : "OFF";
            snprintf(t, sizeof(t), "Convert Temperature %s", conv);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* rs = rates[(b >> 3) & 3];
            snprintf(t, sizeof(t), "Rate Select: %s", rs);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* intcn = (b & (1 << 2)) ? "ON" : "OFF";
            snprintf(t, sizeof(t), "Interrupt Control %s", intcn);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* a2ie = (b & (1 << 1)) ? "ENABLED" : "DISABLED";
            snprintf(t, sizeof(t), "Alarm 2 Interrupt %s", a2ie);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* a1ie = (b & (1 << 0)) ? "ENABLED" : "DISABLED";
            snprintf(t, sizeof(t), "Alarm 1 Interrupt %s", a1ie);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            break;
        }
        case 0x0f: {
            c_put(di, ss, es, s->out_ann, ANN_REG_CONTROL_STATUS, "Control/Status Register", "Stat", "S");
            const char* osf = (b & (1 << 7)) ? "STOPPED" : "RUNNING";
            char t[64];
            snprintf(t, sizeof(t), "Oscillator %s", osf);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            const char* en32khz = (b & (1 << 3)) ? "ENABLED" : "DISABLED";
            snprintf(t, sizeof(t), "32kHz Output %s", en32khz);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* bsy = (b & (1 << 2)) ? "ON" : "OFF";
            snprintf(t, sizeof(t), "Busy %s", bsy);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* a2f = (b & (1 << 1)) ? "ELAPSED" : "CLEAR";
            snprintf(t, sizeof(t), "Alarm 2 %s", a2f);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            const char* a1f = (b & (1 << 0)) ? "ELAPSED" : "CLEAR";
            snprintf(t, sizeof(t), "Alarm 1 %s", a1f);
            c_put(di, ss, es, s->out_ann, ANN_BIT_CONTROL_STATUS, t);
            break;
        }
        case 0x10: {
            c_put(di, ss, es, s->out_ann, ANN_REG_AGEING, "Ageing Register", "Ageing", "A");
            int offset = (b & (1 << 7)) ? -((b ^ 0xff) + 1) : b;
            char t[32];
            snprintf(t, sizeof(t), "Offset=%d", offset);
            c_put(di, ss, es, s->out_ann, ANN_BIT_AGEING, t);
            break;
        }
        case 0x11: {
            c_put(di, ss, es, s->out_ann, ANN_REG_TEMPERATURE, "Temperature Register 1", "Temp1", "T");
            const char* sign;
            if (b & (1 << 7)) {
                sign = "-";
                s->temperature1 = -((b ^ 0xff) + 1);
            } else {
                sign = "+";
                s->temperature1 = b;
            }
            char t[32];
            snprintf(t, sizeof(t), "Sign: %s", sign);
            c_put(di, ss, es, s->out_ann, ANN_BIT_TEMPERATURE, t);
            snprintf(t, sizeof(t), "Temperature: %d", b & 0x7f);
            c_put(di, ss, es, s->out_ann, ANN_BIT_TEMPERATURE, t);
            break;
        }
        case 0x12: {
            c_put(di, ss, es, s->out_ann, ANN_REG_TEMPERATURE, "Temperature Register 2", "Temp2", "T");
            s->temperature2 = b;
            char t[32];
            snprintf(t, sizeof(t), "Temperature fraction: %.2f", b / 256.0);
            c_put(di, ss, es, s->out_ann, ANN_BIT_TEMPERATURE, t);
            c_put(di, ss, es, s->out_ann, ANN_BIT_RESERVED, "Reserved", "Rsvd", "R");
            break;
        }
        }
    }

    s->reg++;
    if (s->reg > 0x12)
        s->reg = 0;
}

static void ds3231_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ds3231_state* s = (ds3231_state*)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "STOP") == 0) {
        s->state = STATE_IDLE;
        s->ss_block = (uint64_t)-1;
        return;
    }

    if (strcmp(cmd, "START") == 0 || strcmp(cmd, "START REPEAT") == 0) {
        s->state = STATE_GET_SLAVE_ADDR;
        return;
    }

    if (s->state == STATE_GET_SLAVE_ADDR) {
        if (databyte != DS3231_I2C_ADDRESS) {
            /* Match Python: silently ignore non-DS3231 addresses */
            s->state = STATE_IDLE;
            return;
        }
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            s->state = STATE_SET_REG_ADDR;
        } else if (strcmp(cmd, "ADDRESS READ") == 0) {
            s->state = STATE_READ_RTC_REGS;
        }
    } else if (s->state == STATE_SET_REG_ADDR) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg = databyte;
            char t[32];
            snprintf(t, sizeof(t), "Select register %d", s->reg);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_REG_SET, t);
            s->state = STATE_WRITE_RTC_REGS;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->state = STATE_IDLE;
        }
    } else if (s->state == STATE_WRITE_RTC_REGS) {
        if (strcmp(cmd, "DATA WRITE") == 0) {
            if (s->reg == 0 || s->reg == 7 || s->reg == 11)
                s->ss_block = start_sample;
            ds3231_handle_reg(di, s, databyte, start_sample, end_sample);
        } else if (strcmp(cmd, "ACK") == 0) {
            if (s->ss_block != (uint64_t)-1) {
                if (s->reg == 7)
                    ds3231_output_datetime(di, s, ANN_WRITE_DATETIME, "Written");
                else if (s->reg == 11)
                    ds3231_output_alarm(di, s, ANN_WRITE_ALARM, "Written", 1);
                else if (s->reg == 14)
                    ds3231_output_alarm(di, s, ANN_WRITE_ALARM, "Written", 2);
            }
        }
    } else if (s->state == STATE_READ_RTC_REGS) {
        if (strcmp(cmd, "DATA READ") == 0) {
            if (s->reg == 0 || s->reg == 7 || s->reg == 11 || s->reg == 17)
                s->ss_block = start_sample;
            ds3231_handle_reg(di, s, databyte, start_sample, end_sample);
        } else if (strcmp(cmd, "NACK") == 0) {
            if (s->ss_block != (uint64_t)-1) {
                if (s->reg == 7)
                    ds3231_output_datetime(di, s, ANN_READ_DATETIME, "Read");
                else if (s->reg == 11)
                    ds3231_output_alarm(di, s, ANN_READ_ALARM, "Read", 1);
                else if (s->reg == 14)
                    ds3231_output_alarm(di, s, ANN_READ_ALARM, "Read", 2);
                else if (s->reg == 18)
                    ds3231_output_temperature(di, s, ANN_READ_TEMP, 1);
                else if (s->reg == 0)
                    ds3231_output_temperature(di, s, ANN_READ_TEMP, 0);
            }
        }
    }
}

static void ds3231_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ds3231_state)));
    }
    ds3231_state* s = (ds3231_state*)c_decoder_get_private(di);
    memset(s, 0, sizeof(ds3231_state));
    s->hours = -1;
    s->minutes = -1;
    s->seconds = -1;
    s->days = -1;
    s->date = -1;
    s->months = -1;
    s->years = -1;
    s->ss_block = (uint64_t)-1;
    s->asecond[0] = '\0';
    s->aminute[0] = '\0';
    s->ahour[0] = '\0';
    s->adaydate[0] = '\0';
    s->aampm[0] = '\0';
}

static void ds3231_start(struct srd_decoder_inst* di)
{
    ds3231_state* s = (ds3231_state*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ds3231");
    const char* day0 = c_opt_str(di, "day0", "Sunday");
    s->dayoffset = 0;
    for (int i = 0; i < 7; i++) {
        if (strcmp(day0, days_of_week[i]) == 0) {
            s->dayoffset = i;
            break;
        }
    }
}

static void ds3231_decode(struct srd_decoder_inst* di)
{
    (void)di;
}

static void ds3231_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ds3231_c_decoder = {
    .id = "ds3231_c",
    .name = "Ds3231(C)",
    .longname = "Maxim DS3231 (C)",
    .desc = "Maxim DS3231 realtime clock module protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ds3231_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ds3231_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ds3231_ann_rows,
    .inputs = ds3231_inputs,
    .num_inputs = 1,
    .outputs = ds3231_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ds3231_tags,
    .num_tags = 2,
    .reset = ds3231_reset,
    .start = ds3231_start,
    .decode = ds3231_decode,
    .destroy = ds3231_destroy,
    .decode_upper = ds3231_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    ds3231_options[0].def = g_variant_new_string("Sunday");
    return &ds3231_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}