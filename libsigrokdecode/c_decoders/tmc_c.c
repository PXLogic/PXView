/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2025 C port (v4 API)
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

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_CLK 0
#define CH_DIO 1
#define CH_STB 2

enum tmc_state {
    STATE_FIND_START,
    STATE_FIND_DATA,
    STATE_FIND_ACK,
    STATE_FIND_STOP,
};

enum tmc_ann {
    ANN_START = 0,
    ANN_STOP,
    ANN_ACK,
    ANN_NACK,
    ANN_COMMAND,
    ANN_DATA,
    ANN_BIT,
    ANN_WARN,
    NUM_ANN,
};

/* Decoder private state — C_DECODER_STATE auto-generates tmc_s typedef,
 * tmc_reset (calloc), and tmc_destroy (free). */
C_DECODER_STATE(tmc, {
    int state;
    int bustype;       /* 0=WIRE2, 1=WIRE3 */
    int bitcount;
    uint8_t databyte;
    uint64_t ss_byte;
    uint64_t ss_ack;
    uint64_t ss;
    uint64_t es;
    uint64_t pdu_start;
    int pdu_bits;
    int bytecount;
    int radix;         /* 0=Hex, 1=Dec, 2=Oct, 3=Bin */
    uint64_t samplerate;
    int out_ann;
    int out_proto;
    int out_binary;
    int out_bitrate;
    struct { int val; uint64_t ss; uint64_t es; } bits[8];
})

static struct srd_channel tmc_channels[] = {
    { "clk", "CLK", "Clock line", 0, SRD_CHANNEL_SCLK, NULL },
    { "dio", "DIO", "Data line", 1, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_channel tmc_optional_channels[] = {
    { "stb", "STB", "Strobe line", 2, SRD_CHANNEL_COMMON, NULL },
};

static struct srd_decoder_option tmc_options[] = {
    { "radix", NULL, "Number format", NULL, NULL },
};

static const char *tmc_ann_labels[][3] = {
    { "", "Start", "S" },
    { "", "Stop", "P" },
    { "", "ACK", "A" },
    { "", "NACK", "N" },
    { "", "Command", "Cmd" },
    { "", "Data", "D" },
    { "", "Bit", "B" },
    { "", "Warnings", "Warn" },
};

static const int tmc_row_bits_classes[] = { ANN_BIT, -1 };
static const int tmc_row_data_classes[] = { ANN_START, ANN_STOP, ANN_ACK, ANN_NACK, ANN_COMMAND, ANN_DATA, -1 };
static const int tmc_row_warnings_classes[] = { ANN_WARN, -1 };

static const struct srd_c_ann_row tmc_ann_rows[] = {
    { "bits", "Bits", tmc_row_bits_classes, 1 },
    { "data", "Cmd/Data", tmc_row_data_classes, 6 },
    { "warnings", "Warnings", tmc_row_warnings_classes, 1 },
};

static const struct srd_decoder_binary tmc_binary[] = {
    { 0, "DATA", "D" },
};

static const char *tmc_inputs[] = { "logic" };
static const char *tmc_outputs[] = { "tmc" };
static const char *tmc_tags[] = { "Embedded/industrial" };

static void tmc_format_value(uint8_t val, int radix, char *out, int out_size)
{
    switch (radix) {
    case 0: snprintf(out, out_size, "0x%02X", val); break;
    case 1: snprintf(out, out_size, "%u", val); break;
    case 2: snprintf(out, out_size, "%o", val); break;
    case 3: {
        /* Match Python's format_data: {:b} — no leading zeros */
        char tmp[9];
        int pos = 0;
        int started = 0;
        for (int i = 7; i >= 0; i--) {
            int bit = (val >> i) & 1;
            if (bit || started || i == 0) {
                tmp[pos++] = bit ? '1' : '0';
                started = 1;
            }
        }
        tmp[pos] = '\0';
        snprintf(out, out_size, "%s", tmp);
        break;
    }
    default: snprintf(out, out_size, "0x%02X", val); break;
    }
}

static void tmc_clear_data(tmc_s *s)
{
    s->bitcount = 0;
    s->databyte = 0;
    memset(s->bits, 0, sizeof(s->bits));
}

static void tmc_handle_bitrate(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum)
{
    (void)samplenum;
    if (!s->samplerate || !s->pdu_start)
        return;
    double elapsed = 1.0 / (double)s->samplerate;
    elapsed *= (double)(samplenum - s->pdu_start - 1);
    if (elapsed > 0) {
        int bitrate = (int)(1.0 / elapsed * s->pdu_bits);
        c_put_meta_int(di, s->ss_byte, samplenum, s->out_bitrate, bitrate);
    }
}

static void tmc_handle_start(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum)
{
    (void)samplenum;
    s->ss = samplenum;
    s->es = samplenum;
    s->pdu_start = samplenum;
    s->pdu_bits = 0;
    s->bytecount = 0;
    c_proto(di, samplenum, samplenum, s->out_proto, "START", C_END);
    c_put(di, samplenum, samplenum, s->out_ann, ANN_START, "Start", "S");
    tmc_clear_data(s);
    s->state = STATE_FIND_DATA;
}

static void tmc_handle_data_wire2(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum, int dio)
{
    /* Insert bit at front, LSB-first */
    memmove(&s->bits[1], &s->bits[0], sizeof(s->bits[0]) * 7);
    s->bits[0].val = dio;
    s->bits[0].ss = samplenum;
    s->bits[0].es = samplenum;

    /* Register end sample of previous bit and display it */
    if (s->bitcount > 0) {
        s->bits[1].es = samplenum;
        if (s->bitcount <= 8) {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", s->bits[1].val);
            c_put(di, s->bits[1].ss, s->bits[1].es, s->out_ann, ANN_BIT, bit_str);
        }
    }

    s->bitcount++;
    if (s->bitcount <= 8) {
        s->databyte >>= 1;
        s->databyte |= (dio << 7);
        return;
    }

    /* Display data byte */
    s->ss = s->ss_byte;
    s->es = samplenum;
    int cmd = (s->bytecount == 0) ? ANN_COMMAND : ANN_DATA;

    /* Output BITS protocol */
    {
        unsigned char bits_data[1 + 8 * 17];
        int bpos = 0;
        bits_data[bpos++] = 8;
        for (int i = 7; i >= 0; i--) {
            bits_data[bpos++] = (unsigned char)s->bits[i].val;
            uint64_t bv = s->bits[i].ss;
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(bv >> (8 * b));
            bv = s->bits[i].es;
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(bv >> (8 * b));
        }
        c_proto(di, s->ss_byte, samplenum, s->out_proto, "BITS",
                C_BYTES(bits_data, bpos), C_END);
    }

    /* Output COMMAND/DATA protocol */
    c_proto(di, s->ss_byte, samplenum, s->out_proto,
        (cmd == ANN_COMMAND) ? "COMMAND" : "DATA", C_U8(s->databyte), C_END);

    /* Binary output */
    {
        unsigned char byte_data[1];
        byte_data[0] = s->databyte;
        c_put_bin(di, s->ss_byte, samplenum, s->out_binary, 0, 1, byte_data);
    }

    /* Annotation — match Python's compose_annot multi-format strings */
    {
        char val_str[32];
        tmc_format_value(s->databyte, s->radix, val_str, sizeof(val_str));
        if (cmd == ANN_COMMAND) {
            char long_str[64], mid_str[48], short_str[48];
            snprintf(long_str, sizeof(long_str), "Command: %s", val_str);
            snprintf(mid_str, sizeof(mid_str), "Cmd: %s", val_str);
            snprintf(short_str, sizeof(short_str), "C: %s", val_str);
            c_put_v(di, s->ss_byte, samplenum, s->out_ann, cmd,
                    s->databyte, long_str, mid_str, short_str, "Cmd", "C");
        } else {
            char long_str[64], mid_str[48];
            snprintf(long_str, sizeof(long_str), "Data: %s", val_str);
            snprintf(mid_str, sizeof(mid_str), "D: %s", val_str);
            c_put_v(di, s->ss_byte, samplenum, s->out_ann, cmd,
                    s->databyte, long_str, mid_str, "Data", "D");
        }
    }

    tmc_clear_data(s);
    s->ss_ack = samplenum;
    s->bytecount++;
    s->state = STATE_FIND_ACK;
}

static void tmc_handle_byte_wire3(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum)
{
    (void)samplenum;
    if (s->bitcount == 0)
        return;

    /* Update end sample of the last bit */
    s->bits[0].es = samplenum;

    /* Display all bits — match Python's reverse chronological order
     * (Python iterates self.bits from index 0=MSB/newest to N-1=LSB/oldest) */
    for (int i = 0; i < s->bitcount; i++) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->bits[i].val);
        c_put(di, s->bits[i].ss, s->bits[i].es, s->out_ann, ANN_BIT, bit_str);
    }

    /* Display data byte */
    s->ss = s->ss_byte;
    s->es = samplenum;
    int cmd = (s->bytecount == 0) ? ANN_COMMAND : ANN_DATA;

    /* Output BITS protocol */
    {
        unsigned char bits_data[1 + 8 * 17];
        int bpos = 0;
        bits_data[bpos++] = (unsigned char)s->bitcount;
        for (int i = s->bitcount - 1; i >= 0; i--) {
            bits_data[bpos++] = (unsigned char)s->bits[i].val;
            uint64_t bv = s->bits[i].ss;
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(bv >> (8 * b));
            bv = s->bits[i].es;
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(bv >> (8 * b));
        }
        c_proto(di, s->ss_byte, samplenum, s->out_proto, "BITS",
                C_BYTES(bits_data, bpos), C_END);
    }

    /* Output COMMAND/DATA protocol */
    c_proto(di, s->ss_byte, samplenum, s->out_proto,
        (cmd == ANN_COMMAND) ? "COMMAND" : "DATA", C_U8(s->databyte), C_END);

    /* Binary output */
    {
        unsigned char byte_data[1];
        byte_data[0] = s->databyte;
        c_put_bin(di, s->ss_byte, samplenum, s->out_binary, 0, 1, byte_data);
    }

    /* Annotation — match Python's compose_annot multi-format strings */
    {
        char val_str[32];
        tmc_format_value(s->databyte, s->radix, val_str, sizeof(val_str));
        if (cmd == ANN_COMMAND) {
            char long_str[64], mid_str[48], short_str[48];
            snprintf(long_str, sizeof(long_str), "Command: %s", val_str);
            snprintf(mid_str, sizeof(mid_str), "Cmd: %s", val_str);
            snprintf(short_str, sizeof(short_str), "C: %s", val_str);
            c_put_v(di, s->ss_byte, samplenum, s->out_ann, cmd,
                    s->databyte, long_str, mid_str, short_str, "Cmd", "C");
        } else {
            char long_str[64], mid_str[48];
            snprintf(long_str, sizeof(long_str), "Data: %s", val_str);
            snprintf(mid_str, sizeof(mid_str), "D: %s", val_str);
            c_put_v(di, s->ss_byte, samplenum, s->out_ann, cmd,
                    s->databyte, long_str, mid_str, "Data", "D");
        }
    }

    s->bytecount++;
}

static void tmc_handle_data_wire3(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum, int dio)
{
    if (s->bitcount >= 8) {
        tmc_handle_byte_wire3(di, s, samplenum);
        tmc_clear_data(s);
        s->ss_byte = samplenum;
    }

    memmove(&s->bits[1], &s->bits[0], sizeof(s->bits[0]) * 7);
    s->bits[0].val = dio;
    s->bits[0].ss = samplenum;
    s->bits[0].es = samplenum;

    s->databyte >>= 1;
    s->databyte |= (dio << 7);

    if (s->bitcount > 0)
        s->bits[1].es = samplenum;

    s->bitcount++;
}

static void tmc_handle_data(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum, int dio)
{
    s->pdu_bits++;
    if (s->bitcount == 0)
        s->ss_byte = samplenum;

    if (s->bustype == 0)
        tmc_handle_data_wire2(di, s, samplenum, dio);
    else
        tmc_handle_data_wire3(di, s, samplenum, dio);
}

static void tmc_handle_ack(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum, int dio)
{
    s->ss = s->ss_ack;
    s->es = samplenum;
    int cmd = dio ? ANN_NACK : ANN_ACK;
    c_proto(di, s->ss_ack, samplenum, s->out_proto,
        dio ? "NACK" : "ACK", C_END);
    c_put(di, s->ss_ack, samplenum, s->out_ann, cmd,
        dio ? "NACK" : "ACK", dio ? "N" : "A");
    s->state = STATE_FIND_DATA;
}

static void tmc_handle_stop(struct srd_decoder_inst *di, tmc_s *s, uint64_t samplenum)
{
    (void)samplenum;
    tmc_handle_bitrate(di, s, samplenum);

    if (s->bustype == 1) /* wire3: flush last byte */
        tmc_handle_byte_wire3(di, s, samplenum);

    /* Display stop */
    c_proto(di, samplenum, samplenum, s->out_proto, "STOP", C_END);
    c_put(di, samplenum, samplenum, s->out_ann, ANN_STOP, "Stop", "P");
    tmc_clear_data(s);
    s->state = STATE_FIND_START;
}

static void tmc_start(struct srd_decoder_inst *di)
{
    tmc_s *s = (tmc_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tmc");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "tmc");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "tmc");
    s->out_bitrate = c_reg_meta(di, SRD_OUTPUT_META,
        "tmc", "int", "Bitrate", "Bitrate from Start bit to Stop bit");

    const char *radix_str = c_opt_str(di, "radix", "Hex");
    if (strcmp(radix_str, "Dec") == 0)
        s->radix = 1;
    else if (strcmp(radix_str, "Oct") == 0)
        s->radix = 2;
    else if (strcmp(radix_str, "Bin") == 0)
        s->radix = 3;
    else
        s->radix = 0;
}

static void tmc_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    tmc_s *s = (tmc_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void tmc_decode(struct srd_decoder_inst *di)
{
    tmc_s *s = (tmc_s *)c_decoder_get_private(di);

    if (s->samplerate == 0)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0)
        return;

    /* Check required channels */
    if (!c_has_ch(di, CH_CLK) || !c_has_ch(di, CH_DIO))
        return;

    while (1) {
        if (s->state == STATE_FIND_START) {
            /* Wait for any of the START conditions:
             * WIRE3: CLK = high, STB = falling  OR  CLK = low, STB = falling
             * WIRE2: CLK = high, DIO = falling */
            int ret = c_wait(di,
                CW_H(CH_CLK), CW_F(CH_STB), CW_OR,
                CW_L(CH_CLK), CW_F(CH_STB), CW_OR,
                CW_H(CH_CLK), CW_F(CH_DIO), CW_END);
            if (ret != SRD_OK) return;

            uint64_t matched = di_matched(di);
            uint64_t samplenum = di_samplenum(di);

            if ((matched & 0b011) || (matched & 0b010)) {
                /* condition 0 or 1: wire3 */
                s->bustype = 1;
                tmc_handle_start(di, s, samplenum);
            } else if (matched & 0b100) {
                /* condition 2: wire2 */
                s->bustype = 0;
                tmc_handle_start(di, s, samplenum);
            }
        } else if (s->state == STATE_FIND_DATA) {
            /* Wait for any of the following conditions:
             * WIRE3 STOP: STB = rising
             * WIRE2 STOP: CLK = high, DIO = rising
             * Clock pulse: CLK = rising */
            int ret = c_wait(di,
                CW_R(CH_STB), CW_OR,
                CW_H(CH_CLK), CW_R(CH_DIO), CW_OR,
                CW_R(CH_CLK), CW_END);
            if (ret != SRD_OK) return;

            uint64_t matched = di_matched(di);
            uint64_t samplenum = di_samplenum(di);

            if ((matched & 0b1) || (matched & 0b10)) {
                /* STOP condition */
                tmc_handle_stop(di, s, samplenum);
            } else if (matched & 0b100) {
                /* CLK rising edge: data */
                int dio = c_pin(di, CH_DIO);
                tmc_handle_data(di, s, samplenum, dio);
            }
        } else if (s->state == STATE_FIND_ACK) {
            /* Wait for an ACK bit */
            int ret = c_wait(di, CW_F(CH_CLK), CW_END);
            if (ret != SRD_OK) return;

            uint64_t samplenum = di_samplenum(di);
            int dio = c_pin(di, CH_DIO);
            tmc_handle_ack(di, s, samplenum, dio);
        } else if (s->state == STATE_FIND_STOP) {
            /* Wait for STOP conditions:
             * WIRE3 STOP: STB = rising
             * WIRE2 STOP: CLK = high, DIO = rising */
            int ret = c_wait(di,
                CW_R(CH_STB), CW_OR,
                CW_H(CH_CLK), CW_R(CH_DIO), CW_END);
            if (ret != SRD_OK) return;

            uint64_t matched = di_matched(di);
            uint64_t samplenum = di_samplenum(di);

            if ((matched & 0b1) || (matched & 0b10))
                tmc_handle_stop(di, s, samplenum);
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder tmc_c_def = {
    .id = "tmc_c",
    .name = "TMC(C)",
    .longname = "Titan Micro Circuit(C)",
    .desc = "Bus for TM1636/37/38 7-segment digital tubes.(C implementation)",
    .license = "gplv2+",
    .channels = tmc_channels,
    .num_channels = 2,
    .optional_channels = tmc_optional_channels,
    .num_optional_channels = 1,
    .options = tmc_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = tmc_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = tmc_ann_rows,
    .inputs = tmc_inputs,
    .num_inputs = 1,
    .outputs = tmc_outputs,
    .num_outputs = 1,
    .binary = tmc_binary,
    .num_binary = 1,
    .tags = tmc_tags,
    .num_tags = 1,
    .state_size = sizeof(tmc_s),
    .reset = tmc_reset,
    .start = tmc_start,
    .decode = tmc_decode,
    .end = NULL,
    .metadata = tmc_metadata,
    .destroy = tmc_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    tmc_options[0].def = g_variant_new_string("Hex");
    GSList *radix_vals = NULL;
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Hex"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Dec"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Oct"));
    radix_vals = g_slist_append(radix_vals, g_variant_new_string("Bin"));
    tmc_options[0].values = radix_vals;
    return &tmc_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}