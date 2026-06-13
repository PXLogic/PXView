/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 Benjamin Larsson <benjamin@southpole.se>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum em4305_ann {
    ANN_BIT_VALUE = 0,
    ANN_FIRST_FIELD_STOP,
    ANN_WRITE_GAP,
    ANN_WRITE_MODE_EXIT,
    ANN_BIT,
    ANN_OPCODE,
    ANN_LOCK,
    ANN_DATA,
    ANN_PASSWORD,
    ANN_ADDRESS,
    ANN_BITRATE,
    NUM_ANN,
};

enum em4305_state {
    EM_FFS_SEARCH,
    EM_FFS_DETECTED,
    EM_SKIP,
};

typedef struct {
    uint64_t samplerate;
    uint64_t last_samplenum;
    uint64_t oldsamplenum;
    uint64_t old_gap_end;
    int gap_detected;
    int bit_nr;
    int state;

    double field_clock;
    double wzmax;
    double wzmin;
    double womax;
    double ffs;
    double writegap;
    double nogap;

    /* bits_pos: [70][3] — [bit_value, ss, es] */
    int bits_pos_val[70];
    uint64_t bits_pos_ss[70];
    uint64_t bits_pos_es[70];

    int em4100_decode1_partial;
    int out_ann;
    int initialized;
} em4305_state;

static const char *br_string[] = { "RF/8", "RF/16", "Unused", "RF/32", "RF/40", "Unused", "Unused", "RF/64" };
static const char *encoder_str[] = { "not used", "Manchester", "Bi-phase", "not used" };
static const char *delayed_on_str[] = { "No delay", "Delayed on - BP/8", "Delayed on - BP/4", "No delay" };
static const char *cmds[] = { "Invalid", "Login", "Write word", "Invalid", "Read word", "Disable", "Protect", "Invalid" };

static struct srd_channel em4305_channels[] = {
    { "data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_em4305_chan_data" },
};

static struct srd_decoder_option em4305_options[] = {
    { "coilfreq", "dec_em4305_opt_coilfreq", "Coil frequency", NULL, NULL },
    { "first_field_stop", "dec_em4305_opt_first_field_stop", "First field stop min", NULL, NULL },
    { "w_gap", "dec_em4305_opt_w_gap", "Write gap min", NULL, NULL },
    { "w_one_max", "dec_em4305_opt_w_one_max", "Write one max", NULL, NULL },
    { "w_zero_on_min", "dec_em4305_opt_w_zero_on_min", "Write zero on min", NULL, NULL },
    { "w_zero_off_max", "dec_em4305_opt_w_zero_off_max", "Write zero off max", NULL, NULL },
    { "em4100_decode", "dec_em4305_opt_em4100_decode", "EM4100 decode", NULL, NULL },
};

static const char* em4305_ann_labels[][3] = {
    { "", "Bit value", "Bit value" },
    { "", "First field stop", "First field stop" },
    { "", "Write gap", "Write gap" },
    { "", "Write mode exit", "Write mode exit" },
    { "", "Bit", "Bit" },
    { "", "Opcode", "Opcode" },
    { "", "Lock", "Lock" },
    { "", "Data", "Data" },
    { "", "Password", "Password" },
    { "", "Address", "Address" },
    { "", "Bitrate", "Bitrate" },
};

static const int em4305_row_bits_classes[] = { ANN_BIT_VALUE, -1 };
static const int em4305_row_structure_classes[] = { ANN_FIRST_FIELD_STOP, ANN_WRITE_GAP, ANN_WRITE_MODE_EXIT, ANN_BIT, -1 };
static const int em4305_row_fields_classes[] = { ANN_OPCODE, ANN_LOCK, ANN_DATA, ANN_PASSWORD, ANN_ADDRESS, -1 };
static const int em4305_row_decode_classes[] = { ANN_BITRATE, -1 };

static const struct srd_c_ann_row em4305_ann_rows[] = {
    { "bits", "Bits", em4305_row_bits_classes, 1 },
    { "structure", "Structure", em4305_row_structure_classes, 4 },
    { "fields", "Fields", em4305_row_fields_classes, 5 },
    { "decode", "Decode", em4305_row_decode_classes, 1 },
};

static const char* em4305_inputs[] = { "logic" };
static const char* em4305_tags[] = { "IC", "RFID" };

static int em4305_get_8_bits(em4305_state *s, int idx)
{
    int retval = 0;
    for (int i = 0; i < 8; i++) {
        retval <<= 1;
        if (idx + i < 70)
            retval |= s->bits_pos_val[idx + i];
    }
    return retval;
}

static int em4305_get_32_bits(em4305_state *s, int idx)
{
    return (em4305_get_8_bits(s, idx + 27) << 24) |
           (em4305_get_8_bits(s, idx + 18) << 16) |
           (em4305_get_8_bits(s, idx + 9) << 8) |
           em4305_get_8_bits(s, idx);
}

static int em4305_get_3_bits(em4305_state *s, int idx)
{
    return (s->bits_pos_val[idx] << 2) |
           (s->bits_pos_val[idx + 1] << 1) |
           s->bits_pos_val[idx + 2];
}

static int em4305_get_4_bits(em4305_state *s, int idx)
{
    return (s->bits_pos_val[idx] << 0) |
           (s->bits_pos_val[idx + 1] << 1) |
           (s->bits_pos_val[idx + 2] << 2) |
           (s->bits_pos_val[idx + 3] << 3);
}

static void em4305_print_row_parity(struct srd_decoder_inst *di, em4305_state *s, int idx, int length)
{
    int parity = 0;
    for (int i = 0; i < length; i++)
        parity += s->bits_pos_val[i + idx];
    parity = parity & 0x1;
    if (idx + length < 70) {
        if (parity == s->bits_pos_val[idx + length]) {
            c_put(di, s->bits_pos_ss[idx + length], s->bits_pos_es[idx + length],
                      s->out_ann, ANN_OPCODE, "Row parity OK", "Parity OK", "OK");
        } else {
            c_put(di, s->bits_pos_ss[idx + length], s->bits_pos_es[idx + length],
                      s->out_ann, ANN_OPCODE, "Row parity failed", "Parity failed", "Fail");
        }
    }
}

static void em4305_print_col_parity(struct srd_decoder_inst *di, em4305_state *s, int idx)
{
    int data_1 = em4305_get_8_bits(s, idx);
    int data_2 = em4305_get_8_bits(s, idx + 9);
    int data_3 = em4305_get_8_bits(s, idx + 9 + 9);
    int data_4 = em4305_get_8_bits(s, idx + 9 + 9 + 9);
    int col_par = em4305_get_8_bits(s, idx + 9 + 9 + 9 + 9);
    int col_par_calc = data_1 ^ data_2 ^ data_3 ^ data_4;

    int cp_idx = idx + 9 + 9 + 9 + 9;
    if (cp_idx + 7 < 70) {
        if (col_par == col_par_calc) {
            c_put(di, s->bits_pos_ss[cp_idx], s->bits_pos_es[cp_idx + 7],
                      s->out_ann, ANN_OPCODE, "Column parity OK", "Parity OK", "OK");
        } else {
            c_put(di, s->bits_pos_ss[cp_idx], s->bits_pos_es[cp_idx + 7],
                      s->out_ann, ANN_OPCODE, "Column parity failed", "Parity failed", "Fail");
        }
    }
}

static void em4305_print_8bit_data(struct srd_decoder_inst *di, em4305_state *s, int idx)
{
    int data = em4305_get_8_bits(s, idx);
    char buf[64], short_buf[32];
    snprintf(buf, sizeof(buf), "Data: %X", data);
    snprintf(short_buf, sizeof(short_buf), "%X", data);
    if (idx + 7 < 70)
        c_put(di, s->bits_pos_ss[idx], s->bits_pos_es[idx + 7], s->out_ann, ANN_ADDRESS, buf, short_buf);
}

static void em4305_put4bits(struct srd_decoder_inst *di, em4305_state *s, int idx)
{
    int bits = (s->bits_pos_val[idx] << 3) | (s->bits_pos_val[idx + 1] << 2) |
               (s->bits_pos_val[idx + 2] << 1) | s->bits_pos_val[idx + 3];
    char buf[16];
    snprintf(buf, sizeof(buf), "%X", bits);
    if (idx + 3 < 70)
        c_put(di, s->bits_pos_ss[idx], s->bits_pos_es[idx + 3], s->out_ann, ANN_BITRATE, buf);
}

static void em4305_decode_config(struct srd_decoder_inst *di, em4305_state *s, int idx)
{
    /* Bitrate */
    int bitrate = em4305_get_3_bits(s, idx + 2);
    if (bitrate < 8 && idx + 5 < 70) {
        char buf[128], short_buf[64];
        snprintf(buf, sizeof(buf), "Data rate: %s", br_string[bitrate]);
        snprintf(short_buf, sizeof(short_buf), "%s", br_string[bitrate]);
        c_put(di, s->bits_pos_ss[idx], s->bits_pos_es[idx + 5], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Encoder */
    int encoding = (s->bits_pos_val[idx + 6] << 0) | (s->bits_pos_val[idx + 7] << 1);
    if (encoding < 4 && idx + 10 < 70) {
        char buf[128], short_buf[64];
        snprintf(buf, sizeof(buf), "Encoder: %s", encoder_str[encoding]);
        snprintf(short_buf, sizeof(short_buf), "%s", encoder_str[encoding]);
        c_put(di, s->bits_pos_ss[idx + 6], s->bits_pos_es[idx + 10], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Zero bits */
    if (idx + 12 < 70)
        c_put(di, s->bits_pos_ss[idx + 11], s->bits_pos_es[idx + 12], s->out_ann, ANN_BITRATE, "Zero bits", "ZB");

    /* Delayed on */
    int delay_on = (s->bits_pos_val[idx + 13] << 0) | (s->bits_pos_val[idx + 14] << 1);
    if (delay_on < 4 && idx + 14 < 70) {
        char buf[128], short_buf[64];
        snprintf(buf, sizeof(buf), "Delayed on: %s", delayed_on_str[delay_on]);
        snprintf(short_buf, sizeof(short_buf), "%s", delayed_on_str[delay_on]);
        c_put(di, s->bits_pos_ss[idx + 13], s->bits_pos_es[idx + 14], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Last default read word */
    int lwr = (s->bits_pos_val[idx + 15] << 3) | (s->bits_pos_val[idx + 16] << 2) |
              (s->bits_pos_val[idx + 18] << 1) | (s->bits_pos_val[idx + 19] << 0);
    if (idx + 19 < 70) {
        char buf[128], short_buf[32], tiny_buf[16];
        snprintf(buf, sizeof(buf), "Last default read word: %d", lwr);
        snprintf(short_buf, sizeof(short_buf), "LWR: %d", lwr);
        snprintf(tiny_buf, sizeof(tiny_buf), "%d", lwr);
        c_put(di, s->bits_pos_ss[idx + 15], s->bits_pos_es[idx + 19], s->out_ann, ANN_BITRATE, buf, short_buf, tiny_buf);
    }

    /* Read login */
    if (idx + 20 < 70) {
        char buf[64], short_buf[16];
        snprintf(buf, sizeof(buf), "Read login: %d", s->bits_pos_val[idx + 20]);
        snprintf(short_buf, sizeof(short_buf), "%d", s->bits_pos_val[idx + 20]);
        c_put(di, s->bits_pos_ss[idx + 20], s->bits_pos_es[idx + 20], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Zero bits */
    if (idx + 21 < 70)
        c_put(di, s->bits_pos_ss[idx + 21], s->bits_pos_es[idx + 21], s->out_ann, ANN_BITRATE, "Zero bits", "ZB");

    /* Write login */
    if (idx + 22 < 70) {
        char buf[64], short_buf[16];
        snprintf(buf, sizeof(buf), "Write login: %d", s->bits_pos_val[idx + 22]);
        snprintf(short_buf, sizeof(short_buf), "%d", s->bits_pos_val[idx + 22]);
        c_put(di, s->bits_pos_ss[idx + 22], s->bits_pos_es[idx + 22], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Zero bits */
    if (idx + 24 < 70)
        c_put(di, s->bits_pos_ss[idx + 23], s->bits_pos_es[idx + 24], s->out_ann, ANN_BITRATE, "Zero bits", "ZB");

    /* Disable */
    if (idx + 25 < 70) {
        char buf[64], short_buf[16];
        snprintf(buf, sizeof(buf), "Disable: %d", s->bits_pos_val[idx + 25]);
        snprintf(short_buf, sizeof(short_buf), "%d", s->bits_pos_val[idx + 25]);
        c_put(di, s->bits_pos_ss[idx + 25], s->bits_pos_es[idx + 25], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Reader talk first */
    if (idx + 27 < 70) {
        char buf[64], short_buf[16];
        snprintf(buf, sizeof(buf), "Reader talk first: %d", s->bits_pos_val[idx + 27]);
        snprintf(short_buf, sizeof(short_buf), "RTF: %d", s->bits_pos_val[idx + 27]);
        c_put(di, s->bits_pos_ss[idx + 27], s->bits_pos_es[idx + 27], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Zero bits */
    if (idx + 28 < 70)
        c_put(di, s->bits_pos_ss[idx + 28], s->bits_pos_es[idx + 28], s->out_ann, ANN_BITRATE, "Zero bits", "ZB");

    /* Pigeon mode */
    if (idx + 29 < 70) {
        char buf[64], short_buf[16];
        snprintf(buf, sizeof(buf), "Pigeon mode: %d", s->bits_pos_val[idx + 29]);
        snprintf(short_buf, sizeof(short_buf), "%d", s->bits_pos_val[idx + 29]);
        c_put(di, s->bits_pos_ss[idx + 29], s->bits_pos_es[idx + 29], s->out_ann, ANN_BITRATE, buf, short_buf);
    }

    /* Reserved */
    if (idx + 34 < 70)
        c_put(di, s->bits_pos_ss[idx + 30], s->bits_pos_es[idx + 34], s->out_ann, ANN_BITRATE, "Reserved", "Res", "R");
}

static void em4305_em4100_decode1(struct srd_decoder_inst *di, em4305_state *s, int idx)
{
    if (idx + 9 < 70)
        c_put(di, s->bits_pos_ss[idx], s->bits_pos_es[idx + 9], s->out_ann, ANN_BITRATE, "EM4100 header", "EM header", "Header", "H");
    em4305_put4bits(di, s, idx + 10);

    int bits = (s->bits_pos_val[idx + 15] << 3) | (s->bits_pos_val[idx + 16] << 2) |
               (s->bits_pos_val[idx + 18] << 1) | (s->bits_pos_val[idx + 19] << 0);
    if (idx + 19 < 70) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%X", bits);
        c_put(di, s->bits_pos_ss[idx + 15], s->bits_pos_es[idx + 19], s->out_ann, ANN_BITRATE, buf);
    }

    em4305_put4bits(di, s, idx + 21);
    em4305_put4bits(di, s, idx + 27);

    s->em4100_decode1_partial = (s->bits_pos_val[idx + 32] << 3) |
                                 (s->bits_pos_val[idx + 33] << 2) |
                                 (s->bits_pos_val[idx + 34] << 1);
    if (idx + 34 < 70)
        c_put(di, s->bits_pos_ss[idx + 32], s->bits_pos_es[idx + 34], s->out_ann, ANN_BITRATE, "Partial nibble");
}

static void em4305_em4100_decode2(struct srd_decoder_inst *di, em4305_state *s, int idx)
{
    if (s->em4100_decode1_partial != 0) {
        int bits = s->em4100_decode1_partial + s->bits_pos_val[idx];
        char buf[16];
        snprintf(buf, sizeof(buf), "%X", bits);
        if (idx < 70)
            c_put(di, s->bits_pos_ss[idx], s->bits_pos_es[idx], s->out_ann, ANN_BITRATE, buf);
        s->em4100_decode1_partial = 0;
    } else {
        if (idx < 70)
            c_put(di, s->bits_pos_ss[idx], s->bits_pos_es[idx], s->out_ann, ANN_BITRATE, "Partial nibble");
    }

    em4305_put4bits(di, s, idx + 2);

    int bits = (s->bits_pos_val[idx + 7] << 3) | (s->bits_pos_val[idx + 9] << 2) |
               (s->bits_pos_val[idx + 10] << 1) | (s->bits_pos_val[idx + 11] << 0);
    if (idx + 11 < 70) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%X", bits);
        c_put(di, s->bits_pos_ss[idx + 7], s->bits_pos_es[idx + 11], s->out_ann, ANN_BITRATE, buf);
    }

    em4305_put4bits(di, s, idx + 13);
    em4305_put4bits(di, s, idx + 19);

    bits = (s->bits_pos_val[idx + 24] << 3) | (s->bits_pos_val[idx + 25] << 2) |
           (s->bits_pos_val[idx + 27] << 1) | (s->bits_pos_val[idx + 28] << 0);
    if (idx + 28 < 70) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%X", bits);
        c_put(di, s->bits_pos_ss[idx + 24], s->bits_pos_es[idx + 28], s->out_ann, ANN_BITRATE, buf);
    }

    if (idx + 34 < 70)
        c_put(di, s->bits_pos_ss[idx + 30], s->bits_pos_es[idx + 34], s->out_ann, ANN_BITRATE, "EM4100 trailer");
}

static void em4305_add_bits_pos(em4305_state *s, int bit, uint64_t ss_bit, uint64_t es_bit)
{
    if (s->bit_nr < 70) {
        s->bits_pos_val[s->bit_nr] = bit;
        s->bits_pos_ss[s->bit_nr] = ss_bit;
        s->bits_pos_es[s->bit_nr] = es_bit;
        s->bit_nr++;
    }
}

static void em4305_put_fields(struct srd_decoder_inst *di, em4305_state *s)
{
    const char *em4100_decode_str = c_opt_str(di, "em4100_decode", "on");
    int do_em4100 = (strcmp(em4100_decode_str, "on") == 0) ? 1 : 0;

    if (s->bit_nr == 50) {
        /* Login frame */
        if (0 < 70)
            c_put(di, s->bits_pos_ss[0], s->bits_pos_es[0], s->out_ann, ANN_BIT, "Logic zero");
        if (4 < 70)
            c_put(di, s->bits_pos_ss[1], s->bits_pos_es[4], s->out_ann, ANN_BIT, "Command", "Cmd", "C");
        if (49 < 70)
            c_put(di, s->bits_pos_ss[5], s->bits_pos_es[49], s->out_ann, ANN_BIT, "Password", "Passwd", "Pass", "P");

        /* Get command */
        int cmd = em4305_get_3_bits(s, 1);
        if (cmd >= 0 && cmd < 8 && 3 < 70)
            c_put(di, s->bits_pos_ss[1], s->bits_pos_es[3], s->out_ann, ANN_OPCODE, cmds[cmd]);
        em4305_print_row_parity(di, s, 1, 3);

        /* Print data */
        em4305_print_8bit_data(di, s, 5);
        em4305_print_row_parity(di, s, 5, 8);
        em4305_print_8bit_data(di, s, 14);
        em4305_print_row_parity(di, s, 14, 8);
        em4305_print_8bit_data(di, s, 23);
        em4305_print_row_parity(di, s, 23, 8);
        em4305_print_8bit_data(di, s, 32);
        em4305_print_row_parity(di, s, 32, 8);
        em4305_print_col_parity(di, s, 5);

        if (49 < 70) {
            if (s->bits_pos_val[49] == 0) {
                c_put(di, s->bits_pos_ss[49], s->bits_pos_es[49], s->out_ann, ANN_OPCODE, "Stop bit", "Stop", "SB");
            } else {
                c_put(di, s->bits_pos_ss[49], s->bits_pos_es[49], s->out_ann, ANN_OPCODE, "Stop bit error", "Error");
            }
        }

        if (cmd == 1) {
            int password = em4305_get_32_bits(s, 5);
            char buf[64];
            snprintf(buf, sizeof(buf), "Login password: %X", password);
            if (12 < 70 && 46 < 70)
                c_put(di, s->bits_pos_ss[12], s->bits_pos_es[46], s->out_ann, ANN_BITRATE, buf);
        }
    }

    if (s->bit_nr == 57) {
        /* Write/Read frame */
        if (0 < 70)
            c_put(di, s->bits_pos_ss[0], s->bits_pos_es[0], s->out_ann, ANN_BIT, "Logic zero", "LZ");
        if (4 < 70)
            c_put(di, s->bits_pos_ss[1], s->bits_pos_es[4], s->out_ann, ANN_BIT, "Command", "Cmd", "C");
        if (11 < 70)
            c_put(di, s->bits_pos_ss[5], s->bits_pos_es[11], s->out_ann, ANN_BIT, "Address", "Addr", "A");
        if (56 < 70)
            c_put(di, s->bits_pos_ss[12], s->bits_pos_es[56], s->out_ann, ANN_BIT, "Data", "Da", "D");

        /* Get command */
        int cmd = em4305_get_3_bits(s, 1);
        if (cmd >= 0 && cmd < 8 && 3 < 70)
            c_put(di, s->bits_pos_ss[1], s->bits_pos_es[3], s->out_ann, ANN_OPCODE, cmds[cmd]);
        em4305_print_row_parity(di, s, 1, 3);

        /* Get address */
        int addr = em4305_get_4_bits(s, 5);
        if (8 < 70) {
            char buf[64], short_buf[16];
            snprintf(buf, sizeof(buf), "Addr: %d", addr);
            snprintf(short_buf, sizeof(short_buf), "%d", addr);
            c_put(di, s->bits_pos_ss[5], s->bits_pos_es[8], s->out_ann, ANN_ADDRESS, buf, short_buf);
        }
        if (10 < 70)
            c_put(di, s->bits_pos_ss[9], s->bits_pos_es[10], s->out_ann, ANN_OPCODE, "Zero bits", "ZB");
        em4305_print_row_parity(di, s, 5, 6);

        /* Print data */
        em4305_print_8bit_data(di, s, 12);
        em4305_print_row_parity(di, s, 12, 8);
        em4305_print_8bit_data(di, s, 21);
        em4305_print_row_parity(di, s, 21, 8);
        em4305_print_8bit_data(di, s, 30);
        em4305_print_row_parity(di, s, 30, 8);
        em4305_print_8bit_data(di, s, 39);
        em4305_print_row_parity(di, s, 39, 8);
        em4305_print_col_parity(di, s, 12);

        if (56 < 70) {
            if (s->bits_pos_val[56] == 0) {
                c_put(di, s->bits_pos_ss[56], s->bits_pos_es[56], s->out_ann, ANN_OPCODE, "Stop bit", "Stop", "SB");
            } else {
                c_put(di, s->bits_pos_ss[56], s->bits_pos_es[56], s->out_ann, ANN_OPCODE, "Stop bit error", "Error");
            }
        }

        if (addr == 4)
            em4305_decode_config(di, s, 12);

        if (addr == 2) {
            int password = em4305_get_32_bits(s, 12);
            char buf[64];
            snprintf(buf, sizeof(buf), "Write password: %X", password);
            if (12 < 70 && 46 < 70)
                c_put(di, s->bits_pos_ss[12], s->bits_pos_es[46], s->out_ann, ANN_BITRATE, buf);
        }

        if (addr == 5 && do_em4100)
            em4305_em4100_decode1(di, s, 12);
        if (addr == 6 && do_em4100)
            em4305_em4100_decode2(di, s, 12);
    }

    s->bit_nr = 0;
}

static void em4305_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(em4305_state)));
    }
    em4305_state *s = (em4305_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(em4305_state));
    s->out_ann = -1;
    s->state = EM_FFS_SEARCH;
    s->em4100_decode1_partial = 0;
}

static void em4305_start(struct srd_decoder_inst *di)
{
    em4305_state *s = (em4305_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "em4305");
}

static void em4305_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    em4305_state *s = (em4305_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
    if (s->samplerate == 0) return;

    int64_t coilfreq = c_opt_int(di, "coilfreq", 125000);
    int64_t first_field_stop = c_opt_int(di, "first_field_stop", 40);
    int64_t w_gap = c_opt_int(di, "w_gap", 12);
    int64_t w_one_max = c_opt_int(di, "w_one_max", 32);
    int64_t w_zero_on_min = c_opt_int(di, "w_zero_on_min", 15);
    int64_t w_zero_off_max = c_opt_int(di, "w_zero_off_max", 27);

    s->field_clock = (double)s->samplerate / (double)coilfreq;
    s->wzmax = (double)w_zero_off_max * s->field_clock;
    s->wzmin = (double)w_zero_on_min * s->field_clock;
    s->womax = (double)w_one_max * s->field_clock;
    s->ffs = (double)first_field_stop * s->field_clock;
    s->writegap = (double)w_gap * s->field_clock;
    s->nogap = 300.0 * s->field_clock;
}

static void em4305_decode(struct srd_decoder_inst *di)
{
    em4305_state *s = (em4305_state *)c_decoder_get_private(di);

    if (s->samplerate == 0) return;

    /* Initialize internal state */
    if (!s->initialized) {
        uint64_t cur_sample = 0;
        if (c_wait(di, CW_END) != SRD_OK)
            return;
        s->last_samplenum = cur_sample;
        s->oldsamplenum = 0;
        s->old_gap_end = 0;
        s->gap_detected = 0;
        s->bit_nr = 0;
        s->initialized = 1;
    }

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t pl = di_samplenum(di) - s->oldsamplenum;

        if (s->state == EM_FFS_DETECTED) {
            if (pl > s->writegap)
                s->gap_detected = 1;
            if ((s->last_samplenum - s->old_gap_end) > s->nogap) {
                s->gap_detected = 0;
                s->state = EM_FFS_SEARCH;
                c_put(di, s->old_gap_end, s->last_samplenum, s->out_ann, ANN_WRITE_MODE_EXIT, "Write mode exit");
                em4305_put_fields(di, s);
            }
        }

        if (s->state == EM_FFS_SEARCH) {
            if (pl > s->ffs) {
                s->gap_detected = 1;
                c_put(di, s->last_samplenum, di_samplenum(di), s->out_ann, ANN_FIRST_FIELD_STOP, "First field stop", "Field stop", "FFS");
                s->state = EM_FFS_DETECTED;
            }
        }

        if (s->gap_detected == 1) {
            s->gap_detected = 0;
            if ((s->last_samplenum - s->old_gap_end) > s->wzmin &&
                (s->last_samplenum - s->old_gap_end) < s->wzmax) {
                c_put(di, s->old_gap_end, di_samplenum(di), s->out_ann, ANN_BIT_VALUE, "0");
                em4305_add_bits_pos(s, 0, s->old_gap_end, di_samplenum(di));
            }
            if ((s->last_samplenum - s->old_gap_end) > s->womax &&
                (s->last_samplenum - s->old_gap_end) < s->nogap) {
                int one_bits = (int)((s->last_samplenum - s->old_gap_end) / s->womax);
                for (int ox = 0; ox < one_bits; ox++) {
                    uint64_t bs = (uint64_t)(s->old_gap_end + ox * s->womax);
                    uint64_t be = (uint64_t)(s->old_gap_end + ox * s->womax + s->womax);
                    c_put(di, bs, be, s->out_ann, ANN_BIT_VALUE, "1");
                    em4305_add_bits_pos(s, 1, bs, be);
                }
                if ((di_samplenum(di) - s->last_samplenum) > s->wzmin &&
                    (di_samplenum(di) - s->last_samplenum) < s->wzmax) {
                    uint64_t bs = (uint64_t)(s->old_gap_end + one_bits * s->womax);
                    c_put(di, bs, di_samplenum(di), s->out_ann, ANN_BIT_VALUE, "0");
                    em4305_add_bits_pos(s, 0, bs, di_samplenum(di));
                }
            }
            s->old_gap_end = di_samplenum(di);
        }

        if (s->state == EM_SKIP)
            s->state = EM_FFS_SEARCH;

        s->oldsamplenum = di_samplenum(di);
        s->last_samplenum = di_samplenum(di);
    }
}

static void em4305_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder em4305_c_decoder = {
    .id = "em4305_c",
    .name = "EM4305(C)",
    .longname = "RFID EM4205/EM4305 (C)",
    .desc = "EM4205/EM4305 100-150kHz RFID protocol.",
    .license = "gplv2+",
    .channels = em4305_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = em4305_options,
    .num_options = 7,
    .num_annotations = NUM_ANN,
    .ann_labels = em4305_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = em4305_ann_rows,
    .inputs = em4305_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = em4305_tags,
    .num_tags = 2,
    .reset = em4305_reset,
    .start = em4305_start,
    .decode = em4305_decode,
    .destroy = em4305_destroy,
    .state_size = 0,
    .metadata = em4305_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &em4305_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}