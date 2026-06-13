/*
 * This file is part of the libsigrokdecode project.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_RESERVED = 0,
    ANN_WRITE,
    ANN_READ,
    ANN_DATA,
    ANN_DISPLAY,
    ANN_ADDRESS,
    ANN_AUTO,
    ANN_FIXED,
    ANN_NORMAL,
    ANN_TEST,
    ANN_DIGIT,
    ANN_CONTRAST,
    ANN_OFF,
    ANN_ON,
    ANN_WARN,
    ANN_DISPLAY_INFO,
    NUM_ANN,
};

#define TM1637_MAX_DISPLAY 8
#define TM1637_MAX_BITS 8

/* Command register addresses (bits 6-7) */
#define CMD_DATA    0x40
#define CMD_DISPLAY 0x80
#define CMD_ADDRESS 0xC0

/* Data bits positions */
#define DATA_RW   1
#define DATA_ADDR 2
#define DATA_MODE 3

/* Display bits positions */
#define DISP_MIN   0
#define DISP_MAX   2
#define DISP_SWITCH 3

/* Address bits positions */
#define ADDR_MIN 0
#define ADDR_MAX 2

static const char *contrasts[] = {
    "1/16", "2/16", "4/16", "10/16",
    "11/16", "12/16", "13/16", "14/16"
};

static const char *segments[] = {
    "a", "b", "c", "d", "e", "f", "g", "dp"
};

typedef struct {
    uint8_t segs;
    char ch;
} font_entry;

static const font_entry font_table[] = {
    {0b0000000, ' '},
    {0b0100000, '\''},
    {0b1000000, '-'},
    {0b0111111, '0'},
    {0b0000110, '1'},
    {0b1011011, '2'},
    {0b1001111, '3'},
    {0b1100110, '4'},
    {0b1101101, '5'},
    {0b1111101, '6'},
    {0b0000111, '7'},
    {0b1111111, '8'},
    {0b1101111, '9'},
    {0b1110111, 'A'},
    {0b1111100, 'b'},
    {0b0111001, 'C'},
    {0b1011110, 'd'},
    {0b1111001, 'E'},
    {0b1110001, 'F'},
    {0b1110110, 'H'},
    {0b0110000, 'I'},
    {0b0001110, 'J'},
    {0b0111000, 'L'},
    {0b1010100, 'n'},
    {0b1011100, 'o'},
    {0b1110011, 'P'},
    {0b1010000, 'r'},
    {0b1111000, 't'},
    {0b0111110, 'U'},
    {0b0001111, ']'},
    {0b0001000, '_'},
    {0b1011000, 'c'},
    {0b1110100, 'h'},
    {0b0010000, 'i'},
    {0b0011100, 'u'},
};
#define FONT_TABLE_SIZE (sizeof(font_table) / sizeof(font_table[0]))

enum {
    STATE_IDLE,
    STATE_REG_CMD,
    STATE_REG_DATA,
};

typedef struct {
    int out_ann;
    /* State */
    int state;
    /* Bit cache */
    uint64_t bit_ss[TM1637_MAX_BITS];
    uint64_t bit_es[TM1637_MAX_BITS];
    int bit_val[TM1637_MAX_BITS];
    int num_bits;
    /* Command state */
    int is_write;
    int is_auto;
    int position;
    /* Display buffer */
    char display[TM1637_MAX_DISPLAY * 2 + 1];
    int display_len;
    /* Options */
    int dpoint_is_colon;
    /* Transmission start */
    uint64_t ssb;
    uint64_t ss;
    uint64_t es;
} tm1637_state;

static const char *tm1637_inputs[] = {"tmc", NULL};
static const char *tm1637_outputs[] = {"tm1637", NULL};
static const char *tm1637_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option tm1637_options[] = {
    {"dpoint", "dec_tm1637_opt_dpoint", "Decimal point", NULL, NULL},
};

static const char *tm1637_ann_labels[][3] = {
    {"", "reserved", "Reserved"},
    {"", "write", "Write"},
    {"", "read", "Read"},
    {"", "data", "Data command"},
    {"", "display", "Display command"},
    {"", "address", "Address command"},
    {"", "auto", "AutoAddr"},
    {"", "fixed", "FixedAddr"},
    {"", "normal", "Normal"},
    {"", "test", "Test"},
    {"", "digit", "Digit"},
    {"", "contrast", "Contrast"},
    {"", "off", "OFF"},
    {"", "on", "ON"},
    {"", "warnings", "Warnings"},
    {"", "display-info", "Tubes"},
};

static const int tm1637_row_bits_classes[] = {
    ANN_RESERVED, ANN_WRITE, ANN_READ, ANN_DATA, ANN_DISPLAY, ANN_ADDRESS,
    ANN_AUTO, ANN_FIXED, ANN_NORMAL, ANN_TEST, ANN_DIGIT, ANN_CONTRAST,
    ANN_OFF, ANN_ON, -1
};
static const int tm1637_row_display_classes[] = {ANN_DISPLAY_INFO, -1};
static const int tm1637_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row tm1637_ann_rows[] = {
    {"bits", "Bits", tm1637_row_bits_classes, 14},
    {"display", "Display", tm1637_row_display_classes, 1},
    {"warnings", "Warnings", tm1637_row_warnings_classes, 1},
};

static void tm1637_putd(struct srd_decoder_inst *di, tm1637_state *s,
    int bit_start, int bit_end, int cls, const char *text)
{
    if (bit_start < 0 || bit_end < 0 || bit_start >= s->num_bits || bit_end >= s->num_bits)
        return;
    c_put(di, s->bit_ss[bit_start], s->bit_es[bit_end], s->out_ann, cls, text);
}

static void tm1637_putr(struct srd_decoder_inst *di, tm1637_state *s,
    int bit_start, int bit_end)
{
    for (int i = bit_start; i <= bit_end && i < s->num_bits; i++)
        c_put(di, s->bit_ss[i], s->bit_es[i], s->out_ann, ANN_RESERVED, "Reserved");
}

static char tm1637_font_lookup(uint8_t segs)
{
    for (int i = 0; i < (int)FONT_TABLE_SIZE; i++) {
        if (font_table[i].segs == segs)
            return font_table[i].ch;
    }
    return '?';
}

static void tm1637_handle_command(struct srd_decoder_inst *di, tm1637_state *s, uint8_t data)
{
    uint8_t cmd = data & 0xC0;
    uint8_t rest = data & ~0xC0;

    if (cmd == CMD_DATA) {
        /* Bits row - Command bits */
        tm1637_putd(di, s, 6, 7, ANN_DATA, "Data command");
        /* Reserved bits 4-5 */
        tm1637_putr(di, s, DATA_MODE + 1, 5);
        /* Mode bit */
        int mode = (rest >> DATA_MODE) & 1;
        tm1637_putd(di, s, DATA_MODE, DATA_MODE,
                    mode ? ANN_TEST : ANN_NORMAL,
                    mode ? "Test" : "Normal");
        /* Addressing bit */
        int addr_mode = (rest >> DATA_ADDR) & 1;
        s->is_auto = !addr_mode;
        tm1637_putd(di, s, DATA_ADDR, DATA_ADDR,
                    addr_mode ? ANN_FIXED : ANN_AUTO,
                    addr_mode ? "FixedAddr" : "AutoAddr");
        /* Read/Write bit */
        int rw = (rest >> DATA_RW) & 1;
        s->is_write = !rw;
        tm1637_putd(di, s, DATA_RW, DATA_RW,
                    rw ? ANN_READ : ANN_WRITE,
                    rw ? "Read" : "Write");
        /* Prohibited bit 0 */
        tm1637_putr(di, s, 0, DATA_RW - 1);
    } else if (cmd == CMD_DISPLAY) {
        /* Bits row - Command bits */
        tm1637_putd(di, s, 6, 7, ANN_DISPLAY, "Display command");
        /* Reserved bits 4-5 */
        tm1637_putr(di, s, DISP_SWITCH + 1, 5);
        /* Switch bit */
        int sw = (rest >> DISP_SWITCH) & 1;
        tm1637_putd(di, s, DISP_SWITCH, DISP_SWITCH,
                    sw ? ANN_ON : ANN_OFF,
                    sw ? "ON" : "OFF");
        /* PWM bits */
        int pwm = rest & 0x07;
        char buf[32];
        snprintf(buf, sizeof(buf), "Contrast %s", contrasts[pwm]);
        tm1637_putd(di, s, DISP_MIN, DISP_MAX, ANN_CONTRAST, buf);
    } else if (cmd == CMD_ADDRESS) {
        /* Bits row - Command bits */
        tm1637_putd(di, s, 6, 7, ANN_ADDRESS, "Address command");
        /* Reserved bits 3-5 */
        tm1637_putr(di, s, ADDR_MAX + 1, 5);
        /* Digit bits */
        int adr = (rest & 0x07) + 1;
        s->position = adr;
        char buf[32];
        snprintf(buf, sizeof(buf), "Digit %d", adr);
        tm1637_putd(di, s, ADDR_MIN, ADDR_MAX, ANN_DIGIT, buf);
    }
}

static void tm1637_handle_data(struct srd_decoder_inst *di, tm1637_state *s, uint8_t data)
{
    /* Active segments bits */
    for (int i = 0; i < 8; i++) {
        if (data >> i & 1) {
            tm1637_putd(di, s, i, i, ANN_DIGIT, segments[i]);
        }
    }

    /* Register digit */
    uint8_t mask = data & ~(1 << 7);
    char ch = tm1637_font_lookup(mask);
    char dp[2] = {'\0', '\0'};
    if ((data >> 7) & 1)
        dp[0] = s->dpoint_is_colon ? ':' : '.';

    if (s->display_len < TM1637_MAX_DISPLAY * 2) {
        s->display[s->display_len++] = ch;
        if (dp[0])
            s->display[s->display_len++] = dp[0];
        s->display[s->display_len] = '\0';
    }

    if (s->is_auto)
        s->position++;
}

static void tm1637_handle_info(struct srd_decoder_inst *di, tm1637_state *s)
{
    if (s->display_len > 0) {
        char buf[TM1637_MAX_DISPLAY * 2 + 16];
        snprintf(buf, sizeof(buf), "Tubes: %s", s->display);
        c_put(di, s->ssb, s->es, s->out_ann, ANN_DISPLAY_INFO, buf);
    }
    s->display_len = 0;
    s->display[0] = '\0';
}

static void tm1637_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tm1637_state *s = (tm1637_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "BITS") == 0) {
        /* BITS v2 format from tmc_c:
           fields[0].u8 = flags, fields[1].u8 = bit_count,
           then per bit: [value(1B)][ss(8B LE)][es(8B LE)] */
        if (n_fields < 2)
            return;
        uint8_t flags = fields[0].u8;
        (void)flags;
        uint8_t bit_count = fields[1].u8;
        if (bit_count > TM1637_MAX_BITS)
            bit_count = TM1637_MAX_BITS;
        s->num_bits = 0;
        const c_field *p = fields + 2;
        for (int i = 0; i < bit_count && (p + 17) <= fields + n_fields; i++, p += 17) {
            s->bit_val[i] = p[0].u8;
            memcpy(&s->bit_ss[i], p + 1, 8);
            memcpy(&s->bit_es[i], p + 9, 8);
            s->num_bits++;
        }
    } else if (strcmp(cmd, "START") == 0) {
        s->ssb = start_sample;
        s->state = STATE_REG_CMD;
    } else if (strcmp(cmd, "COMMAND") == 0) {
        uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
        tm1637_handle_command(di, s, databyte);
        s->state = STATE_REG_DATA;
    } else if (strcmp(cmd, "DATA") == 0) {
        uint8_t databyte = (n_fields > 0) ? fields[0].u8 : 0;
        tm1637_handle_data(di, s, databyte);
    } else if (strcmp(cmd, "STOP") == 0) {
        tm1637_handle_info(di, s);
        s->state = STATE_IDLE;
    }
}

static void tm1637_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tm1637_state)));
    }
    tm1637_state *s = (tm1637_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tm1637_state));
    s->state = STATE_IDLE;
    s->dpoint_is_colon = 0;
}

static void tm1637_start(struct srd_decoder_inst *di)
{
    tm1637_state *s = (tm1637_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tm1637");

    const char *dpoint = c_opt_str(di, "dpoint", "Dot");
    s->dpoint_is_colon = (dpoint && strcmp(dpoint, "Colon") == 0) ? 1 : 0;
}

static void tm1637_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tm1637_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder tm1637_c_decoder = {
    .id = "tm1637_c",
    .name = "TM1637(C)",
    .longname = "LED drive control special circuit (C)",
    .desc = "Titan Micro Electronics LED drive control special circuit for driving displays with 7-segments digital tubes. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = tm1637_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = tm1637_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = tm1637_ann_rows,
    .inputs = tm1637_inputs,
    .num_inputs = 1,
    .outputs = tm1637_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = tm1637_tags,
    .num_tags = 1,
    .reset = tm1637_reset,
    .start = tm1637_start,
    .decode = tm1637_decode,
    .destroy = tm1637_destroy,
    .decode_upper = tm1637_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    tm1637_options[0].def = g_variant_new_string("Dot");
    GSList *dpoint_vals = NULL;
    dpoint_vals = g_slist_append(dpoint_vals, g_variant_new_string("Dot"));
    dpoint_vals = g_slist_append(dpoint_vals, g_variant_new_string("Colon"));
    tm1637_options[0].values = dpoint_vals;

    return &tm1637_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}