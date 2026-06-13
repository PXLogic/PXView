#include "libsigrokdecode.h"

static void fmt_binary14(uint16_t val, char *buf, int bufsize) {
    if (bufsize < 15) { buf[0] = '\0'; return; }
    for (int i = 13; i >= 0; i--) buf[13 - i] = (val & (1 << i)) ? '1' : '0';
    buf[14] = '\0';
}
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum dcf77_ann {
    ANN_START_OF_MINUTE = 0,
    ANN_SPECIAL_BITS,
    ANN_CALL_BIT,
    ANN_SUMMER_TIME,
    ANN_CEST,
    ANN_CET,
    ANN_LEAP_SECOND,
    ANN_START_OF_TIME,
    ANN_MINUTE,
    ANN_MINUTE_PARITY,
    ANN_HOUR,
    ANN_HOUR_PARITY,
    ANN_DAY,
    ANN_DAY_OF_WEEK,
    ANN_MONTH,
    ANN_YEAR,
    ANN_DATE_PARITY,
    ANN_RAW_BITS,
    ANN_UNKNOWN_BITS,
    ANN_WARN,
    NUM_ANN,
};

enum dcf77_state {
    STATE_WAIT_RISING,
    STATE_GET_BIT,
};

#define DATA_CH 0

static const char* day_names[] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

static const char* month_names[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

struct dcf77_priv {
    enum dcf77_state state;
    uint64_t ss_bit;
    uint64_t ss_bit_old;
    uint64_t es_bit;
    uint64_t ss_block;
    int bitcount;
    int bitnumber_is_known;
    uint64_t tmp;
    int datebits[23];
    int datebits_count;
    int out_ann;
};

static struct srd_channel dcf77_channels[] = {
    { "data", "DATA", "DATA line", 0, SRD_CHANNEL_SDATA, "dec_dcf77_chan_data" },
};

static const char* dcf77_inputs[] = { "logic", NULL };
static const char* dcf77_outputs[] = { NULL };
static const char* dcf77_tags[] = { "Clock/timing", NULL };

static const char* dcf77_ann_labels[][3] = {
    { "", "start-of-minute", "Start of minute" },
    { "", "special-bits", "Special bits (civil warnings, weather forecast)" },
    { "", "call-bit", "Call bit" },
    { "", "summer-time", "Summer time announcement" },
    { "", "cest", "CEST bit" },
    { "", "cet", "CET bit" },
    { "", "leap-second", "Leap second bit" },
    { "", "start-of-time", "Start of encoded time" },
    { "", "minute", "Minute" },
    { "", "minute-parity", "Minute parity bit" },
    { "", "hour", "Hour" },
    { "", "hour-parity", "Hour parity bit" },
    { "", "day", "Day of month" },
    { "", "day-of-week", "Day of week" },
    { "", "month", "Month" },
    { "", "year", "Year" },
    { "", "date-parity", "Date parity bit" },
    { "", "raw-bits", "Raw bits" },
    { "", "unknown-bits", "Unknown bits" },
    { "", "warnings", "Human-readable warnings" },
};

static const int dcf77_row_bits_classes[] = { ANN_RAW_BITS, ANN_UNKNOWN_BITS };
static const int dcf77_row_fields_classes[] = {
    ANN_START_OF_MINUTE, ANN_SPECIAL_BITS, ANN_CALL_BIT, ANN_SUMMER_TIME,
    ANN_CEST, ANN_CET, ANN_LEAP_SECOND, ANN_START_OF_TIME, ANN_MINUTE,
    ANN_MINUTE_PARITY, ANN_HOUR, ANN_HOUR_PARITY, ANN_DAY, ANN_DAY_OF_WEEK,
    ANN_MONTH, ANN_YEAR, ANN_DATE_PARITY
};
static const int dcf77_row_warnings_classes[] = { ANN_WARN };

static const struct srd_c_ann_row dcf77_ann_rows[] = {
    { "bits", "Bits", dcf77_row_bits_classes, 2 },
    { "fields", "Fields", dcf77_row_fields_classes, 17 },
    { "warnings", "Warnings", dcf77_row_warnings_classes, 1 },
};

static int bcd2int(uint64_t bcd)
{
    int result = 0;
    int weights[] = { 1, 2, 4, 8, 10, 20, 40, 80 };
    int i;
    for (i = 0; i < 8; i++) {
        if (bcd & (1ULL << i))
            result += weights[i];
    }
    return result;
}

static int popcount64(uint64_t v)
{
    int count = 0;
    while (v) {
        count += v & 1;
        v >>= 1;
    }
    return count;
}

static void handle_dcf77_bit(struct srd_decoder_inst* di, int bit)
{
    struct dcf77_priv* s = (struct dcf77_priv*)c_decoder_get_private(di);
    int c = s->bitcount;
    char t1[128], t2[64], t3[16];

    if (s->bitnumber_is_known) {
        snprintf(t1, sizeof(t1), "Bit %d: %d", c, bit);
        snprintf(t2, sizeof(t2), "%d", bit);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_RAW_BITS, t1, t2);
    } else {
        snprintf(t1, sizeof(t1), "Unknown bit %d: %d", c, bit);
        snprintf(t2, sizeof(t2), "%d", bit);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_UNKNOWN_BITS, t1, t2);
    }

    if (!s->bitnumber_is_known)
        return;

    if (c >= 36 && c <= 58) {
        s->datebits[s->datebits_count++] = bit;
    }

    if (c == 0) {
        if (bit == 0) {
            c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_START_OF_MINUTE,
                "Start of minute (always 0)", "Start of minute", "SoM");
        } else {
            c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_WARN,
                "Start of minute != 0", "SoM != 0");
        }
    } else if (c >= 1 && c <= 14) {
        if (c == 1) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 1));
        }
        if (c == 14) {
            { char bbuf[15]; fmt_binary14(s->tmp, bbuf, sizeof(bbuf)); snprintf(t1, sizeof(t1), "Special bits: %s", bbuf); }
            { char bbuf[15]; fmt_binary14(s->tmp, bbuf, sizeof(bbuf)); snprintf(t2, sizeof(t2), "SB: %s", bbuf); }
            c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_SPECIAL_BITS, t1, t2);
        }
    } else if (c == 15) {
        const char* not_str = (bit == 1) ? "" : "not ";
        snprintf(t1, sizeof(t1), "Call bit: %sset", not_str);
        snprintf(t2, sizeof(t2), "CB: %sset", not_str);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_CALL_BIT, t1, t2);
    } else if (c == 16) {
        const char* not_str = (bit == 1) ? "" : "not ";
        const char* yesno = (bit == 1) ? "yes" : "no";
        char st4[32];
        snprintf(t1, sizeof(t1), "Summer time announcement: %sactive", not_str);
        snprintf(t2, sizeof(t2), "Summer time: %sactive", not_str);
        snprintf(st4, sizeof(st4), "Summer time: %s", yesno);
        snprintf(t3, sizeof(t3), "ST: %s", yesno);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_SUMMER_TIME, t1, t2, st4, t3);
    } else if (c == 17) {
        const char* not_str = (bit == 1) ? "" : "not ";
        const char* yesno = (bit == 1) ? "yes" : "no";
        snprintf(t1, sizeof(t1), "CEST: %sin effect", not_str);
        snprintf(t2, sizeof(t2), "CEST: %s", yesno);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_CEST, t1, t2);
    } else if (c == 18) {
        const char* not_str = (bit == 1) ? "" : "not ";
        const char* yesno = (bit == 1) ? "yes" : "no";
        snprintf(t1, sizeof(t1), "CET: %sin effect", not_str);
        snprintf(t2, sizeof(t2), "CET: %s", yesno);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_CET, t1, t2);
    } else if (c == 19) {
        const char* not_str = (bit == 1) ? "" : "not ";
        const char* yesno = (bit == 1) ? "yes" : "no";
        char ls4[32];
        snprintf(t1, sizeof(t1), "Leap second announcement: %sactive", not_str);
        snprintf(t2, sizeof(t2), "Leap second: %sactive", not_str);
        snprintf(ls4, sizeof(ls4), "Leap second: %s", yesno);
        snprintf(t3, sizeof(t3), "LS: %s", yesno);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_LEAP_SECOND, t1, t2, ls4, t3);
    } else if (c == 20) {
        if (bit == 1) {
            c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_START_OF_TIME,
                "Start of encoded time (always 1)", "Start of encoded time", "SoeT");
        } else {
            c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_WARN,
                "Start of encoded time != 1", "SoeT != 1");
        }
    } else if (c >= 21 && c <= 27) {
        if (c == 21) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 21));
        }
        if (c == 27) {
            int m = bcd2int(s->tmp);
            snprintf(t1, sizeof(t1), "Minutes: %d", m);
            snprintf(t2, sizeof(t2), "Min: %d", m);
            c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_MINUTE, t1, t2);
        }
    } else if (c == 28) {
        s->tmp |= ((uint64_t)bit << (c - 21));
        int parity = popcount64(s->tmp);
        const char* ok_str = ((parity % 2) == 0) ? "OK" : "INVALID!";
        snprintf(t1, sizeof(t1), "Minute parity: %s", ok_str);
        snprintf(t2, sizeof(t2), "Min parity: %s", ok_str);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_MINUTE_PARITY, t1, t2);
    } else if (c >= 29 && c <= 34) {
        if (c == 29) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 29));
        }
        if (c == 34) {
            int h = bcd2int(s->tmp);
            snprintf(t1, sizeof(t1), "Hours: %d", h);
            c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_HOUR, t1);
        }
    } else if (c == 35) {
        s->tmp |= ((uint64_t)bit << (c - 29));
        int parity = popcount64(s->tmp);
        const char* ok_str = ((parity % 2) == 0) ? "OK" : "INVALID!";
        snprintf(t1, sizeof(t1), "Hour parity: %s", ok_str);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_HOUR_PARITY, t1);
    } else if (c >= 36 && c <= 41) {
        if (c == 36) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 36));
        }
        if (c == 41) {
            int d = bcd2int(s->tmp);
            snprintf(t1, sizeof(t1), "Day: %d", d);
            c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_DAY, t1);
        }
    } else if (c >= 42 && c <= 44) {
        if (c == 42) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 42));
        }
        if (c == 44) {
            int d = bcd2int(s->tmp);
            if (d >= 1 && d <= 7) {
                const char* dn = day_names[d - 1];
                snprintf(t1, sizeof(t1), "Day of week: %d (%s)", d, dn);
                snprintf(t2, sizeof(t2), "DoW: %d (%s)", d, dn);
                c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_DAY_OF_WEEK, t1, t2);
            } else {
                snprintf(t1, sizeof(t1), "Day of week: %d (invalid)", d);
                snprintf(t2, sizeof(t2), "DoW: %d (inv)", d);
                c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_WARN, t1, t2);
            }
        }
    } else if (c >= 45 && c <= 49) {
        if (c == 45) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 45));
        }
        if (c == 49) {
            int m = bcd2int(s->tmp);
            if (m >= 1 && m <= 12) {
                const char* mn = month_names[m];
                snprintf(t1, sizeof(t1), "Month: %d (%s)", m, mn);
                snprintf(t2, sizeof(t2), "Mon: %d (%s)", m, mn);
                c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_MONTH, t1, t2);
            } else {
                snprintf(t1, sizeof(t1), "Month: %d (invalid)", m);
                snprintf(t2, sizeof(t2), "Mon: %d (inv)", m);
                c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_WARN, t1, t2);
            }
        }
    } else if (c >= 50 && c <= 57) {
        if (c == 50) {
            s->tmp = bit;
            s->ss_block = s->ss_bit;
        } else {
            s->tmp |= ((uint64_t)bit << (c - 50));
        }
        if (c == 57) {
            int y = bcd2int(s->tmp);
            snprintf(t1, sizeof(t1), "Year: %d", y);
            c_put(di, s->ss_block, s->es_bit, s->out_ann, ANN_YEAR, t1);
        }
    } else if (c == 58) {
        int i;
        int parity = 0;
        for (i = 0; i < s->datebits_count; i++) {
            parity += s->datebits[i];
        }
        const char* ok_str = ((parity % 2) == 0) ? "OK" : "INVALID!";
        snprintf(t1, sizeof(t1), "Date parity: %s", ok_str);
        snprintf(t2, sizeof(t2), "DP: %s", ok_str);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_DATE_PARITY, t1, t2);
        s->datebits_count = 0;
    } else {
        snprintf(t1, sizeof(t1), "Invalid DCF77 bit: %d", c);
        snprintf(t2, sizeof(t2), "Invalid bit: %d", c);
        snprintf(t3, sizeof(t3), "Inv: %d", c);
        c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_WARN, t1, t2, t3);
    }
}

static void dcf77_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct dcf77_priv)));
    }
    struct dcf77_priv* s = (struct dcf77_priv*)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct dcf77_priv));
    s->state = STATE_WAIT_RISING;
    s->ss_bit_old = 0;
    s->bitnumber_is_known = 0;
    s->datebits_count = 0;
}

static void dcf77_start(struct srd_decoder_inst* di)
{
    struct dcf77_priv* s = (struct dcf77_priv*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "dcf77");
}

static void dcf77_decode(struct srd_decoder_inst* di)
{
    struct dcf77_priv* s = (struct dcf77_priv*)c_decoder_get_private(di);
    int ret;

    while (1) {
        switch (s->state) {

        case STATE_WAIT_RISING: {
            ret = c_wait(di, CW_R(DATA_CH), CW_END);
            if (ret != SRD_OK)
                return;

            s->ss_bit = di_samplenum(di);

            if (s->ss_bit_old != 0) {
                uint64_t len_edges = s->ss_bit - s->ss_bit_old;
                uint64_t samplerate = c_samplerate(di);
                if (samplerate > 0) {
                    int len_edges_ms = (int)(((double)len_edges / (double)samplerate) * 1000.0);
                    if (len_edges_ms >= 1600 && len_edges_ms <= 2400) {
                        s->bitcount = 0;
                        s->bitnumber_is_known = 1;
                    }
                }
            }

            s->ss_bit_old = s->ss_bit;
            s->state = STATE_GET_BIT;
            break;
        }

        case STATE_GET_BIT: {
            ret = c_wait(di, CW_F(DATA_CH), CW_END);
            if (ret != SRD_OK)
                return;

            s->es_bit = di_samplenum(di);

            uint64_t len_high = di_samplenum(di) - s->ss_bit;
            uint64_t samplerate = c_samplerate(di);
            int bit = -1;

            if (samplerate > 0) {
                int len_high_ms = (int)(((double)len_high / (double)samplerate) * 1000.0);
                if (len_high_ms >= 40 && len_high_ms <= 160)
                    bit = 0;
                else if (len_high_ms >= 161 && len_high_ms <= 260)
                    bit = 1;
            }

            if (bit == 0 || bit == 1) {
                handle_dcf77_bit(di, bit);
                s->bitcount++;
            } else {
                c_put(di, s->ss_bit, s->es_bit, s->out_ann, ANN_WARN,
                    "Invalid bit timing", "Inv timing", "Inv");
            }

            s->state = STATE_WAIT_RISING;
            break;
        }
        }
    }
}

static void dcf77_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder dcf77_c_decoder = {
    .id = "dcf77_c",
    .name = "DCF77(C)",
    .longname = "DCF77 time protocol (C)",
    .desc = "European longwave time signal (77.5kHz carrier signal). (C implementation)",
    .license = "gplv2+",
    .channels = dcf77_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = dcf77_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = dcf77_ann_rows,
    .inputs = dcf77_inputs,
    .num_inputs = 1,
    .outputs = dcf77_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = dcf77_tags,
    .num_tags = 1,
    .reset = dcf77_reset,
    .start = dcf77_start,
    .decode = dcf77_decode,
    .destroy = dcf77_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &dcf77_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}