/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2011-2014 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2016 Gerhard Sittig <gerhard.sittig@gmx.net>
 * Copyright (C) 2023 DreamSourceLab <support@dreamsourcelab.com>
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
    ANN_BIT = 0,
    ANN_START,
    ANN_DATA,
    ANN_PARITY_OK,
    ANN_PARITY_ERR,
    ANN_STOP_OK,
    ANN_STOP_ERR,
    ANN_BREAK,
    ANN_OPCODE,
    ANN_DATA_PROG,
    ANN_DATA_DEV,
    ANN_PDI_BREAK,
    ANN_ENABLE,
    ANN_DISABLE,
    ANN_COMMAND,
    NUM_ANN,
};

enum {
    BIN_BYTES = 0,
};

enum {
    OP_LDS = 0, OP_LD, OP_STS, OP_ST,
    OP_LDCS, OP_REPEAT, OP_STCS, OP_KEY,
};

struct pdi_bit {
    int val;
    uint64_t ss;
    uint64_t es;
};

typedef struct {
    uint64_t samplerate;

    /* Clock edge tracking */
    uint64_t ss_last_fall;
    uint64_t ss_curr_fall;
    int data_sample;

    /* UART frame bits */
    struct pdi_bit bits[12];  /* max 1+8+1+1 = 11 bits */
    int bit_count;

    /* BREAK detection */
    int zero_count;
    uint64_t zero_ss;
    uint64_t break_ss;
    uint64_t break_es;

    /* PDI instruction state */
    int insn_rep_count;
    int insn_opcode;
    uint8_t insn_dat_bytes[8];
    int insn_dat_count;
    uint64_t insn_ss_data;
    uint64_t cmd_ss;
    char cmd_parts_nice[256];
    char cmd_parts_terse[256];
    int insn_write_counts;
    int insn_read_counts;
    int width_addr;
    int width_data;
    const char *ptr_txt;
    const char *ptr_txt_terse;
    int reg_num;
    const char *reg_txt;
    const char *reg_txt_terse;

    int out_ann;
    int out_binary;
} avr_pdi_priv;

static const char *pointer_format_nice[] = {
    "*(ptr)", "*(ptr++)", "ptr", "ptr++ (rsv)"
};
static const char *pointer_format_terse[] = {
    "*p", "*p++", "p", "(rsv)"
};

static struct srd_channel avr_pdi_channels[] = {
    {"reset", "RESET", "RESET / PDI_CLK", 0, SRD_CHANNEL_SCLK, "dec_avr_pdi_chan_reset"},
    {"data",  "DATA",  "PDI_DATA",        1, SRD_CHANNEL_SDATA, "dec_avr_pdi_chan_data"},
};

static const char *avr_pdi_ann_labels[][3] = {
    {"", "uart-bit",   "UART bit"},
    {"", "start-bit",  "Start bit"},
    {"", "data-bit",   "Data bit"},
    {"", "parity-ok",  "Parity OK bit"},
    {"", "parity-err", "Parity error bit"},
    {"", "stop-ok",    "Stop OK bit"},
    {"", "stop-err",   "Stop error bit"},
    {"", "break",      "BREAK condition"},
    {"", "opcode",     "Instruction opcode"},
    {"", "data-prog",  "Programmer data"},
    {"", "data-dev",   "Device data"},
    {"", "pdi-break",  "BREAK at PDI level"},
    {"", "enable",     "Enable PDI"},
    {"", "disable",    "Disable PDI"},
    {"", "cmd-data",   "PDI command with data"},
};

static const char *avr_pdi_binary_labels[][3] __attribute__((unused)) = {
    {"", "bytes", "PDI protocol bytes"},
};

static const int avr_pdi_row_uart_bits_classes[] = {ANN_BIT, -1};
static const int avr_pdi_row_uart_fields_classes[] = {ANN_START, ANN_DATA, ANN_PARITY_OK, ANN_PARITY_ERR, ANN_STOP_OK, ANN_STOP_ERR, ANN_BREAK, -1};
static const int avr_pdi_row_pdi_fields_classes[] = {ANN_OPCODE, ANN_DATA_PROG, ANN_DATA_DEV, ANN_PDI_BREAK, -1};
static const int avr_pdi_row_pdi_cmds_classes[] = {ANN_ENABLE, ANN_DISABLE, ANN_COMMAND, -1};

static const struct srd_c_ann_row avr_pdi_ann_rows[] = {
    {"uart_bits",   "UART bits",    avr_pdi_row_uart_bits_classes,   1},
    {"uart_fields", "UART fields",  avr_pdi_row_uart_fields_classes, 7},
    {"pdi_fields",  "PDI fields",   avr_pdi_row_pdi_fields_classes,  4},
    {"pdi_cmds",    "PDI Cmds",     avr_pdi_row_pdi_cmds_classes,    3},
};

static const struct srd_decoder_binary avr_pdi_binary[] = {
    {0, "bytes", "PDI protocol bytes"},
};

static const char *avr_pdi_inputs[] = {"logic"};
static const char *avr_pdi_tags[] = {"Debug/trace"};

static int count_ones(uint8_t val)
{
    int count = 0;
    while (val) {
        count += val & 1;
        val >>= 1;
    }
    return count;
}

static int parity_even_ok(uint8_t data_val, int parity_bit)
{
    return (count_ones(data_val) + parity_bit) % 2 == 0;
}

static const char *ctrl_reg_name(int reg)
{
    switch (reg) {
        case 0: return "status";
        case 1: return "reset";
        case 2: return "ctrl";
        default: return NULL;
    }
}

static void clear_insn(avr_pdi_priv *s)
{
    s->insn_opcode = -1;
    s->insn_dat_count = 0;
    memset(s->insn_dat_bytes, 0, sizeof(s->insn_dat_bytes));
    s->insn_ss_data = 0;
    s->cmd_ss = 0;
    s->cmd_parts_nice[0] = '\0';
    s->cmd_parts_terse[0] = '\0';
    s->insn_write_counts = 0;
    s->insn_read_counts = 0;
    s->width_addr = 0;
    s->width_data = 0;
    s->ptr_txt = NULL;
    s->ptr_txt_terse = NULL;
    s->reg_num = -1;
    s->reg_txt = NULL;
    s->reg_txt_terse = NULL;
}

static void clear_state(avr_pdi_priv *s)
{
    s->ss_last_fall = 0;
    s->ss_curr_fall = 0;
    s->data_sample = -1;
    s->bit_count = 0;
    s->zero_count = 0;
    s->zero_ss = 0;
    s->break_ss = 0;
    s->break_es = 0;
    s->insn_rep_count = 0;
    clear_insn(s);
}

static void put_ann_bit(struct srd_decoder_inst *di, avr_pdi_priv *s, int bit_nr, int ann_idx)
{
    struct pdi_bit *b = &s->bits[bit_nr];
    char val_str[4];
    snprintf(val_str, sizeof(val_str), "%d", b->val);
    c_put(di, b->ss, b->es, s->out_ann, ann_idx, val_str);
}

static void put_ann_data(struct srd_decoder_inst *di, avr_pdi_priv *s, int bit_nr, int ann_idx, const char **txts)
{
    struct pdi_bit *b = &s->bits[bit_nr];
    struct srd_c_annotation ann;
    ann.ann_class = ann_idx;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, b->ss, b->es, s->out_ann, &ann);
}

static void put_ann_row_val(struct srd_decoder_inst *di, avr_pdi_priv *s,
                            uint64_t ss, uint64_t es, int ann_idx, const char **txts)
{
    struct srd_c_annotation ann;
    ann.ann_class = ann_idx;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, ss, es, s->out_ann, &ann);
}

static void handle_byte(struct srd_decoder_inst *di, avr_pdi_priv *s,
                        uint64_t ss, uint64_t es, int is_break, uint8_t byteval);

static void handle_bits(struct srd_decoder_inst *di, avr_pdi_priv *s,
                        uint64_t ss, uint64_t es, int bitval)
{
    int frame_bitcount = 1 + 8 + 1 + 1;  /* start + data + parity + stop */

    /* Detect BREAK: adjacent runs of all-zero bits */
    if (bitval == 1) {
        s->zero_count = 0;
    } else {
        if (!s->zero_count)
            s->zero_ss = ss;
        s->zero_count++;
        if (s->zero_count == frame_bitcount)
            s->break_ss = s->zero_ss;
    }

    /* BREAK handling */
    if (s->break_ss != 0) {
        if (bitval == 0) {
            s->break_es = es;
            return;
        }
        if (s->break_es == 0)
            return;

        const char *brk_txts[] = {"Break condition", "BREAK", "BRK", NULL};
        put_ann_row_val(di, s, s->break_ss, s->break_es, ANN_BREAK, brk_txts);
        handle_byte(di, s, s->break_ss, s->break_es, 1, 0);
        s->break_ss = 0;
        s->break_es = 0;
        s->bit_count = 0;
        return;
    }

    /* Ignore high bits when waiting for START */
    if (s->bit_count == 0 && bitval == 1)
        return;

    /* Store bit */
    if (s->bit_count < 12) {
        s->bits[s->bit_count].val = bitval;
        s->bits[s->bit_count].ss = ss;
        s->bits[s->bit_count].es = es;
    }
    s->bit_count++;

    if (s->bit_count < frame_bitcount)
        return;

    /* Parse UART frame */
    int bits_num = 0;
    for (int pos = 0; pos < frame_bitcount; pos++)
        bits_num |= s->bits[pos].val << pos;

    bits_num >>= 1;
    uint8_t data_val = bits_num & 0xff; bits_num >>= 8;
    int parity_bit = bits_num & 0x01; bits_num >>= 1;
    int stop_bit = bits_num & 0x01;

    int parity_ok = parity_even_ok(data_val, parity_bit);
    int stop_ok = (stop_bit == 1);
    int valid_frame = parity_ok && stop_ok;

    /* Emit annotations */
    for (int idx = 0; idx < frame_bitcount; idx++)
        put_ann_bit(di, s, idx, ANN_BIT);

    /* Start bit */
    const char *start_txts[] = {"Start bit", "Start", "S", NULL};
    put_ann_data(di, s, 0, ANN_START, start_txts);

    /* Data bits */
    char data_text[8];
    snprintf(data_text, sizeof(data_text), "%02x", data_val);
    char d1[32], d2[16];
    snprintf(d1, sizeof(d1), "Data: %s", data_text);
    snprintf(d2, sizeof(d2), "D: %s", data_text);
    const char *data_txts[] = {d1, d2, data_text, NULL};
    struct srd_c_annotation ann;
    ann.ann_class = ANN_DATA;
    ann.ann_type = 0;
    ann.ann_text = (char **)data_txts;
    c_decoder_put(di, s->bits[1].ss, s->bits[8].es, s->out_ann, &ann);

    /* Parity bit */
    if (parity_ok) {
        const char *par_txts[] = {"Parity OK", "Par OK", "P", NULL};
        put_ann_data(di, s, 9, ANN_PARITY_OK, par_txts);
    } else {
        const char *par_txts[] = {"Parity error", "Par ERR", "PE", NULL};
        put_ann_data(di, s, 9, ANN_PARITY_ERR, par_txts);
    }

    /* Stop bit */
    if (stop_ok) {
        const char *stop_txts[] = {"Stop bit", "Stop", "T", NULL};
        put_ann_data(di, s, 10, ANN_STOP_OK, stop_txts);
    } else {
        const char *stop_txts[] = {"Stop bit error", "Stop ERR", "TE", NULL};
        put_ann_data(di, s, 10, ANN_STOP_ERR, stop_txts);
    }

    /* Binary output and PDI instruction decode */
    if (valid_frame) {
        uint64_t byte_ss = s->bits[0].ss;
        uint64_t byte_es = s->bits[frame_bitcount - 1].es;
        c_put_bin(di, byte_ss, byte_es, s->out_binary, BIN_BYTES, 1, &data_val);
        handle_byte(di, s, byte_ss, byte_es, 0, data_val);
    }

    s->bit_count = 0;
}

static void handle_byte(struct srd_decoder_inst *di, avr_pdi_priv *s,
                        uint64_t ss, uint64_t es, int is_break, uint8_t byteval)
{
    /* Handle BREAK */
    if (is_break) {
        strcat(s->cmd_parts_nice, "BREAK");
        strcat(s->cmd_parts_terse, "BRK");
        s->insn_rep_count = 0;
        /* Fall through to end of instruction */
    }

    /* Decode instruction opcode */
    if (s->insn_opcode == -1 && !is_break) {
        int opcode = (byteval & 0xe0) >> 5;
        int arg30 = byteval & 0x0f;
        int arg32 = (byteval & 0x0c) >> 2;
        int arg10 = byteval & 0x03;
        s->insn_opcode = opcode;
        s->cmd_ss = ss;

        char mnem1[64], mnem2[48], mnem3[16];

        if (opcode == OP_LDS) {
            s->width_addr = arg32 + 1;
            s->width_data = arg10 + 1;
            s->insn_write_counts = 1;
            s->insn_read_counts = 1;
            snprintf(mnem1, sizeof(mnem1), "Insn: LDS a%d, m%d", s->width_addr, s->width_data);
            snprintf(mnem2, sizeof(mnem2), "LDS a%d, m%d", s->width_addr, s->width_data);
            snprintf(mnem3, sizeof(mnem3), "LDS");
            strcpy(s->cmd_parts_nice, "LDS");
            strcpy(s->cmd_parts_terse, "LDS");
        } else if (opcode == OP_LD) {
            s->ptr_txt = pointer_format_nice[arg32];
            s->ptr_txt_terse = pointer_format_terse[arg32];
            s->width_data = arg10 + 1;
            s->insn_write_counts = 0;
            s->insn_read_counts = 1;
            if (s->insn_rep_count) {
                s->insn_read_counts = s->insn_rep_count * s->insn_read_counts;
                s->insn_rep_count = 0;
            }
            snprintf(mnem1, sizeof(mnem1), "Insn: LD %s m%d", s->ptr_txt, s->width_data);
            snprintf(mnem2, sizeof(mnem2), "LD %s m%d", s->ptr_txt, s->width_data);
            snprintf(mnem3, sizeof(mnem3), "LD");
            snprintf(s->cmd_parts_nice, sizeof(s->cmd_parts_nice), "LD %s", s->ptr_txt);
            snprintf(s->cmd_parts_terse, sizeof(s->cmd_parts_terse), "LD %s", s->ptr_txt_terse);
        } else if (opcode == OP_STS) {
            s->width_addr = arg32 + 1;
            s->width_data = arg10 + 1;
            s->insn_write_counts = 2;
            s->insn_read_counts = 0;
            snprintf(mnem1, sizeof(mnem1), "Insn: STS a%d, i%d", s->width_addr, s->width_data);
            snprintf(mnem2, sizeof(mnem2), "STS a%d, i%d", s->width_addr, s->width_data);
            snprintf(mnem3, sizeof(mnem3), "STS");
            strcpy(s->cmd_parts_nice, "STS");
            strcpy(s->cmd_parts_terse, "STS");
        } else if (opcode == OP_ST) {
            s->ptr_txt = pointer_format_nice[arg32];
            s->ptr_txt_terse = pointer_format_terse[arg32];
            s->width_data = arg10 + 1;
            s->insn_write_counts = 1;
            s->insn_read_counts = 0;
            if (s->insn_rep_count) {
                s->insn_write_counts = s->insn_rep_count * s->insn_write_counts;
                s->insn_rep_count = 0;
            }
            snprintf(mnem1, sizeof(mnem1), "Insn: ST %s i%d", s->ptr_txt, s->width_data);
            snprintf(mnem2, sizeof(mnem2), "ST %s i%d", s->ptr_txt, s->width_data);
            snprintf(mnem3, sizeof(mnem3), "ST");
            snprintf(s->cmd_parts_nice, sizeof(s->cmd_parts_nice), "ST %s", s->ptr_txt);
            snprintf(s->cmd_parts_terse, sizeof(s->cmd_parts_terse), "ST %s", s->ptr_txt_terse);
        } else if (opcode == OP_LDCS) {
            s->reg_num = arg30;
            const char *rn = ctrl_reg_name(s->reg_num);
            char reg_buf[16];
            if (!rn) {
                snprintf(reg_buf, sizeof(reg_buf), "r%d", s->reg_num);
                rn = reg_buf;
            }
            s->reg_txt = rn;
            char reg_terse[16];
            snprintf(reg_terse, sizeof(reg_terse), "%d", s->reg_num);
            s->reg_txt_terse = reg_terse;
            s->insn_write_counts = 0;
            s->insn_read_counts = 1;
            s->width_data = 1;
            snprintf(mnem1, sizeof(mnem1), "Insn: LDCS %s, m1", s->reg_txt);
            snprintf(mnem2, sizeof(mnem2), "LDCS %s, m1", s->reg_txt);
            snprintf(mnem3, sizeof(mnem3), "LDCS");
            snprintf(s->cmd_parts_nice, sizeof(s->cmd_parts_nice), "LDCS %s", s->reg_txt);
            snprintf(s->cmd_parts_terse, sizeof(s->cmd_parts_terse), "LDCS %s", s->reg_txt_terse);
        } else if (opcode == OP_STCS) {
            s->reg_num = arg30;
            const char *rn = ctrl_reg_name(s->reg_num);
            char reg_buf[16];
            if (!rn) {
                snprintf(reg_buf, sizeof(reg_buf), "r%d", s->reg_num);
                rn = reg_buf;
            }
            s->reg_txt = rn;
            char reg_terse[16];
            snprintf(reg_terse, sizeof(reg_terse), "%d", s->reg_num);
            s->reg_txt_terse = reg_terse;
            s->width_data = 1;
            s->insn_write_counts = 1;
            s->insn_read_counts = 0;
            snprintf(mnem1, sizeof(mnem1), "Insn: STCS %s, i1", s->reg_txt);
            snprintf(mnem2, sizeof(mnem2), "STCS %s, i1", s->reg_txt);
            snprintf(mnem3, sizeof(mnem3), "STCS");
            snprintf(s->cmd_parts_nice, sizeof(s->cmd_parts_nice), "STCS %s", s->reg_txt);
            snprintf(s->cmd_parts_terse, sizeof(s->cmd_parts_terse), "STCS %s", s->reg_txt_terse);
        } else if (opcode == OP_REPEAT) {
            s->width_data = arg10 + 1;
            s->insn_write_counts = 1;
            s->insn_read_counts = 0;
            snprintf(mnem1, sizeof(mnem1), "Insn: REPEAT i%d", s->width_data);
            snprintf(mnem2, sizeof(mnem2), "REPEAT i%d", s->width_data);
            snprintf(mnem3, sizeof(mnem3), "REP");
            strcpy(s->cmd_parts_nice, "REPEAT");
            strcpy(s->cmd_parts_terse, "REP");
        } else if (opcode == OP_KEY) {
            s->width_data = 8;
            snprintf(mnem1, sizeof(mnem1), "Insn: KEY i%d", s->width_data);
            snprintf(mnem2, sizeof(mnem2), "KEY i%d", s->width_data);
            snprintf(mnem3, sizeof(mnem3), "KEY");
            strcpy(s->cmd_parts_nice, "KEY");
            strcpy(s->cmd_parts_terse, "KEY");
        } else {
            s->bit_count = 0;
            return;
        }

        /* Emit opcode annotation */
        const char *op_txts[] = {mnem1, mnem2, mnem3, NULL};
        put_ann_row_val(di, s, ss, es, ANN_OPCODE, op_txts);

        /* Prepare for data bytes */
        memset(s->insn_dat_bytes, 0, sizeof(s->insn_dat_bytes));

        if (s->insn_write_counts != 0) {
            if (s->insn_opcode == OP_LDS)
                s->insn_dat_count = s->width_addr;
            else if (s->insn_opcode == OP_STS)
                s->insn_dat_count = (s->insn_write_counts == 2) ? s->width_addr : s->width_data;
            else if (s->insn_opcode == OP_ST)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_STCS)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_REPEAT)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_KEY)
                s->insn_dat_count = s->width_data;
            return;
        }
        if (s->insn_read_counts != 0) {
            if (s->insn_opcode == OP_LDS)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_LD)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_LDCS)
                s->insn_dat_count = s->width_data;
            return;
        }
        /* Fall through: no operands */
    }

    /* Read data bytes */
    if (s->insn_dat_count > 0 && !is_break) {
        if (s->insn_dat_bytes[0] == 0 && s->insn_ss_data == 0)
            s->insn_ss_data = ss;

        /* Find first empty slot */
        int idx = 0;
        while (idx < 8 && s->insn_dat_bytes[idx] != 0) idx++;
        if (idx < 8)
            s->insn_dat_bytes[idx] = byteval;

        s->insn_dat_count--;
        if (s->insn_dat_count > 0)
            return;

        /* Determine direction */
        int data_ann;
        if (s->insn_write_counts != 0) {
            data_ann = ANN_DATA_PROG;
            s->insn_write_counts--;
        } else {
            data_ann = ANN_DATA_DEV;
            s->insn_read_counts--;
        }

        /* Format data value (little endian, reverse for display) */
        int num_bytes = 0;
        while (num_bytes < 8 && s->insn_dat_bytes[num_bytes] != 0) num_bytes++;
        /* Actually count from how many we accumulated */
        uint8_t tmp[8];
        int nb = 0;
        for (int i = 0; i < 8; i++) {
            if (s->insn_dat_bytes[i] != 0 || i < idx + 1) {
                tmp[nb++] = s->insn_dat_bytes[i];
            }
        }
        /* Reverse for big-endian display */
        char digits[32] = {0};
        int dpos = 0;
        for (int i = nb - 1; i >= 0; i--)
            dpos += snprintf(digits + dpos, sizeof(digits) - dpos, "%02x", tmp[i]);
        char hex_str[32];
        snprintf(hex_str, sizeof(hex_str), "0x%s", digits);
        char prefix_str[64];
        snprintf(prefix_str, sizeof(prefix_str), "Data: %s", hex_str);
        const char *data_txts[] = {prefix_str, hex_str, digits, NULL};

        uint64_t data_ss = s->insn_ss_data;
        uint64_t data_es = es;
        put_ann_row_val(di, s, data_ss, data_es, data_ann, data_txts);

        /* Append to command parts */
        int plen = (int)strlen(s->cmd_parts_nice);
        snprintf(s->cmd_parts_nice + plen, sizeof(s->cmd_parts_nice) - plen, " %s", hex_str);
        int tlen = (int)strlen(s->cmd_parts_terse);
        snprintf(s->cmd_parts_terse + tlen, sizeof(s->cmd_parts_terse) - tlen, " %s", digits);

        memset(s->insn_dat_bytes, 0, sizeof(s->insn_dat_bytes));
        s->insn_ss_data = 0;

        /* Check for more data */
        if (s->insn_write_counts != 0) {
            if (s->insn_opcode == OP_LDS)
                s->insn_dat_count = s->width_addr;
            else if (s->insn_opcode == OP_STS)
                s->insn_dat_count = (s->insn_write_counts == 2) ? s->width_addr : s->width_data;
            else if (s->insn_opcode == OP_ST)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_STCS)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_REPEAT)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_KEY)
                s->insn_dat_count = s->width_data;
            return;
        }
        if (s->insn_read_counts != 0) {
            if (s->insn_opcode == OP_LDS)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_LD)
                s->insn_dat_count = s->width_data;
            else if (s->insn_opcode == OP_LDCS)
                s->insn_dat_count = s->width_data;
            return;
        }
        /* Fall through: all operands seen */
    }

    if (s->cmd_ss == 0)
        return;

    /* End of instruction: emit command annotation */
    const char *cmd_txts[] = {s->cmd_parts_nice, s->cmd_parts_terse, NULL};
    put_ann_row_val(di, s, s->cmd_ss, es, ANN_COMMAND, cmd_txts);

    /* Handle REPEAT */
    if (s->insn_opcode == OP_REPEAT && !is_break) {
        s->insn_rep_count = (int)strtol(s->cmd_parts_nice + strlen(s->cmd_parts_nice) - 1, NULL, 0);
        /* Parse the last hex value in cmd_parts_nice as repeat count */
        char *last_space = strrchr(s->cmd_parts_nice, ' ');
        if (last_space)
            s->insn_rep_count = (int)strtol(last_space + 1, NULL, 0);
    }

    /* Save rep count, clear insn state */
    int save_rep = s->insn_rep_count;
    clear_insn(s);
    s->insn_rep_count = save_rep;
}

static void avr_pdi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(avr_pdi_priv)));
    }
    avr_pdi_priv *s = (avr_pdi_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(avr_pdi_priv));
    s->out_ann = -1;
    s->out_binary = -1;
    s->insn_opcode = -1;
    clear_state(s);
}

static void avr_pdi_start(struct srd_decoder_inst *di)
{
    avr_pdi_priv *s = (avr_pdi_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "avr_pdi");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "avr_pdi");
}

static void avr_pdi_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    avr_pdi_priv *s = (avr_pdi_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void avr_pdi_decode(struct srd_decoder_inst *di)
{
    avr_pdi_priv *s = (avr_pdi_priv *)c_decoder_get_private(di);
    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int clock_pin = c_pin(di, 0);
        int data_pin = c_pin(di, 1);

        /* Sample data on rising clock edge */
        if (clock_pin == 1) {
            s->data_sample = data_pin;
            continue;
        }

        /* Falling clock edge: process previous bit slot */
        s->ss_last_fall = s->ss_curr_fall;
        s->ss_curr_fall = di_samplenum(di);
        if (s->ss_last_fall == 0) {
            /* First falling edge, no previous bit slot */
            continue;
        }

        uint64_t bit_ss = s->ss_last_fall;
        uint64_t bit_es = s->ss_curr_fall;
        int bit_val = s->data_sample;
        handle_bits(di, s, bit_ss, bit_es, bit_val);
    }
}

static void avr_pdi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder avr_pdi_c_decoder = {
    .id = "avr_pdi_c",
    .name = "AVR PDI(C)",
    .longname = "Atmel Program and Debug Interface(C)",
    .desc = "Atmel ATxmega Program and Debug Interface (PDI) protocol. (C implementation)",
    .license = "gplv2+",
    .channels = avr_pdi_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = avr_pdi_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = avr_pdi_ann_rows,
    .inputs = avr_pdi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = avr_pdi_binary,
    .num_binary = 1,
    .tags = avr_pdi_tags,
    .num_tags = 1,
    .reset = avr_pdi_reset,
    .start = avr_pdi_start,
    .decode = avr_pdi_decode,
    .destroy = avr_pdi_destroy,
    .state_size = 0,
    .metadata = avr_pdi_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &avr_pdi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}