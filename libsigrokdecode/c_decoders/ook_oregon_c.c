#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BIT = 0,
    ANN_FIELD,
    ANN_L2,
    ANN_PREAMBLE,
    ANN_SYNC,
    ANN_SENSOR_ID,
    ANN_CHANNEL,
    ANN_ROLLING_CODE,
    ANN_FLAGS1,
    NUM_ANN,
};

enum oregon_version {
    ORE_VER_UNKNOWN = 0,
    ORE_VER_V1,
    ORE_VER_V21,
    ORE_VER_V3,
};

enum oregon_sensor_type {
    ORE_TYPE_UNKNOWN = 0,
    ORE_TYPE_TEMP,
    ORE_TYPE_TEMP_HUM,
    ORE_TYPE_TEMP_HUM1,
    ORE_TYPE_TEMP_HUM_BARO,
    ORE_TYPE_TEMP_HUM_BARO1,
    ORE_TYPE_UV,
    ORE_TYPE_UV1,
    ORE_TYPE_WIND,
    ORE_TYPE_RAIN,
    ORE_TYPE_RAIN1,
};

#define ORE_MAX_BITS 4096
#define ORE_MAX_NIBBLES 512

typedef struct {
    int out_ann;
    int out_binary;
    /* Bit data from ook protocol */
    uint64_t *ss_arr;
    uint64_t *es_arr;
    char *bit_arr;       /* '0', '1', 'E' */
    int num_bits;
    int arr_capacity;
    /* Concatenated bit string */
    char ookstring[ORE_MAX_BITS];
    int ook_len;
    /* Decode position */
    int decode_pos;
    /* Version */
    enum oregon_version ver;
    char ver_str[8];
    /* Decoded nibbles for L2: [ss, es, label_int, hex_char] */
    uint64_t nib_ss[ORE_MAX_NIBBLES];
    uint64_t nib_es[ORE_MAX_NIBBLES];
    char nib_hex[ORE_MAX_NIBBLES];  /* hex char or '\0' for blank */
    int nib_label[ORE_MAX_NIBBLES]; /* label as int, -1 for position */
    int num_nibbles;
    /* Sensor ID */
    char sensor_id[8];
    enum oregon_sensor_type sensor_type;
    /* Options */
    int unknown_type;
} oregon_state;

/* Sensor lookup table from lists.py */
typedef struct {
    const char *id;
    const char *models;
    enum oregon_sensor_type type;
} oregon_sensor_entry;

static const oregon_sensor_entry oregon_sensors[] = {
    {"1984", "WGR800", ORE_TYPE_WIND},
    {"1994", "WGR800", ORE_TYPE_WIND},
    {"1A2D", "THGR228N", ORE_TYPE_TEMP_HUM1},
    {"1A3D", "THGR918", ORE_TYPE_UNKNOWN},
    {"1D20", "THGR228N", ORE_TYPE_TEMP_HUM},
    {"1D30", "THGN500", ORE_TYPE_UNKNOWN},
    {"2914", "PCR800", ORE_TYPE_RAIN},
    {"2A19", "PCR800", ORE_TYPE_RAIN1},
    {"2A1D", "RGR918", ORE_TYPE_RAIN},
    {"2D10", "RGR968", ORE_TYPE_RAIN1},
    {"3A0D", "STR918", ORE_TYPE_WIND},
    {"5A5D", "BTHR918", ORE_TYPE_UNKNOWN},
    {"5A6D", "BTHR918N", ORE_TYPE_TEMP_HUM_BARO},
    {"5D53", "BTHGN129", ORE_TYPE_UNKNOWN},
    {"5D60", "BTHR968", ORE_TYPE_TEMP_HUM_BARO},
    {"C844", "THWR800", ORE_TYPE_TEMP},
    {"CC13", "RTGR328N", ORE_TYPE_TEMP_HUM},
    {"CC23", "THGR328N", ORE_TYPE_TEMP_HUM},
    {"CD39", "RTHR328N", ORE_TYPE_TEMP},
    {"D874", "UVN800", ORE_TYPE_UV1},
    {"EA4C", "THWR288A", ORE_TYPE_TEMP},
    {"EC40", "THN132N", ORE_TYPE_TEMP},
    {"EC70", "UVR128", ORE_TYPE_UV},
    {"F824", "THGN800", ORE_TYPE_TEMP_HUM},
    {"F8B4", "THGR810", ORE_TYPE_TEMP_HUM},
    {NULL, NULL, ORE_TYPE_UNKNOWN},
};

/* Sensor checksum exceptions */
typedef struct {
    const char *id;
    const char *chk_ver;
    const char *comment;
} oregon_checksum_entry;

static const oregon_checksum_entry oregon_checksums[] = {
    {"1D20", "v3", "THGR228N"},
    {"5D60", "v3", "BTHR918N"},
    {"EC40", "v3", "THN132N"},
    {NULL, NULL, NULL},
};

static const char *dir_table[] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW", "N"
};

static const char *oregon_inputs[] = {"ook", NULL};
static const char *oregon_tags[] = {"Sensor", NULL};

static struct srd_decoder_option oregon_options[] = {
    {"unknown", "dec_ook_oregon_opt_unknown", "Unknown type is", NULL, NULL},
};

static const char *oregon_ann_labels[][3] = {
    {"", "Bit", "Bit"},
    {"", "Field", "Field"},
    {"", "L2", "Level 2"},
    {"", "Preamble", "Preamble"},
    {"", "Sync", "Sync"},
    {"", "SensorID", "SensorID"},
    {"", "Channel", "Channel"},
    {"", "Rolling code", "Rolling code"},
    {"", "Flags1", "Flags1"},
};

static const int oreg_row_bits_classes[] = {ANN_BIT, -1};
static const int oreg_row_fields_classes[] = {ANN_FIELD, ANN_PREAMBLE, ANN_SYNC, -1};
static const int oreg_row_l2_classes[] = {ANN_L2, -1};

static const struct srd_c_ann_row oregon_ann_rows[] = {
    {"bits", "Bits", oreg_row_bits_classes, 1},
    {"fields", "Fields", oreg_row_fields_classes, 3},
    {"l2", "Level 2", oreg_row_l2_classes, 1},
};

static int __attribute__((unused)) bcd2int(unsigned char b)
{
    return ((b >> 4) & 0x0f) * 10 + (b & 0x0f);
}

static const char *oregon_find_sensor_type(const char *sensor_id, enum oregon_sensor_type *type)
{
    for (int i = 0; oregon_sensors[i].id != NULL; i++) {
        if (strcmp(sensor_id, oregon_sensors[i].id) == 0) {
            *type = oregon_sensors[i].type;
            return oregon_sensors[i].models;
        }
    }
    *type = ORE_TYPE_UNKNOWN;
    return "Unknown";
}

static const char *oregon_find_checksum_ver(const char *sensor_id)
{
    for (int i = 0; oregon_checksums[i].id != NULL; i++) {
        if (strcmp(sensor_id, oregon_checksums[i].id) == 0)
            return oregon_checksums[i].chk_ver;
    }
    return NULL;
}

static void oregon_put_nib(struct srd_decoder_inst *di, oregon_state *s,
                            const char *label, int numbits)
{
    int num_nibbles = numbits / 4;

    /* Build the hex result from the ookstring at decode_pos */
    /* First extract nibbles, reverse each nibble */
    char nib_strs[64][5];
    memset(nib_strs, 0, sizeof(nib_strs));
    int has_error = 0;
    for (int i = 0; i < num_nibbles && i < 64; i++) {
        int base = s->decode_pos + 4 * i;
        for (int j = 0; j < 4 && (base + j) < s->ook_len; j++)
            nib_strs[i][j] = s->ookstring[base + j];
        nib_strs[i][4] = '\0';
        /* Reverse */
        char tmp;
        tmp = nib_strs[i][0]; nib_strs[i][0] = nib_strs[i][3]; nib_strs[i][3] = tmp;
        tmp = nib_strs[i][1]; nib_strs[i][1] = nib_strs[i][2]; nib_strs[i][2] = tmp;
        if (strchr(nib_strs[i], 'E'))
            has_error = 1;
    }

    char hex_result[64];
    if (has_error) {
        hex_result[0] = '\0';
    } else {
        /* Convert nibbles to binary then to hex */
        unsigned int val = 0;
        for (int i = 0; i < num_nibbles; i++) {
            val = (val << 4) | (strtol(nib_strs[i], NULL, 2) & 0xF);
        }
        snprintf(hex_result, sizeof(hex_result), "%X", val);
        /* Pad with leading zeros */
        int expected_len = num_nibbles;
        int actual_len = (int)strlen(hex_result);
        if (actual_len < expected_len) {
            char padded[64];
            memset(padded, '0', expected_len - actual_len);
            memcpy(padded + (expected_len - actual_len), hex_result, actual_len + 1);
            strcpy(hex_result, padded);
        }
    }

    /* Build label */
    char field_text[128];
    if (label[0] != '\0')
        snprintf(field_text, sizeof(field_text), "%s: %s", label, hex_result);
    else
        snprintf(field_text, sizeof(field_text), "%s", hex_result);

    /* Put field annotation */
    if (s->decode_pos < s->num_bits && (s->decode_pos + numbits - 1) < s->num_bits) {
        c_put(di, s->ss_arr[s->decode_pos], s->es_arr[s->decode_pos + numbits - 1],
                  s->out_ann, ANN_FIELD, field_text);
    }

    /* Save nibbles for L2 decoder */
    for (int i = 0; i < num_nibbles; i++) {
        int nib_idx = s->decode_pos / 4 + i;
        int base = s->decode_pos + 4 * i;
        if (s->num_nibbles < ORE_MAX_NIBBLES && base + 3 < s->num_bits) {
            s->nib_ss[s->num_nibbles] = s->ss_arr[base];
            s->nib_es[s->num_nibbles] = s->es_arr[base + 3];
            if (label[0] != '\0')
                s->nib_label[s->num_nibbles] = -1; /* named label */
            else
                s->nib_label[s->num_nibbles] = nib_idx;
            /* Individual nibble hex */
            if (strchr(nib_strs[i], 'E'))
                s->nib_hex[s->num_nibbles] = '\0';
            else {
                unsigned int nv = strtol(nib_strs[i], NULL, 2) & 0xF;
                s->nib_hex[s->num_nibbles] = "0123456789ABCDEF"[nv];
            }
            s->num_nibbles++;
        }
    }

    s->decode_pos += numbits;
}

static void oregon_put_l2_param(struct srd_decoder_inst *di, oregon_state *s,
                                 int offset, int digits, int dec_point,
                                 const char *pre_label, const char *label)
{
    char out_string[64];
    int pos = 0;
    for (int i = offset; i < offset + digits && i < s->num_nibbles; i++) {
        if (s->nib_hex[i] != '\0')
            out_string[pos++] = s->nib_hex[i];
    }
    out_string[pos] = '\0';

    if ((int)strlen(out_string) == digits) {
        double result = 0;
        for (int i = dec_point; i > 0; i--) {
            int idx = offset + dec_point - i;
            if (idx < s->num_nibbles && s->nib_hex[idx] != '\0') {
                int v;
                char h[2] = {s->nib_hex[idx], '\0'};
                v = strtol(h, NULL, 16);
                result += v / pow(10, i);
            }
        }
        for (int i = dec_point; i < digits; i++) {
            int idx = offset + i;
            if (idx < s->num_nibbles && s->nib_hex[idx] != '\0') {
                int v;
                char h[2] = {s->nib_hex[idx], '\0'};
                v = strtol(h, NULL, 16);
                result += v * pow(10, i - dec_point);
            }
        }
        char buf[256];
        /* Format result without trailing zeros */
        snprintf(buf, sizeof(buf), "%g", result);
        char text[512];
        snprintf(text, sizeof(text), "%s%s%s", pre_label, buf, label);
        uint64_t es = s->nib_es[offset + digits - 1];
        /* Align temp to include +/- nibble */
        if (strcmp(label, "\xC2\xB0""C") == 0 && (offset + digits) < s->num_nibbles)
            es = s->nib_es[offset + digits];
        c_put(di, s->nib_ss[offset], es, s->out_ann, ANN_L2, text);
    }
}

static void oregon_temp(struct srd_decoder_inst *di, oregon_state *s, int offset)
{
    if (offset + 3 >= s->num_nibbles)
        return;
    char temp_sign = '?';
    if (s->nib_hex[offset + 3] != '\0') {
        char h[2] = {s->nib_hex[offset + 3], '\0'};
        int v = strtol(h, NULL, 16);
        temp_sign = (v != 0) ? '-' : '+';
    }
    char pre[4];
    pre[0] = temp_sign;
    pre[1] = '\0';
    oregon_put_l2_param(di, s, offset, 3, 1, pre, "\xC2\xB0""C");
}

static void oregon_baro(struct srd_decoder_inst *di, oregon_state *s, int offset)
{
    if (offset + 2 >= s->num_nibbles)
        return;
    if (s->nib_hex[offset] == '\0' || s->nib_hex[offset + 1] == '\0' || s->nib_hex[offset + 2] == '\0')
        return;
    char h1[2] = {s->nib_hex[offset + 1], '\0'};
    char h0[2] = {s->nib_hex[offset], '\0'};
    int val = (strtol(h1, NULL, 16) << 4) | strtol(h0, NULL, 16);
    int baro = val + 856;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d mb", baro);
    c_put(di, s->nib_ss[offset], s->nib_es[offset + 3], s->out_ann, ANN_L2, buf);
}

static void oregon_wind_dir(struct srd_decoder_inst *di, oregon_state *s, int offset)
{
    if (offset >= s->num_nibbles || s->nib_hex[offset] == '\0')
        return;
    char h[2] = {s->nib_hex[offset], '\0'};
    int v = strtol(h, NULL, 16);
    int w_dir = (int)(v * 22.5);
    int idx = (int)floor((w_dir + 11.25) / 22.5);
    if (idx < 0) idx = 0;
    if (idx > 16) idx = 16;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s (%d\xC2\xB0)", dir_table[idx], w_dir);
    c_put(di, s->nib_ss[offset], s->nib_es[offset], s->out_ann, ANN_L2, buf);
}

static void oregon_channel(struct srd_decoder_inst *di, oregon_state *s, int offset)
{
    if (offset >= s->num_nibbles || s->nib_hex[offset] == '\0')
        return;
    char h[2] = {s->nib_hex[offset], '\0'};
    int ch = strtol(h, NULL, 16);
    char channel_str[16] = "";
    if (s->ver != ORE_VER_V3) {
        if (ch != 0) {
            int bit_pos = 0;
            while ((ch & 1) == 0) {
                bit_pos++;
                ch >>= 1;
            }
            if (s->ver == ORE_VER_V21)
                bit_pos += 1;
            snprintf(channel_str, sizeof(channel_str), "%d", bit_pos);
        }
    } else {
        snprintf(channel_str, sizeof(channel_str), "%d", ch);
    }
    if (channel_str[0] != '\0') {
        char buf[32];
        snprintf(buf, sizeof(buf), "Ch %s", channel_str);
        c_put(di, s->nib_ss[offset], s->nib_es[offset], s->out_ann, ANN_L2, buf);
    }
}

static void oregon_battery(struct srd_decoder_inst *di, oregon_state *s, int offset)
{
    if (offset >= s->num_nibbles || s->nib_hex[offset] == '\0')
        return;
    char h[2] = {s->nib_hex[offset], '\0'};
    int v = strtol(h, NULL, 16);
    const char *batt = ((v >> 2) & 0x1) ? "Low" : "OK";
    char buf[32];
    snprintf(buf, sizeof(buf), "Batt %s", batt);
    c_put(di, s->nib_ss[offset], s->nib_es[offset], s->out_ann, ANN_L2, buf);
}

static void oregon_put_checksum(struct srd_decoder_inst *di, oregon_state *s,
                                 int nibbles, int checksum)
{
    if (nibbles + 1 >= s->num_nibbles)
        return;
    const char *result = "BAD";
    if (s->nib_hex[nibbles + 1] != '\0' && s->nib_hex[nibbles] != '\0' && checksum != -1) {
        char h_hi[2] = {s->nib_hex[nibbles + 1], '\0'};
        char h_lo[2] = {s->nib_hex[nibbles], '\0'};
        int rx;
        if (s->ver != ORE_VER_V1)
            rx = (strtol(h_hi, NULL, 16) << 4) | strtol(h_lo, NULL, 16);
        else
            rx = (strtol(h_lo, NULL, 16) << 4) | strtol(h_hi, NULL, 16);
        if (checksum == rx)
            result = "OK";
    }
    char rx_check[4];
    snprintf(rx_check, sizeof(rx_check), "%c%c",
             s->nib_hex[nibbles + 1] ? s->nib_hex[nibbles + 1] : '?',
             s->nib_hex[nibbles] ? s->nib_hex[nibbles] : '?');
    char buf[256];
    snprintf(buf, sizeof(buf), "Checksum %s Calc %X Rx %s", result, checksum, rx_check);
    c_put(di, s->nib_ss[nibbles], s->nib_es[nibbles + 1], s->out_ann, ANN_L2, buf);
}

static void oregon_checksum(struct srd_decoder_inst *di, oregon_state *s, int nibbles)
{
    int checksum = 0;
    for (int i = 0; i < nibbles; i++) {
        int base = i * 4;
        if (base + 3 >= s->ook_len) {
            checksum = -1;
            break;
        }
        char nibble[5];
        memcpy(nibble, s->ookstring + base, 4);
        nibble[4] = '\0';
        /* Reverse */
        char tmp;
        tmp = nibble[0]; nibble[0] = nibble[3]; nibble[3] = tmp;
        tmp = nibble[1]; nibble[1] = nibble[2]; nibble[2] = tmp;
        if (strchr(nibble, 'E')) {
            checksum = -1;
            break;
        }
        checksum += strtol(nibble, NULL, 2);
        if (checksum > 255)
            checksum -= 255;
    }
    const char *chk_ver = oregon_find_checksum_ver(s->sensor_id);
    if (chk_ver != NULL) {
        if (strcmp(chk_ver, "v1") == 0) s->ver = ORE_VER_V1;
        else if (strcmp(chk_ver, "v2.1") == 0) s->ver = ORE_VER_V21;
        else if (strcmp(chk_ver, "v3") == 0) s->ver = ORE_VER_V3;
    }
    if (s->ver == ORE_VER_V21)
        checksum -= 10;
    oregon_put_checksum(di, s, nibbles, checksum);
}

static void oregon_checksum_v1(struct srd_decoder_inst *di, oregon_state *s)
{
    int checksum = 0;
    for (int i = 0; i < 3; i++) {
        int idx_lo = 2 * i;
        int idx_hi = 2 * i + 1;
        if (idx_hi >= s->num_nibbles || s->nib_hex[idx_lo] == '\0' || s->nib_hex[idx_hi] == '\0') {
            checksum = -1;
            break;
        }
        char h_lo[2] = {s->nib_hex[idx_lo], '\0'};
        char h_hi[2] = {s->nib_hex[idx_hi], '\0'};
        checksum += ((strtol(h_lo, NULL, 16) & 0xF) << 4) | (strtol(h_hi, NULL, 16) & 0xF);
        if (checksum > 255)
            checksum -= 255;
    }
    oregon_put_checksum(di, s, 6, checksum);
}

static void oregon_level2(struct srd_decoder_inst *di, oregon_state *s)
{
    if (s->num_nibbles < 4)
        return;
    /* Build sensor_id */
    s->sensor_id[0] = s->nib_hex[0] ? s->nib_hex[0] : '?';
    s->sensor_id[1] = s->nib_hex[1] ? s->nib_hex[1] : '?';
    s->sensor_id[2] = s->nib_hex[2] ? s->nib_hex[2] : '?';
    s->sensor_id[3] = s->nib_hex[3] ? s->nib_hex[3] : '?';
    s->sensor_id[4] = '\0';

    enum oregon_sensor_type stype = ORE_TYPE_UNKNOWN;
    const char *models = oregon_find_sensor_type(s->sensor_id, &stype);
    if (stype == ORE_TYPE_UNKNOWN && s->unknown_type != ORE_TYPE_UNKNOWN)
        stype = (enum oregon_sensor_type)s->unknown_type;
    s->sensor_type = stype;

    char text[128];
    snprintf(text, sizeof(text), "%s - %d", models, stype);
    c_put(di, s->nib_ss[0], s->nib_es[3], s->out_ann, ANN_L2, text);

    oregon_channel(di, s, 4);
    oregon_battery(di, s, 7);

    switch (stype) {
    case ORE_TYPE_RAIN:
        oregon_put_l2_param(di, s, 8, 4, 2, "", " in/hr");
        oregon_put_l2_param(di, s, 12, 6, 3, "Total ", " in");
        oregon_checksum(di, s, 18);
        break;
    case ORE_TYPE_RAIN1:
        oregon_put_l2_param(di, s, 8, 3, 1, "", " mm/hr");
        oregon_put_l2_param(di, s, 11, 5, 1, "Total ", " mm");
        oregon_checksum(di, s, 18);
        break;
    case ORE_TYPE_TEMP:
        oregon_temp(di, s, 8);
        oregon_checksum(di, s, 12);
        break;
    case ORE_TYPE_TEMP_HUM_BARO:
        oregon_temp(di, s, 8);
        oregon_put_l2_param(di, s, 12, 2, 0, "Hum ", "%");
        oregon_baro(di, s, 15);
        oregon_checksum(di, s, 19);
        break;
    case ORE_TYPE_TEMP_HUM_BARO1:
        oregon_temp(di, s, 8);
        oregon_put_l2_param(di, s, 12, 2, 0, "Hum ", "%");
        oregon_baro(di, s, 14);
        break;
    case ORE_TYPE_TEMP_HUM:
        oregon_temp(di, s, 8);
        oregon_put_l2_param(di, s, 12, 2, 0, "Hum ", "%");
        oregon_checksum(di, s, 15);
        break;
    case ORE_TYPE_TEMP_HUM1:
        oregon_temp(di, s, 8);
        oregon_put_l2_param(di, s, 12, 2, 0, "Hum ", "%");
        oregon_checksum(di, s, 14);
        break;
    case ORE_TYPE_UV:
        oregon_put_l2_param(di, s, 8, 2, 0, "", "");
        break;
    case ORE_TYPE_UV1:
        oregon_put_l2_param(di, s, 11, 2, 0, "", "");
        break;
    case ORE_TYPE_WIND:
        oregon_wind_dir(di, s, 8);
        oregon_put_l2_param(di, s, 11, 3, 1, "Gust ", " m/s");
        oregon_put_l2_param(di, s, 14, 3, 1, "Speed ", " m/s");
        oregon_checksum(di, s, 17);
        break;
    default:
        break;
    }
}

static void oregon_dump_hex(struct srd_decoder_inst *di, oregon_state *s,
                             uint64_t start, uint64_t finish)
{
    char hexstring[512];
    int pos = 0;
    for (int i = 0; i < s->num_nibbles && pos < 400; i++) {
        if (s->nib_hex[i] != '\0')
            hexstring[pos++] = s->nib_hex[i];
        else
            hexstring[pos++] = ' ';
    }
    hexstring[pos] = '\0';

    char buf[600];
    snprintf(buf, sizeof(buf), "Oregon %s \"%s\"", s->ver_str, hexstring);
    c_put(di, start, finish, s->out_ann, ANN_BIT, buf);
}

static void oregon_put_pre_and_sync(struct srd_decoder_inst *di, oregon_state *s,
                                     int len_pream, int len_sync, const char *ver)
{
    int decode_pos = len_pream + len_sync;
    if (decode_pos >= s->num_bits)
        return;

    /* Preamble annotation */
    char pre_text[64];
    snprintf(pre_text, sizeof(pre_text), "Oregon %s Preamble", ver);
    c_put(di, s->ss_arr[0], s->ss_arr[len_pream], s->out_ann, ANN_FIELD, pre_text);

    /* Sync annotation */
    c_put(di, s->ss_arr[len_pream], s->ss_arr[decode_pos], s->out_ann, ANN_FIELD, "Sync");

    /* Strip off preamble and sync */
    int strip = decode_pos;
    memmove(s->ookstring, s->ookstring + strip, s->ook_len - strip + 1);
    s->ook_len -= strip;
    memmove(s->ss_arr, s->ss_arr + strip, (s->num_bits - strip) * sizeof(uint64_t));
    memmove(s->es_arr, s->es_arr + strip, (s->num_bits - strip) * sizeof(uint64_t));
    memmove(s->bit_arr, s->bit_arr + strip, (s->num_bits - strip) * sizeof(char));
    s->num_bits -= strip;
    s->decode_pos = 0;
    strncpy(s->ver_str, ver, sizeof(s->ver_str) - 1);
}

static void oregon_v1(struct srd_decoder_inst *di, oregon_state *s)
{
    s->decode_pos = 0;
    s->num_nibbles = 0;
    if (s->num_bits < 32)
        return;
    oregon_put_nib(di, s, "RollingCode", 4);
    oregon_put_nib(di, s, "Ch", 4);
    oregon_put_nib(di, s, "Temp", 16);
    oregon_put_nib(di, s, "Checksum", 8);

    oregon_dump_hex(di, s, s->ss_arr[0], s->es_arr[s->num_bits - 1]);

    /* L2 decode */
    oregon_temp(di, s, 2);
    oregon_channel(di, s, 1);
    oregon_battery(di, s, 2);
    oregon_checksum_v1(di, s);
}

static void oregon_v3(struct srd_decoder_inst *di, oregon_state *s)
{
    s->decode_pos = 0;
    s->num_nibbles = 0;

    if (s->num_bits < 32) {
        c_put(di, s->ss_arr[0], s->es_arr[s->num_bits - 1],
                  s->out_ann, ANN_FIELD, "Too short to decode");
        return;
    }

    oregon_put_nib(di, s, "SensorID", 16);
    oregon_put_nib(di, s, "Ch", 4);
    oregon_put_nib(di, s, "RollingCode", 8);
    oregon_put_nib(di, s, "Flags1", 4);

    int rem_nibbles = (s->ook_len - s->decode_pos) / 4;
    for (int i = 0; i < rem_nibbles; i++)
        oregon_put_nib(di, s, "", 4);

    oregon_dump_hex(di, s, s->ss_arr[0], s->es_arr[s->num_bits - 1]);
    oregon_level2(di, s);
}

static void oregon_v2(struct srd_decoder_inst *di, oregon_state *s)
{
    s->decode_pos = 0;
    /* Convert to v3 format - discard odd bits */
    /* Rebuild ookstring keeping only even-indexed bits */
    int new_len = 0;
    for (int i = 1; i < s->ook_len; i += 2) {
        s->ookstring[new_len++] = s->ookstring[i];
    }
    s->ookstring[new_len] = '\0';
    s->ook_len = new_len;

    /* Re-align: for odd-indexed decoded entries, use previous start pos */
    int new_bits = 0;
    for (int i = 0; i < s->num_bits; i++) {
        if (i % 2 == 1) {
            s->ss_arr[new_bits] = s->ss_arr[i - 1];
            s->es_arr[new_bits] = s->es_arr[i];
            s->bit_arr[new_bits] = s->bit_arr[i];
            new_bits++;
        }
    }
    s->num_bits = new_bits;

    oregon_v3(di, s);
}

static void oregon_decode(struct srd_decoder_inst *di, oregon_state *s)
{
    s->ook_len = 0;
    s->decode_pos = 0;

    /* Build ookstring */
    for (int i = 0; i < s->num_bits && s->ook_len < ORE_MAX_BITS - 1; i++)
        s->ookstring[s->ook_len++] = s->bit_arr[i];
    s->ookstring[s->ook_len] = '\0';

    if (s->ook_len < 17)
        return;

    /* Version detection */
    char *p;
    if ((p = strstr(s->ookstring, "10011001")) != NULL) {
        int preamble_len = (int)(p - s->ookstring);
        if (s->ook_len > preamble_len + 8 && preamble_len > 16) {
            s->ver = ORE_VER_V21;
            oregon_put_pre_and_sync(di, s, preamble_len, 8, "v2.1");
            oregon_v2(di, s);
            return;
        }
    }
    if ((p = strstr(s->ookstring, "E1100")) != NULL) {
        int preamble_len = (int)(p - s->ookstring);
        if (s->ook_len > preamble_len + 5 && preamble_len <= 12) {
            s->ver = ORE_VER_V1;
            oregon_put_pre_and_sync(di, s, preamble_len, 5, "v1");
            oregon_v1(di, s);
            return;
        }
    }
    if ((p = strstr(s->ookstring, "0101")) != NULL) {
        int preamble_len = (int)(p - s->ookstring);
        if (s->ook_len > preamble_len + 4 && preamble_len > 12) {
            s->ver = ORE_VER_V3;
            oregon_put_pre_and_sync(di, s, preamble_len, 4, "v3");
            oregon_v3(di, s);
            return;
        }
    }
    /* Not Oregon or wrong preamble */
    c_put(di, s->ss_arr[0], s->es_arr[s->num_bits - 1],
              s->out_ann, ANN_FIELD, "Not Oregon or wrong preamble");
}

static void ook_oregon_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    oregon_state *s = (oregon_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") == 0) {
        /* Parse bit list from data.
         * Format: each bit is 9 bytes:
         *   ss (4 bytes LE) + es (4 bytes LE) + bit_char (1 byte: '0','1','E')
         */
        int num_entries = (int)(n_fields / 9);
        s->num_bits = 0;
        s->ook_len = 0;
        s->decode_pos = 0;
        s->num_nibbles = 0;
        s->ver = ORE_VER_UNKNOWN;
        s->ver_str[0] = '\0';
        s->sensor_id[0] = '\0';

        /* Ensure capacity */
        if (num_entries > s->arr_capacity) {
            if (s->ss_arr) g_free(s->ss_arr);
            if (s->es_arr) g_free(s->es_arr);
            if (s->bit_arr) g_free(s->bit_arr);
            s->arr_capacity = num_entries + 256;
            s->ss_arr = (uint64_t *)g_malloc(s->arr_capacity * sizeof(uint64_t));
            s->es_arr = (uint64_t *)g_malloc(s->arr_capacity * sizeof(uint64_t));
            s->bit_arr = (char *)g_malloc(s->arr_capacity);
        }

        for (int i = 0; i < num_entries; i++) {
            const c_field *p = fields + i * 9;
            uint64_t ss = 0, es = 0;
            for (int j = 0; j < 8; j++) {
                ss |= ((uint64_t)p[j].u8) << (j * 8);
            }
            for (int j = 0; j < 8; j++) {
                es |= ((uint64_t)p[4 + j].u8) << (j * 8);
            }
            char bit_char = p[8].u8;
            s->ss_arr[i] = ss;
            s->es_arr[i] = es;
            s->bit_arr[i] = bit_char;
            s->num_bits++;
        }

        oregon_decode(di, s);
    }
}

static void ook_oregon_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        oregon_state *s = (oregon_state *)g_malloc0(sizeof(oregon_state));
        s->ss_arr = NULL;
        s->es_arr = NULL;
        s->bit_arr = NULL;
        s->arr_capacity = 0;
        c_decoder_set_private(di, s);
    }
    oregon_state *s = (oregon_state *)c_decoder_get_private(di);
    s->num_bits = 0;
    s->ook_len = 0;
    s->decode_pos = 0;
    s->num_nibbles = 0;
    s->ver = ORE_VER_UNKNOWN;
    s->ver_str[0] = '\0';
    s->sensor_id[0] = '\0';
    s->sensor_type = ORE_TYPE_UNKNOWN;
    s->unknown_type = ORE_TYPE_UNKNOWN;
}

static void ook_oregon_start(struct srd_decoder_inst *di)
{
    oregon_state *s = (oregon_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ook_oregon");

    const char *unknown = c_opt_str(di, "unknown", "Unknown");
    if (unknown) {
        if (strcmp(unknown, "Temp") == 0) s->unknown_type = ORE_TYPE_TEMP;
        else if (strcmp(unknown, "Temp_Hum") == 0) s->unknown_type = ORE_TYPE_TEMP_HUM;
        else if (strcmp(unknown, "Temp_Hum1") == 0) s->unknown_type = ORE_TYPE_TEMP_HUM1;
        else if (strcmp(unknown, "Temp_Hum_Baro") == 0) s->unknown_type = ORE_TYPE_TEMP_HUM_BARO;
        else if (strcmp(unknown, "Temp_Hum_Baro1") == 0) s->unknown_type = ORE_TYPE_TEMP_HUM_BARO1;
        else if (strcmp(unknown, "UV") == 0) s->unknown_type = ORE_TYPE_UV;
        else if (strcmp(unknown, "UV1") == 0) s->unknown_type = ORE_TYPE_UV1;
        else if (strcmp(unknown, "Wind") == 0) s->unknown_type = ORE_TYPE_WIND;
        else if (strcmp(unknown, "Rain") == 0) s->unknown_type = ORE_TYPE_RAIN;
        else if (strcmp(unknown, "Rain1") == 0) s->unknown_type = ORE_TYPE_RAIN1;
    }
}

static void ook_oregon_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ook_oregon_destroy(struct srd_decoder_inst *di)
{
    oregon_state *s = (oregon_state *)c_decoder_get_private(di);
    if (s) {
        if (s->ss_arr) g_free(s->ss_arr);
        if (s->es_arr) g_free(s->es_arr);
        if (s->bit_arr) g_free(s->bit_arr);
        g_free(s);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ook_oregon_c_decoder = {
    .id = "ook_oregon_c",
    .name = "Oregon(C)",
    .longname = "Oregon Scientific (C)",
    .desc = "Oregon Scientific weather sensor protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = oregon_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = oregon_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = oregon_ann_rows,
    .inputs = oregon_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = oregon_tags,
    .num_tags = 1,
    .reset = ook_oregon_reset,
    .start = ook_oregon_start,
    .decode = ook_oregon_decode,
    .destroy = ook_oregon_destroy,
    .decode_upper = ook_oregon_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    oregon_options[0].def = g_variant_new_string("Unknown");
    GSList *vals = NULL;
    vals = g_slist_append(vals, g_variant_new_string("Unknown"));
    vals = g_slist_append(vals, g_variant_new_string("Temp"));
    vals = g_slist_append(vals, g_variant_new_string("Temp_Hum"));
    vals = g_slist_append(vals, g_variant_new_string("Temp_Hum1"));
    vals = g_slist_append(vals, g_variant_new_string("Temp_Hum_Baro"));
    vals = g_slist_append(vals, g_variant_new_string("Temp_Hum_Baro1"));
    vals = g_slist_append(vals, g_variant_new_string("UV"));
    vals = g_slist_append(vals, g_variant_new_string("UV1"));
    vals = g_slist_append(vals, g_variant_new_string("Wind"));
    vals = g_slist_append(vals, g_variant_new_string("Rain"));
    vals = g_slist_append(vals, g_variant_new_string("Rain1"));
    oregon_options[0].values = vals;

    return &ook_oregon_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}