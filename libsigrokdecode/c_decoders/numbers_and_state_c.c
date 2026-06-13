#include "libsigrokdecode.h"
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHANNELS 16
#define MAX_ENUM_SLOTS 32

enum {
    ANN_RAW = 0,
    ANN_NUM,
    ANN_ENUM_0,
    ANN_ENUM_OVR = ANN_ENUM_0 + MAX_ENUM_SLOTS,
    ANN_WARN = ANN_ENUM_OVR + 1,
    NUM_ANN = ANN_WARN + 1,
};

enum nas_interp {
    INTERP_UNSIGNED = 0,
    INTERP_SIGNED,
    INTERP_FIXPOINT,
    INTERP_FIXSIGNED,
    INTERP_IEEE754,
    INTERP_ENUM,
};

enum nas_format {
    FMT_NATIVE = 0,
    FMT_BIN,
    FMT_OCT,
    FMT_DEC,
    FMT_HEX,
};

typedef struct {
    int clk_edge;
    int bitcount;
    int interp;
    int format;
    int fracbits;
    int have_clk;
    int data_channels[MAX_CHANNELS];
    int num_data_channels;
    int out_ann;
    int out_python;
    uint64_t prev_pattern;
    uint64_t ss;
    int bFirst;
    int interp_inited;
    uint64_t signmask;
    uint64_t signfull;
    double fixdiv;
    int fixsign;
    char* enum_names[MAX_ENUM_SLOTS];
    uint64_t enum_values[MAX_ENUM_SLOTS];
    int enum_count;
    int enum_have;
} nas_state;

static struct srd_channel nas_optional_channels[] = {
    { "clk", "Clock", "Clock", 0, SRD_CHANNEL_SCLK, NULL },
    { "bit0", "Bit0", "Bit position 0", 1, SRD_CHANNEL_SDATA, NULL },
    { "bit1", "Bit1", "Bit position 1", 2, SRD_CHANNEL_SDATA, NULL },
    { "bit2", "Bit2", "Bit position 2", 3, SRD_CHANNEL_SDATA, NULL },
    { "bit3", "Bit3", "Bit position 3", 4, SRD_CHANNEL_SDATA, NULL },
    { "bit4", "Bit4", "Bit position 4", 5, SRD_CHANNEL_SDATA, NULL },
    { "bit5", "Bit5", "Bit position 5", 6, SRD_CHANNEL_SDATA, NULL },
    { "bit6", "Bit6", "Bit position 6", 7, SRD_CHANNEL_SDATA, NULL },
    { "bit7", "Bit7", "Bit position 7", 8, SRD_CHANNEL_SDATA, NULL },
    { "bit8", "Bit8", "Bit position 8", 9, SRD_CHANNEL_SDATA, NULL },
    { "bit9", "Bit9", "Bit position 9", 10, SRD_CHANNEL_SDATA, NULL },
    { "bit10", "Bit10", "Bit position 10", 11, SRD_CHANNEL_SDATA, NULL },
    { "bit11", "Bit11", "Bit position 11", 12, SRD_CHANNEL_SDATA, NULL },
    { "bit12", "Bit12", "Bit position 12", 13, SRD_CHANNEL_SDATA, NULL },
    { "bit13", "Bit13", "Bit position 13", 14, SRD_CHANNEL_SDATA, NULL },
    { "bit14", "Bit14", "Bit position 14", 15, SRD_CHANNEL_SDATA, NULL },
    { "bit15", "Bit15", "Bit position 15", 16, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option nas_options[] = {
    { "clkedge", NULL, "Clock edge", NULL, NULL },
    { "count", NULL, "Total bits count", NULL, NULL },
    { "interp", NULL, "Interpretation", NULL, NULL },
    { "fracbits", NULL, "Fraction bits count", NULL, NULL },
    { "mapping", NULL, "Enum to text map file", NULL, NULL },
    { "format", NULL, "Number format", NULL, NULL },
    { "enum", NULL, "Enum name=value pairs (e.g. IDLE=0;RUN=1;STOP=2)", NULL, NULL },
};

static const char* nas_ann_labels[NUM_ANN][3] = {
    { "", "raw", "Raw pattern" },
    { "", "number", "Number" },
    { "", "enum0", "Enumeration slot 0" },
    { "", "enum1", "Enumeration slot 1" },
    { "", "enum2", "Enumeration slot 2" },
    { "", "enum3", "Enumeration slot 3" },
    { "", "enum4", "Enumeration slot 4" },
    { "", "enum5", "Enumeration slot 5" },
    { "", "enum6", "Enumeration slot 6" },
    { "", "enum7", "Enumeration slot 7" },
    { "", "enum8", "Enumeration slot 8" },
    { "", "enum9", "Enumeration slot 9" },
    { "", "enum10", "Enumeration slot 10" },
    { "", "enum11", "Enumeration slot 11" },
    { "", "enum12", "Enumeration slot 12" },
    { "", "enum13", "Enumeration slot 13" },
    { "", "enum14", "Enumeration slot 14" },
    { "", "enum15", "Enumeration slot 15" },
    { "", "enum16", "Enumeration slot 16" },
    { "", "enum17", "Enumeration slot 17" },
    { "", "enum18", "Enumeration slot 18" },
    { "", "enum19", "Enumeration slot 19" },
    { "", "enum20", "Enumeration slot 20" },
    { "", "enum21", "Enumeration slot 21" },
    { "", "enum22", "Enumeration slot 22" },
    { "", "enum23", "Enumeration slot 23" },
    { "", "enum24", "Enumeration slot 24" },
    { "", "enum25", "Enumeration slot 25" },
    { "", "enum26", "Enumeration slot 26" },
    { "", "enum27", "Enumeration slot 27" },
    { "", "enum28", "Enumeration slot 28" },
    { "", "enum29", "Enumeration slot 29" },
    { "", "enum30", "Enumeration slot 30" },
    { "", "enum31", "Enumeration slot 31" },
    { "", "enumovr", "Enumeration overflow" },
    { "", "warning", "Warning" },
};

static const int nas_row_cls[NUM_ANN][2] = {
    { 0, -1 },
    { 1, -1 },
    { 2, -1 },
    { 3, -1 },
    { 4, -1 },
    { 5, -1 },
    { 6, -1 },
    { 7, -1 },
    { 8, -1 },
    { 9, -1 },
    { 10, -1 },
    { 11, -1 },
    { 12, -1 },
    { 13, -1 },
    { 14, -1 },
    { 15, -1 },
    { 16, -1 },
    { 17, -1 },
    { 18, -1 },
    { 19, -1 },
    { 20, -1 },
    { 21, -1 },
    { 22, -1 },
    { 23, -1 },
    { 24, -1 },
    { 25, -1 },
    { 26, -1 },
    { 27, -1 },
    { 28, -1 },
    { 29, -1 },
    { 30, -1 },
    { 31, -1 },
    { 32, -1 },
    { 33, -1 },
    { 34, -1 },
    { 35, -1 },
};

static const struct srd_c_ann_row nas_ann_rows[NUM_ANN] = {
    { "raws", "Raw bits", nas_row_cls[0], 1 },
    { "numbers", "Numbers", nas_row_cls[1], 1 },
    { "enums0", "Enumeration slots 0", nas_row_cls[2], 1 },
    { "enums1", "Enumeration slots 1", nas_row_cls[3], 1 },
    { "enums2", "Enumeration slots 2", nas_row_cls[4], 1 },
    { "enums3", "Enumeration slots 3", nas_row_cls[5], 1 },
    { "enums4", "Enumeration slots 4", nas_row_cls[6], 1 },
    { "enums5", "Enumeration slots 5", nas_row_cls[7], 1 },
    { "enums6", "Enumeration slots 6", nas_row_cls[8], 1 },
    { "enums7", "Enumeration slots 7", nas_row_cls[9], 1 },
    { "enums8", "Enumeration slots 8", nas_row_cls[10], 1 },
    { "enums9", "Enumeration slots 9", nas_row_cls[11], 1 },
    { "enums10", "Enumeration slots 10", nas_row_cls[12], 1 },
    { "enums11", "Enumeration slots 11", nas_row_cls[13], 1 },
    { "enums12", "Enumeration slots 12", nas_row_cls[14], 1 },
    { "enums13", "Enumeration slots 13", nas_row_cls[15], 1 },
    { "enums14", "Enumeration slots 14", nas_row_cls[16], 1 },
    { "enums15", "Enumeration slots 15", nas_row_cls[17], 1 },
    { "enums16", "Enumeration slots 16", nas_row_cls[18], 1 },
    { "enums17", "Enumeration slots 17", nas_row_cls[19], 1 },
    { "enums18", "Enumeration slots 18", nas_row_cls[20], 1 },
    { "enums19", "Enumeration slots 19", nas_row_cls[21], 1 },
    { "enums20", "Enumeration slots 20", nas_row_cls[22], 1 },
    { "enums21", "Enumeration slots 21", nas_row_cls[23], 1 },
    { "enums22", "Enumeration slots 22", nas_row_cls[24], 1 },
    { "enums23", "Enumeration slots 23", nas_row_cls[25], 1 },
    { "enums24", "Enumeration slots 24", nas_row_cls[26], 1 },
    { "enums25", "Enumeration slots 25", nas_row_cls[27], 1 },
    { "enums26", "Enumeration slots 26", nas_row_cls[28], 1 },
    { "enums27", "Enumeration slots 27", nas_row_cls[29], 1 },
    { "enums28", "Enumeration slots 28", nas_row_cls[30], 1 },
    { "enums29", "Enumeration slots 29", nas_row_cls[31], 1 },
    { "enums30", "Enumeration slots 30", nas_row_cls[32], 1 },
    { "enums31", "Enumeration slots 31", nas_row_cls[33], 1 },
    { "enumsovr", "Enumeration overflows", nas_row_cls[34], 1 },
    { "warnings", "Warnings", nas_row_cls[35], 1 },
};

static const char* nas_inputs[] = { "logic", NULL };
static const char* nas_outputs[] = { "numbers_and_state", NULL };
static const char* nas_tags[] = { "Encoding", "Util", NULL };

static void nas_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(nas_state)));
    }
    nas_state* s = (nas_state*)c_decoder_get_private(di);
    for (int i = 0; i < s->enum_count; i++) {
        g_free(s->enum_names[i]);
    }
    memset(s, 0, sizeof(nas_state));
    s->bFirst = 1;
}

static void nas_start(struct srd_decoder_inst* di)
{
    nas_state* s = (nas_state*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "numbers_and_state");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "numbers_and_state");

    const char* ce = c_opt_str(di, "clkedge", "rising");
    if (strcmp(ce, "falling") == 0)
        s->clk_edge = 2;
    else if (strcmp(ce, "either") == 0)
        s->clk_edge = 0;
    else
        s->clk_edge = 1;

    s->bitcount = (int)c_opt_int(di, "count", 0);

    const char* interp = c_opt_str(di, "interp", "unsigned");
    if (strcmp(interp, "signed") == 0)
        s->interp = INTERP_SIGNED;
    else if (strcmp(interp, "fixpoint") == 0)
        s->interp = INTERP_FIXPOINT;
    else if (strcmp(interp, "fixsigned") == 0)
        s->interp = INTERP_FIXSIGNED;
    else if (strcmp(interp, "ieee754") == 0)
        s->interp = INTERP_IEEE754;
    else if (strcmp(interp, "enum") == 0)
        s->interp = INTERP_ENUM;
    else
        s->interp = INTERP_UNSIGNED;

    s->fracbits = (int)c_opt_int(di, "fracbits", 0);

    const char* fmt = c_opt_str(di, "format", "-");
    if (strcmp(fmt, "bin") == 0)
        s->format = FMT_BIN;
    else if (strcmp(fmt, "oct") == 0)
        s->format = FMT_OCT;
    else if (strcmp(fmt, "dec") == 0)
        s->format = FMT_DEC;
    else if (strcmp(fmt, "hex") == 0)
        s->format = FMT_HEX;
    else
        s->format = FMT_NATIVE;

    s->have_clk = c_has_ch(di, 0);
    s->num_data_channels = 0;
    for (int i = 0; i < MAX_CHANNELS; i++) {
        if (c_has_ch(di, i + 1)) {
            s->data_channels[s->num_data_channels] = i + 1;
            s->num_data_channels++;
        }
    }

    if (s->bitcount == 0 && s->num_data_channels > 0) {
        /* Match Python: bitcount = channels[-1] - Pin.BIT_0 + 1
         * Python's range(_max_channels) only goes to _max_channels-1=15,
         * so channels[-1] max is 15, bitcount = 15 - 1 + 1 = 15 */
        int max_ch = s->data_channels[s->num_data_channels - 1];
        if (max_ch > MAX_CHANNELS - 1)
            max_ch = MAX_CHANNELS - 1;
        s->bitcount = max_ch;
    }

    s->enum_count = 0;
    s->enum_have = 0;
    const char* enum_str = c_opt_str(di, "enum", "");
    if (enum_str && enum_str[0] != '\0') {
        gchar** pairs = g_strsplit(enum_str, ";", -1);
        for (int i = 0; pairs[i] && s->enum_count < MAX_ENUM_SLOTS; i++) {
            gchar* pair = g_strstrip(pairs[i]);
            if (!*pair)
                continue;
            gchar** kv = g_strsplit(pair, "=", 2);
            if (kv[0] && kv[1]) {
                gchar* name = g_strstrip(kv[0]);
                gchar* val_str = g_strstrip(kv[1]);
                if (*name && *val_str) {
                    uint64_t val = strtoull(val_str, NULL, 0);
                    s->enum_names[s->enum_count] = g_strdup(name);
                    s->enum_values[s->enum_count] = val;
                    s->enum_count++;
                }
            }
            g_strfreev(kv);
        }
        g_strfreev(pairs);
        s->enum_have = (s->enum_count > 0);
    }
}

static uint64_t nas_grab_pattern(struct srd_decoder_inst* di, nas_state* s, uint64_t samplenum)
{
    (void)samplenum;
    (void)samplenum;
    uint64_t pattern = 0;
    for (int i = 0; i < s->bitcount && i < MAX_CHANNELS; i++) {
        int ch = i + 1;
        if (c_has_ch(di, ch)) {
            int bit = c_pin(di, ch);
            if (bit)
                pattern |= (1ULL << i);
        }
    }
    return pattern;
}

static int nas_interp_init(nas_state* s)
{
    if (s->interp_inited)
        return 0;
    s->interp_inited = 1;

    if (s->interp == INTERP_SIGNED || s->interp == INTERP_FIXPOINT || s->interp == INTERP_FIXSIGNED) {
        s->signmask = 1ULL << (s->bitcount - 1);
        s->signfull = 1ULL << s->bitcount;
    }

    if (s->interp == INTERP_FIXPOINT || s->interp == INTERP_FIXSIGNED) {
        s->fixsign = (s->interp == INTERP_FIXSIGNED);
        s->fixdiv = (double)(1ULL << s->fracbits);
    }

    return 0;
}

static double half_to_double(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    double result;

    if (exp == 0) {
        if (mant == 0) {
            /* Zero */
            result = 0.0;
        } else {
            /* Subnormal: normalize */
            exp = 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FF; /* Remove implicit bit */
            result = ldexp((double)mant / 1024.0, exp - 15);
        }
    } else if (exp == 31) {
        if (mant == 0)
            result = INFINITY;
        else
            result = NAN;
    } else {
        /* Normal */
        result = ldexp(1.0 + (double)mant / 1024.0, (int)exp - 15);
    }

    return sign ? -result : result;
}

static int nas_interp_value(nas_state* s, uint64_t pattern, double* out_value)
{
    nas_interp_init(s);

    switch (s->interp) {
    case INTERP_UNSIGNED:
        *out_value = (double)pattern;
        return 0;
    case INTERP_SIGNED:
        if (pattern & s->signmask)
            *out_value = -(double)(s->signfull - pattern);
        else
            *out_value = (double)pattern;
        return 0;
    case INTERP_FIXPOINT:
        if (s->fixsign) {
            if (pattern & s->signmask)
                *out_value = -(double)(s->signfull - pattern) / s->fixdiv;
            else
                *out_value = (double)pattern / s->fixdiv;
        } else {
            *out_value = (double)pattern / s->fixdiv;
        }
        return 0;
    case INTERP_FIXSIGNED:
        if (pattern & s->signmask)
            *out_value = -(double)(s->signfull - pattern) / s->fixdiv;
        else
            *out_value = (double)pattern / s->fixdiv;
        return 0;
    case INTERP_IEEE754:
        if (s->bitcount == 16) {
            *out_value = half_to_double((uint16_t)pattern);
            return 0;
        } else if (s->bitcount == 32) {
            union {
                uint32_t i;
                float f;
            } u;
            u.i = (uint32_t)pattern;
            *out_value = (double)u.f;
            return 0;
        } else if (s->bitcount == 64) {
            union {
                uint64_t i;
                double d;
            } u;
            u.i = pattern;
            *out_value = u.d;
            return 0;
        }
        return -1;
    case INTERP_ENUM:
        *out_value = (double)pattern;
        return 0;
    }

    return -1;
}

static int nas_format_value(nas_state* s, double value, uint64_t pattern, char* buf, int bufsize)
{
    (void)pattern;
    int64_t ival = (int64_t)value;
    uint64_t uval = (uint64_t)value;

    switch (s->format) {
    case FMT_NATIVE:
        if (s->interp == INTERP_FIXPOINT || s->interp == INTERP_FIXSIGNED || s->interp == INTERP_IEEE754)
            snprintf(buf, bufsize, "%g", value);
        else if (s->interp == INTERP_SIGNED || s->interp == INTERP_FIXSIGNED)
            snprintf(buf, bufsize, "%lld", (long long)ival);
        else
            snprintf(buf, bufsize, "%llu", (unsigned long long)uval);
        return 0;
    case FMT_BIN:
        if (value < 0)
            return -1;
        /* Manual binary conversion since %llb is not C11 standard */
        {
            int bc = s->bitcount;
            if (bc > 64) bc = 64;
            if (bc > (int)bufsize - 1) bc = (int)bufsize - 1;
            for (int i = 0; i < bc; i++) {
                buf[i] = (uval >> (bc - 1 - i)) & 1 ? '1' : '0';
            }
            buf[bc] = '\0';
        }
        return 0;
    case FMT_OCT:
        if (value < 0)
            return -1;
        snprintf(buf, bufsize, "%0*llo", (s->bitcount + 2) / 3, (unsigned long long)uval);
        return 0;
    case FMT_DEC:
        if (s->interp == INTERP_SIGNED || s->interp == INTERP_FIXSIGNED)
            snprintf(buf, bufsize, "%lld", (long long)ival);
        else
            snprintf(buf, bufsize, "%llu", (unsigned long long)uval);
        return 0;
    case FMT_HEX:
        if (value < 0)
            return -1;
        snprintf(buf, bufsize, "%0*llx", (s->bitcount + 3) / 4, (unsigned long long)uval);
        return 0;
    }

    return -1;
}

static const char* nas_lookup_enum(nas_state* s, uint64_t pattern)
{
    for (int i = 0; i < s->enum_count; i++) {
        if (s->enum_values[i] == pattern)
            return s->enum_names[i];
    }
    return NULL;
}

static void nas_handle_pattern(struct srd_decoder_inst* di, nas_state* s,
    uint64_t ss, uint64_t es, uint64_t pattern)
{
    char raw_str[65];
    for (int i = 0; i < s->bitcount && i < 64; i++) {
        int bit = (pattern >> (s->bitcount - 1 - i)) & 1;
        raw_str[i] = bit ? '1' : '0';
    }
    raw_str[s->bitcount < 64 ? s->bitcount : 64] = '\0';
    c_put(di, ss, es, s->out_ann, ANN_RAW, raw_str);

    {
        unsigned char py_raw[12];
        int pos = 0;
        py_raw[pos++] = (unsigned char)(s->bitcount & 0xFF);
        for (int i = 0; i < 8 && pos < (int)sizeof(py_raw); i++)
            py_raw[pos++] = (unsigned char)((pattern >> (8 * i)) & 0xFF);
        c_proto(di, ss, es, s->out_python, "RAW", C_U8(s->bitcount), C_U64(pattern), C_END);
    }

    double value;
    if (nas_interp_value(s, pattern, &value) != 0)
        return;

    {
        unsigned char py_num[8];
        union {
            double d;
            unsigned char b[8];
        } u;
        u.d = value;
        memcpy(py_num, u.b, 8);
        c_proto(di, ss, es, s->out_python, "NUMBER", C_F64(value), C_END);
    }

    char fmt_buf[128];
    if (nas_format_value(s, value, pattern, fmt_buf, sizeof(fmt_buf)) != 0)
        return;

    const char* enum_name = nas_lookup_enum(s, pattern);
    const char* display_text = enum_name ? enum_name : fmt_buf;

    c_put(di, ss, es, s->out_ann, ANN_NUM, display_text);

    if (s->interp == INTERP_ENUM) {
        int cls;
        if (pattern < MAX_ENUM_SLOTS)
            cls = ANN_ENUM_0 + (int)pattern;
        else
            cls = ANN_ENUM_OVR;
        c_put(di, ss, es, s->out_ann, cls, display_text);
    }

    if (enum_name || s->interp == INTERP_ENUM) {
        unsigned char py_enum[8 + 64];
        union {
            double d;
            unsigned char b[8];
        } u;
        u.d = value;
        memcpy(py_enum, u.b, 8);
        const char* name_to_send = enum_name ? enum_name : fmt_buf;
        int namelen = (int)strlen(name_to_send);
        if (namelen > 64) namelen = 64;
        memcpy(py_enum + 8, name_to_send, namelen);
        c_proto(di, ss, es, s->out_python, "ENUM", C_F64(value), C_STR(name_to_send), C_END);
    }
}

static void nas_decode(struct srd_decoder_inst* di)
{
    nas_state* s = (nas_state*)c_decoder_get_private(di);
    if (s->num_data_channels == 0)
        return;

    /* Read initial sample at current position, like Python's self.wait(cur_cond=None) */
    {
        if (c_wait(di, CW_END) == SRD_OK) {
            uint64_t cur_sample = di_samplenum(di);
            s->ss = cur_sample;
            s->prev_pattern = nas_grab_pattern(di, s, cur_sample);
            s->bFirst = 0;
        }
    }

    while (1) {
        int ret;
        if (s->clk_edge == 1)
            ret = c_wait(di, CW_R(0), CW_END);
        else if (s->clk_edge == 2)
            ret = c_wait(di, CW_F(0), CW_END);
        else
            ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t pattern = nas_grab_pattern(di, s, di_samplenum(di));

        uint64_t es = di_samplenum(di);
        if (pattern == s->prev_pattern)
            continue;

        nas_handle_pattern(di, s, s->ss, es, s->prev_pattern);
        s->ss = es;
        s->prev_pattern = pattern;
    }
}

static void nas_destroy(struct srd_decoder_inst* di)
{
    nas_state* s = (nas_state*)c_decoder_get_private(di);
    if (s) {
        for (int i = 0; i < s->enum_count; i++) {
            g_free(s->enum_names[i]);
        }
        g_free(s);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder numbers_and_state_c_decoder = {
    .id = "numbers_and_state_c",
    .name = "Numbers and State(C)",
    .longname = "Interpret bit patters as numbers or state enums (C)",
    .desc = "Interpret bit patterns as different kinds of numbers (integer, float, enum). (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = nas_optional_channels,
    .num_optional_channels = 17,
    .options = nas_options,
    .num_options = 7,
    .num_annotations = NUM_ANN,
    .ann_labels = nas_ann_labels,
    .num_annotation_rows = NUM_ANN,
    .annotation_rows = nas_ann_rows,
    .inputs = nas_inputs,
    .num_inputs = 1,
    .outputs = nas_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = nas_tags,
    .num_tags = 2,
    .reset = nas_reset,
    .start = nas_start,
    .decode = nas_decode,
    .destroy = nas_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    nas_options[0].def = g_variant_new_string("rising");
    GSList* clkedge_vals = NULL;
    clkedge_vals = g_slist_append(clkedge_vals, g_variant_new_string("rising"));
    clkedge_vals = g_slist_append(clkedge_vals, g_variant_new_string("falling"));
    clkedge_vals = g_slist_append(clkedge_vals, g_variant_new_string("either"));
    nas_options[0].values = clkedge_vals;

    nas_options[1].def = g_variant_new_int64(0);

    nas_options[2].def = g_variant_new_string("unsigned");
    GSList* interp_vals = NULL;
    interp_vals = g_slist_append(interp_vals, g_variant_new_string("unsigned"));
    interp_vals = g_slist_append(interp_vals, g_variant_new_string("signed"));
    interp_vals = g_slist_append(interp_vals, g_variant_new_string("fixpoint"));
    interp_vals = g_slist_append(interp_vals, g_variant_new_string("fixsigned"));
    interp_vals = g_slist_append(interp_vals, g_variant_new_string("ieee754"));
    interp_vals = g_slist_append(interp_vals, g_variant_new_string("enum"));
    nas_options[2].values = interp_vals;

    nas_options[3].def = g_variant_new_int64(0);

    nas_options[4].def = g_variant_new_string("enumtext.json");

    nas_options[5].def = g_variant_new_string("-");
    GSList* format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("-"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("bin"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("oct"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("dec"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("hex"));
    nas_options[5].values = format_vals;

    nas_options[6].def = g_variant_new_string("");

    return &numbers_and_state_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}