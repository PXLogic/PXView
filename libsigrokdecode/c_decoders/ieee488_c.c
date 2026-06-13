/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2016 Rudolf Reuter <reuterru@arcor.de>
 * Copyright (C) 2017 Marcus Comstedt <marcus@mc.pp.se>
 * Copyright (C) 2019 Gerhard Sittig <gerhard.sittig@gmx.net>
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

#define PIN_DIO1  0
#define PIN_DIO2  1
#define PIN_DIO3  2
#define PIN_DIO4  3
#define PIN_DIO5  4
#define PIN_DIO6  5
#define PIN_DIO7  6
#define PIN_DIO8  7
#define PIN_EOI   8
#define PIN_DAV   9
#define PIN_NRFD  10
#define PIN_NDAC  11
#define PIN_IFC   12
#define PIN_SRQ   13
#define PIN_ATN   14
#define PIN_REN   15
#define PIN_CLK   16
#define PIN_DATA  PIN_DIO1

enum ieee488_state {
    STATE_WAIT_READY_TO_SEND = 0,
    STATE_WAIT_READY_FOR_DATA,
    STATE_PREP_DATA_TEST_EOI,
    STATE_CLOCK_DATA_BITS,
};

enum ieee488_ann {
    ANN_BIT = 0,
    ANN_RAW_BYTE,
    ANN_CMD,
    ANN_LADDR,
    ANN_TADDR,
    ANN_SADDR,
    ANN_DATA,
    ANN_EOI,
    ANN_TEXT,
    ANN_IEC_PERIPH,
    ANN_WARN,
    NUM_ANN,
};

enum ieee488_bin {
    BIN_RAW = 0,
    BIN_DATA,
    NUM_BIN,
};

#define MAX_ACCU_BYTES 4096
#define MAX_ACCU_TEXT  8192
#define MAX_LISTENERS  31

typedef struct {
    int is_serial;

    enum ieee488_state serial_state;
    uint8_t serial_bits[8];
    int serial_bit_count;
    uint64_t ss_byte;
    uint64_t ss_bit;

    uint8_t curr_raw;
    int curr_atn;
    int curr_eoi;
    int latch_atn;
    int latch_eoi;

    uint64_t ss_raw;
    uint64_t es_raw;

    uint64_t ss_eoi;
    uint64_t es_eoi;

    uint8_t accu_bytes[MAX_ACCU_BYTES];
    int accu_bytes_len;
    char accu_text[MAX_ACCU_TEXT];
    int accu_text_len;
    uint64_t ss_text;
    uint64_t es_text;

    int last_talker;
    int last_listener[MAX_LISTENERS];
    int last_listener_count;

    int last_iec_addr;
    int last_iec_sec;

    int iec_periph;
    int delim_eol;

    int out_ann;
    int out_bin;
    int out_python;

    int has_clk;
    int has_dio8;
    int has_dav;
    int has_atn;
    int has_eoi;
    int has_ifc;
    int has_srq;
    int has_all_dio;

    int idx_dav;
    int idx_atn;
    int idx_eoi;
    int idx_ifc;
    int parallel_first_pass;
} ieee488_priv;

static struct srd_channel ieee488_channels[] = {
    { "dio1", "DIO1/DATA", "Data I/O bit 1, or serial data", 0, SRD_CHANNEL_SDATA, "dec_ieee488_chan_dio1" },
};

static struct srd_channel ieee488_optional_channels[] = {
    { "dio2", "DIO2", "Data I/O bit 2", 1, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio2" },
    { "dio3", "DIO3", "Data I/O bit 3", 2, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio3" },
    { "dio4", "DIO4", "Data I/O bit 4", 3, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio4" },
    { "dio5", "DIO5", "Data I/O bit 5", 4, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio5" },
    { "dio6", "DIO6", "Data I/O bit 6", 5, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio6" },
    { "dio7", "DIO7", "Data I/O bit 7", 6, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio7" },
    { "dio8", "DIO8", "Data I/O bit 8", 7, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dio8" },
    { "eoi", "EOI", "End or identify", 8, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_eoi" },
    { "dav", "DAV", "Data valid", 9, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_dav" },
    { "nrfd", "NRFD", "Not ready for data", 10, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_nrfd" },
    { "ndac", "NDAC", "Not data accepted", 11, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_ndac" },
    { "ifc", "IFC", "Interface clear", 12, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_ifc" },
    { "srq", "SRQ", "Service request", 13, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_srq" },
    { "atn", "ATN", "Attention", 14, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_atn" },
    { "ren", "REN", "Remote enable", 15, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_ren" },
    { "clk", "CLK", "Serial clock", 16, SRD_CHANNEL_COMMON, "dec_ieee488_opt_chan_clk" },
};

static struct srd_decoder_option ieee488_options_arr[2];

static const char* ieee488_ann_labels[][3] = {
    { "", "bit", "IEC bit" },
    { "", "raw", "Raw byte" },
    { "", "cmd", "Command" },
    { "", "laddr", "Listener address" },
    { "", "taddr", "Talker address" },
    { "", "saddr", "Secondary address" },
    { "", "data", "Data byte" },
    { "", "eoi", "EOI" },
    { "", "text", "Talker text" },
    { "", "periph", "IEC bus peripherals" },
    { "", "warning", "Warning" },
};

static const int ieee488_row_bits_classes[] = { ANN_BIT, -1 };
static const int ieee488_row_raws_classes[] = { ANN_RAW_BYTE, -1 };
static const int ieee488_row_gpib_classes[] = { ANN_CMD, ANN_LADDR, ANN_TADDR, ANN_SADDR, ANN_DATA, -1 };
static const int ieee488_row_eois_classes[] = { ANN_EOI, -1 };
static const int ieee488_row_texts_classes[] = { ANN_TEXT, -1 };
static const int ieee488_row_periphs_classes[] = { ANN_IEC_PERIPH, -1 };
static const int ieee488_row_warnings_classes[] = { ANN_WARN, -1 };
static const struct srd_c_ann_row ieee488_ann_rows[] = {
    { "bits", "IEC bits", ieee488_row_bits_classes, 1 },
    { "raws", "Raw bytes", ieee488_row_raws_classes, 1 },
    { "gpib", "Commands/data", ieee488_row_gpib_classes, 5 },
    { "eois", "EOI", ieee488_row_eois_classes, 1 },
    { "texts", "Talker texts", ieee488_row_texts_classes, 1 },
    { "periphs", "IEC peripherals", ieee488_row_periphs_classes, 1 },
    { "warnings", "Warnings", ieee488_row_warnings_classes, 1 },
};

static const struct srd_decoder_binary ieee488_binary[] = {
    { BIN_RAW, "raw", "Raw bytes" },
    { BIN_DATA, "data", "Talker bytes" },
};

static const char* ieee488_inputs[] = { "logic", NULL };
static const char* ieee488_outputs[] = { "ieee488", NULL };
static const char* ieee488_tags[] = { "PC", "Retro computing", NULL };

/* Command table */
typedef struct {
    uint8_t code;
    const char *name;
    const char *short_name;
} cmd_entry;

static const cmd_entry cmd_table[] = {
    { 0x01, "Go To Local", "GTL" },
    { 0x04, "Selected Device Clear", "SDC" },
    { 0x05, "Parallel Poll Configure", "PPC" },
    { 0x08, "Global Execute Trigger", "GET" },
    { 0x09, "Take Control", "TCT" },
    { 0x11, "Local Lock Out", "LLO" },
    { 0x14, "Device Clear", "DCL" },
    { 0x15, "Parallel Poll Unconfigure", "PPU" },
    { 0x18, "Serial Poll Enable", "SPE" },
    { 0x19, "Serial Poll Disable", "SPD" },
    { 0x3f, "Unlisten", "UNL" },
    { 0x5f, "Untalk", "UNT" },
    { 0x00, NULL, NULL } /* sentinel */
};

/* ASCII control codes */
static const char *control_codes[] = {
    "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL",
    "BS",  "TAB", "LF",  "VT",  "FF",  "CR",  "SO",  "SI",
    "DLE", "DC1", "DC2", "DC3", "DC4", "NAK", "SYN", "ETB",
    "CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US",
};

static uint8_t bitpack(uint8_t *bits, int count)
{
    uint8_t val = 0;
    int i;
    for (i = 0; i < count && i < 8; i++)
        val |= (bits[i] << i);
    return val;
}

static int is_command(uint8_t b, int *is_unl, int *is_unt)
{
    *is_unl = 0;
    *is_unt = 0;
    if (b < 0x20) return 1;
    if (b >= 0x20 && b < 0x40 && (b & 0x1f) == 31) { *is_unl = 1; return 1; }
    if (b >= 0x40 && b < 0x60 && (b & 0x1f) == 31) { *is_unt = 1; return 1; }
    return 0;
}

static int is_listen_addr(uint8_t b)
{
    if (b >= 0x20 && b < 0x40) return b & 0x1f;
    return -1;
}

static int is_talk_addr(uint8_t b)
{
    if (b >= 0x40 && b < 0x60) return b & 0x1f;
    return -1;
}

static int is_secondary_addr(uint8_t b)
{
    if (b >= 0x60 && b < 0x80) return b & 0x1f;
    return -1;
}

static int is_msb_set(uint8_t b)
{
    if (b & 0x80) return b;
    return -1;
}

static void get_data_text(uint8_t b, char *buf, int buf_len)
{
    if (b >= 0x20 && b < 0x7f && b != '[' && b != ']') {
        snprintf(buf, buf_len, "%c", (char)b);
    } else if (b < 0x20) {
        snprintf(buf, buf_len, "[%s]", control_codes[b]);
    } else {
        snprintf(buf, buf_len, "[%02x]", b);
    }
}

static void emit_eoi_ann(struct srd_decoder_inst *di, ieee488_priv *s, uint64_t ss, uint64_t es)
{
    c_put(di, ss, es, s->out_ann, ANN_EOI, "EOI");
}

static void flush_bytes_text_accu(struct srd_decoder_inst *di, ieee488_priv *s)
{
    if (s->accu_bytes_len > 0 && s->ss_text != 0 && s->es_text != 0) {
        c_put_bin(di, s->ss_text, s->es_text, s->out_bin,
            BIN_DATA, s->accu_bytes_len, s->accu_bytes);
        c_proto(di, s->ss_text, s->es_text, s->out_python,
            "TALKER_BYTES", C_BYTES(s->accu_bytes, s->accu_bytes_len), C_END);
        s->accu_bytes_len = 0;
    }
    if (s->accu_text_len > 0 && s->ss_text != 0 && s->es_text != 0) {
        c_put(di, s->ss_text, s->es_text, s->out_ann, ANN_TEXT, s->accu_text);
        c_proto(di, s->ss_text, s->es_text, s->out_python,
            "TALKER_TEXT", C_STR(s->accu_text), C_END);
        s->accu_text_len = 0;
        s->accu_text[0] = '\0';
    }
    s->ss_text = 0;
    s->es_text = 0;
}

static void handle_ifc_change(struct srd_decoder_inst *di, ieee488_priv *s, int ifc)
{
    if (ifc) {
        s->last_talker = -1;
        s->last_listener_count = 0;
        flush_bytes_text_accu(di, s);
    }
}

static void handle_eoi_change(struct srd_decoder_inst *di, ieee488_priv *s, int eoi, uint64_t samplenum)
{
    (void)samplenum;
    if (eoi) {
        s->ss_eoi = samplenum;
        s->curr_eoi = eoi;
    } else {
        s->es_eoi = samplenum;
        if (s->ss_eoi && s->latch_eoi) {
            emit_eoi_ann(di, s, s->ss_eoi, s->es_eoi);
        }
        s->es_text = s->es_eoi;
        flush_bytes_text_accu(di, s);
        s->ss_eoi = 0;
        s->es_eoi = 0;
        s->curr_eoi = 0;
    }
}

static void handle_atn_change(struct srd_decoder_inst *di, ieee488_priv *s, int atn)
{
    s->curr_atn = atn;
    if (atn) {
        flush_bytes_text_accu(di, s);
    }
}

static void handle_iec_periph(struct srd_decoder_inst *di, ieee488_priv *s,
    uint64_t ss, uint64_t es, int addr, int sec, int data)
{
    if (s->iec_periph != 1)
        return;

    if (addr < 0 && sec < 0 && data < 0) {
        s->last_iec_addr = -1;
        s->last_iec_sec = -1;
        return;
    }

    if (addr >= 0) {
        s->last_iec_addr = addr;
        if (addr == 8) {
            c_put(di, ss, es, s->out_ann, ANN_IEC_PERIPH, "Disk 0");
        } else if (addr == 9) {
            c_put(di, ss, es, s->out_ann, ANN_IEC_PERIPH, "Disk 1");
        }
    }

    int cur_addr = (addr >= 0) ? addr : s->last_iec_addr;
    if (sec >= 0) {
        s->last_iec_sec = sec;
        int subcmd = sec & 0xf0;
        int channel = sec & 0x0f;
        if (cur_addr >= 8 && cur_addr < 16) {
            char str1[64], str2[32], str3[16];
            if (subcmd == 0x60) {
                snprintf(str1, sizeof(str1), "Reopen %d", channel);
                snprintf(str2, sizeof(str2), "Re %d", channel);
                snprintf(str3, sizeof(str3), "R%c", '0' + channel);
                c_put(di, ss, es, s->out_ann, ANN_IEC_PERIPH, str1, str2, str3);
            } else if (subcmd == 0xe0) {
                snprintf(str1, sizeof(str1), "Close %d", channel);
                snprintf(str2, sizeof(str2), "Cl %d", channel);
                snprintf(str3, sizeof(str3), "C%c", '0' + channel);
                c_put(di, ss, es, s->out_ann, ANN_IEC_PERIPH, str1, str2, str3);
            } else if (subcmd == 0xf0) {
                snprintf(str1, sizeof(str1), "Open %d", channel);
                snprintf(str2, sizeof(str2), "Op %d", channel);
                snprintf(str3, sizeof(str3), "O%c", '0' + channel);
                c_put(di, ss, es, s->out_ann, ANN_IEC_PERIPH, str1, str2, str3);
            }
        }
    }
}

static void handle_data_byte(struct srd_decoder_inst *di, ieee488_priv *s)
{
    uint8_t b = s->curr_raw;

    /* Raw byte annotation */
    char raw_text[16];
    if (s->curr_atn)
        snprintf(raw_text, sizeof(raw_text), "/%02x", b);
    else
        snprintf(raw_text, sizeof(raw_text), "%02x", b);
    c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_RAW_BYTE, raw_text);

    /* Binary raw output */
    c_put_bin(di, s->ss_raw, s->es_raw, s->out_bin, BIN_RAW, 1, &b);

    /* PROTO: GPIB_RAW */
    uint16_t raw_val = s->curr_atn ? (b | 0x100) : b;
    unsigned char proto_raw[2];
    memcpy(proto_raw, &raw_val, 2);
    c_proto(di, s->ss_raw, s->es_raw, s->out_python, "GPIB_RAW", C_U16(raw_val), C_END);

    if (s->curr_atn) {
        /* ATN active: command/address */
        int is_unl = 0, is_unt = 0;
        int is_cmd = is_command(b, &is_unl, &is_unt);
        int laddr = is_listen_addr(b);
        int taddr = is_talk_addr(b);
        int saddr = is_secondary_addr(b);
        int msb = is_msb_set(b);

        
        int upd_iec = 0;
        int iec_addr = -1, iec_sec = -1, iec_data = -1;
        const char *py_type = NULL;
        int py_addr = 0;
        int py_peers = 0;

        if (is_cmd) {
            /* Find command in table */
            const cmd_entry *entry = NULL;
            int i;
            for (i = 0; cmd_table[i].name != NULL; i++) {
                if (cmd_table[i].code == b) {
                    entry = &cmd_table[i];
                    break;
                }
            }
            if (entry) {
                c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_CMD,
                    entry->name, entry->short_name);
            } else {
                char unk_str[64], unk_short[32];
                snprintf(unk_str, sizeof(unk_str), "Unknown command 0x%02x", b);
                snprintf(unk_short, sizeof(unk_short), "cmd %02x", b);
                c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_CMD,
                    unk_str, unk_short);
                c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_WARN,
                    "Unknown GPIB command", "unknown", "UNK");
            }
            py_type = "COMMAND";
            py_addr = 0;

            if (is_unl) {
                s->last_listener_count = 0;
                py_peers = 1;
            }
            if (is_unt) {
                s->last_talker = -1;
                py_peers = 1;
            }
            if (is_unl || is_unt) {
                upd_iec = 1;
                iec_addr = -1;
                iec_sec = -1;
                iec_data = -1;
            }
        } else if (laddr >= 0) {
            char str1[32], str2[16], str3[8];
            snprintf(str1, sizeof(str1), "Listen %d", laddr);
            snprintf(str2, sizeof(str2), "L %d", laddr);
            snprintf(str3, sizeof(str3), "L%c", '0' + laddr);
            c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_LADDR, str1, str2, str3);
            py_type = "LISTEN";
            py_addr = laddr;

            if (laddr == s->last_talker)
                s->last_talker = -1;
            if (s->last_listener_count < MAX_LISTENERS)
                s->last_listener[s->last_listener_count++] = laddr;
            upd_iec = 1;
            iec_addr = laddr;
            py_peers = 1;
        } else if (taddr >= 0) {
            char str1[32], str2[16], str3[8];
            snprintf(str1, sizeof(str1), "Talk %d", taddr);
            snprintf(str2, sizeof(str2), "T %d", taddr);
            snprintf(str3, sizeof(str3), "T%c", '0' + taddr);
            c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_TADDR, str1, str2, str3);
            py_type = "TALK";
            py_addr = taddr;

            /* Remove from listeners if present */
            int i, j;
            for (i = 0; i < s->last_listener_count; i++) {
                if (s->last_listener[i] == taddr) {
                    for (j = i; j < s->last_listener_count - 1; j++)
                        s->last_listener[j] = s->last_listener[j + 1];
                    s->last_listener_count--;
                    break;
                }
            }
            s->last_talker = taddr;
            upd_iec = 1;
            iec_addr = taddr;
            py_peers = 1;
        } else if (saddr >= 0) {
            char str1[32], str2[16], str3[8];
            snprintf(str1, sizeof(str1), "Secondary %d", saddr);
            snprintf(str2, sizeof(str2), "S %d", saddr);
            snprintf(str3, sizeof(str3), "S%c", '0' + saddr);
            c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_SADDR, str1, str2, str3);
            upd_iec = 1;
            iec_sec = b;
            py_type = "SECONDARY";
            py_addr = saddr;
        } else if (msb >= 0) {
            char str1[32], str2[16], str3[8];
            snprintf(str1, sizeof(str1), "Secondary %d", msb);
            snprintf(str2, sizeof(str2), "S %d", msb);
            snprintf(str3, sizeof(str3), "S%c", '0' + (msb & 0x1f));
            c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_SADDR, str1, str2, str3);
            upd_iec = 1;
            iec_sec = b;
            py_type = "MSB_SET";
            py_addr = b;
        }

        if (upd_iec) {
            handle_iec_periph(di, s, s->ss_raw, s->es_raw, iec_addr, iec_sec, iec_data);
        }

        if (py_type) {
            unsigned char proto_data[5];
            memcpy(proto_data, &py_addr, 4);
            proto_data[4] = b;
            c_proto(di, s->ss_raw, s->es_raw, s->out_python,
                py_type, C_U32(py_addr), C_U8(b), C_END);
        }

        if (py_peers) {
            /* Sort listeners */
            int i, j;
            for (i = 0; i < s->last_listener_count - 1; i++)
                for (j = i + 1; j < s->last_listener_count; j++)
                    if (s->last_listener[i] > s->last_listener[j]) {
                        int tmp = s->last_listener[i];
                        s->last_listener[i] = s->last_listener[j];
                        s->last_listener[j] = tmp;
                    }
            /* PROTO: TALK_LISTEN */
            unsigned char tl_data[128];
            int tl_len = 0;
            int talker_val = (s->last_talker >= 0) ? s->last_talker : 0;
            memcpy(tl_data, &talker_val, 4);
            tl_len = 4;
            for (i = 0; i < s->last_listener_count && tl_len + 4 < 128; i++) {
                memcpy(tl_data + tl_len, &s->last_listener[i], 4);
                tl_len += 4;
            }
            c_proto(di, s->ss_raw, s->es_raw, s->out_python,
                "TALK_LISTEN", C_U32(talker_val), C_BYTES(tl_data + 4, tl_len - 4), C_END);
        }
    } else {
        /* ATN inactive: data byte */
        if (!s->curr_atn) {
            /* Check extra flush for EOL delimiter */
            if (s->delim_eol && s->accu_bytes_len > 0) {
                int is_eol = (b == 10 || b == 13);
                int had_eol = (s->accu_bytes_len > 0 &&
                    (s->accu_bytes[s->accu_bytes_len - 1] == 10 ||
                     s->accu_bytes[s->accu_bytes_len - 1] == 13));
                if (had_eol && !is_eol) {
                    flush_bytes_text_accu(di, s);
                }
            }
        }

        if (s->accu_bytes_len < MAX_ACCU_BYTES) {
            s->accu_bytes[s->accu_bytes_len++] = b;
        }

        char text_buf[16];
        get_data_text(b, text_buf, sizeof(text_buf));

        if (s->accu_text_len == 0)
            s->ss_text = s->ss_raw;
        if (s->accu_text_len + (int)strlen(text_buf) < MAX_ACCU_TEXT) {
            strcpy(s->accu_text + s->accu_text_len, text_buf);
            s->accu_text_len += (int)strlen(text_buf);
        }
        s->es_text = s->es_raw;

        c_put(di, s->ss_raw, s->es_raw, s->out_ann, ANN_DATA, text_buf);

        handle_iec_periph(di, s, s->ss_raw, s->es_raw, -1, -1, b);

        /* PROTO: DATA_BYTE */
        unsigned char db_data[5];
        int talker = (s->last_talker >= 0) ? s->last_talker : 0;
        memcpy(db_data, &talker, 4);
        db_data[4] = b;
        c_proto(di, s->ss_raw, s->es_raw, s->out_python,
            "DATA_BYTE", C_U32(talker), C_U8(b), C_END);
    }
}

static void handle_dav_change(struct srd_decoder_inst *di, ieee488_priv *s,
    int dav, uint8_t *data, uint64_t samplenum)
{
    (void)samplenum;
    if (dav) {
        s->ss_raw = samplenum;
        s->curr_raw = bitpack(data, 8);
        s->latch_atn = s->curr_atn;
        s->latch_eoi = s->curr_eoi;
        return;
    }
    s->es_raw = samplenum;
    handle_data_byte(di, s);
    s->ss_raw = 0;
    s->es_raw = 0;
    s->curr_raw = 0;
}

static void inject_dav_phase(struct srd_decoder_inst *di, ieee488_priv *s,
    uint64_t ss, uint64_t es, uint8_t *bits)
{
    s->ss_raw = ss;
    s->curr_raw = bitpack(bits, 8);
    s->latch_atn = s->curr_atn;
    s->latch_eoi = s->curr_eoi;
    s->es_raw = es;
    handle_data_byte(di, s);
    s->ss_raw = 0;
    s->es_raw = 0;
    s->curr_raw = 0;
}

static int invert_pin(int p)
{
    if (p == 0 || p == 1)
        return 1 - p;
    return p;
}

static void decode_serial(struct srd_decoder_inst *di, ieee488_priv *s)
{
    s->serial_state = STATE_WAIT_READY_TO_SEND;
    s->serial_bit_count = 0;

    while (1) {
        int ret;
        switch (s->serial_state) {
        case STATE_WAIT_READY_TO_SEND:
            ret = c_wait(di, CW_F(PIN_ATN), CW_OR, CW_L(PIN_DATA), CW_H(PIN_CLK), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1ULL << 0)) {
                /* ATN falling edge, reset */
                s->serial_state = STATE_WAIT_READY_TO_SEND;
            }
            if (di_matched(di) & (1ULL << 1)) {
                int data = c_pin(di, PIN_DATA);
                int clk = c_pin(di, PIN_CLK);
                if (data == 0 && clk == 1) {
                    s->serial_state = STATE_WAIT_READY_FOR_DATA;
                }
            }
            break;

        case STATE_WAIT_READY_FOR_DATA:
            ret = c_wait(di, CW_F(PIN_ATN), CW_OR, CW_H(PIN_DATA), CW_H(PIN_CLK), CW_OR, CW_L(PIN_CLK), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1ULL << 0)) {
                s->serial_state = STATE_WAIT_READY_TO_SEND;
                break;
            }
            if (di_matched(di) & (1ULL << 1)) {
                int data = c_pin(di, PIN_DATA);
                int clk = c_pin(di, PIN_CLK);
                if (data == 1 && clk == 1) {
                    s->ss_byte = di_samplenum(di);
                    int atn = invert_pin(c_pin(di, PIN_ATN));
                    handle_atn_change(di, s, atn);
                    if (s->curr_eoi)
                        handle_eoi_change(di, s, 0, di_samplenum(di));
                    s->serial_bit_count = 0;
                    s->serial_state = STATE_PREP_DATA_TEST_EOI;
                } else if (clk == 0) {
                    s->serial_state = STATE_WAIT_READY_TO_SEND;
                }
            }
            if (di_matched(di) & (1ULL << 2)) {
                s->serial_state = STATE_WAIT_READY_TO_SEND;
            }
            break;

        case STATE_PREP_DATA_TEST_EOI:
            ret = c_wait(di, CW_F(PIN_ATN), CW_OR, CW_F(PIN_DATA), CW_OR, CW_L(PIN_CLK), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1ULL << 0)) {
                s->serial_state = STATE_WAIT_READY_TO_SEND;
                break;
            }
            if (di_matched(di) & (1ULL << 1)) {
                int data = c_pin(di, PIN_DATA);
                int clk = c_pin(di, PIN_CLK);
                if (data == 0 && clk == 1) {
                    handle_eoi_change(di, s, 1, di_samplenum(di));
                }
            }
            if (di_matched(di) & (1ULL << 2)) {
                s->serial_state = STATE_CLOCK_DATA_BITS;
                s->ss_bit = di_samplenum(di);
            }
            break;

        case STATE_CLOCK_DATA_BITS: {
            ret = c_wait(di, CW_F(PIN_ATN), CW_OR, CW_E(PIN_CLK), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1ULL << 0)) {
                s->serial_state = STATE_WAIT_READY_TO_SEND;
                break;
            }
            if (di_matched(di) & (1ULL << 1)) {
                int clk = c_pin(di, PIN_CLK);
                    if (clk == 1) {
                        /* Rising edge: latch DATA */
                        int data = c_pin(di, PIN_DATA);
                    if (s->serial_bit_count < 8)
                        s->serial_bits[s->serial_bit_count] = data;
                } else {
                    /* Falling edge: end of bit */
                    uint64_t es_bit = di_samplenum(di);
                    char bit_str[16];
                    snprintf(bit_str, sizeof(bit_str), "%d", s->serial_bits[s->serial_bit_count]);
                    c_put(di, s->ss_bit, es_bit, s->out_ann, ANN_BIT, bit_str);

                    /* PROTO: IEC_BIT */
                    
                    c_proto(di, s->ss_bit, es_bit, s->out_python,
                        "IEC_BIT", C_U8(s->serial_bits[s->serial_bit_count]), C_END);

                    s->ss_bit = di_samplenum(di);
                    s->serial_bit_count++;

                    if (s->serial_bit_count == 8) {
                        uint64_t es_byte = di_samplenum(di);
                        inject_dav_phase(di, s, s->ss_byte, es_byte, s->serial_bits);
                        if (s->curr_eoi)
                            handle_eoi_change(di, s, 0, di_samplenum(di));
                        s->serial_state = STATE_WAIT_READY_TO_SEND;
                    }
                }
            }
            break;
        }
        }
    }
}

static void decode_parallel(struct srd_decoder_inst *di, ieee488_priv *s)
{
    s->parallel_first_pass = 1;

    while (1) {
        int ret;
        int i;

        if (s->parallel_first_pass)
            ret = c_wait(di, CW_L(PIN_DAV), CW_END);
        else
            ret = c_wait(di, CW_E(PIN_DAV), CW_OR, CW_END);
        if (s->parallel_first_pass)
            ret = c_wait(di, CW_L(PIN_ATN), CW_END);
        else
            ret = c_wait(di, CW_E(PIN_ATN), CW_END);
        if (s->has_eoi)
            ret = c_wait(di, CW_OR, CW_END);
        if (s->parallel_first_pass)
            ret = c_wait(di, CW_L(PIN_EOI), CW_END);
        else
            ret = c_wait(di, CW_E(PIN_EOI), CW_END);
        if (s->has_ifc)
            ret = c_wait(di, CW_OR, CW_END);
        if (s->parallel_first_pass)
            ret = c_wait(di, CW_L(PIN_IFC), CW_END);
        else
            ret = c_wait(di, CW_E(PIN_IFC), CW_END);
        if (ret != SRD_OK)
            return;

        /* Read all pins and invert */
        int pins[17];
        for (i = 0; i < 17; i++) {
            int p = c_pin(di, i);
            pins[i] = invert_pin(p);
        }

        /* Process in order (important for same-sample edges) */
        if (s->has_ifc && (di_matched(di) & (1ULL << s->idx_ifc)) && pins[PIN_IFC] == 1)
            handle_ifc_change(di, s, pins[PIN_IFC]);
        if (s->has_eoi && (di_matched(di) & (1ULL << s->idx_eoi)) && pins[PIN_EOI] == 1)
            handle_eoi_change(di, s, pins[PIN_EOI], di_samplenum(di));
        if ((di_matched(di) & (1ULL << s->idx_atn)) && pins[PIN_ATN] == 1)
            handle_atn_change(di, s, pins[PIN_ATN]);
        if (di_matched(di) & (1ULL << s->idx_dav))
            handle_dav_change(di, s, pins[PIN_DAV], (uint8_t *)&pins[PIN_DIO1], di_samplenum(di));
        if ((di_matched(di) & (1ULL << s->idx_atn)) && pins[PIN_ATN] == 0)
            handle_atn_change(di, s, pins[PIN_ATN]);
        if (s->has_eoi && (di_matched(di) & (1ULL << s->idx_eoi)) && pins[PIN_EOI] == 0)
            handle_eoi_change(di, s, pins[PIN_EOI], di_samplenum(di));
        if (s->has_ifc && (di_matched(di) & (1ULL << s->idx_ifc)) && pins[PIN_IFC] == 0)
            handle_ifc_change(di, s, pins[PIN_IFC]);

        s->parallel_first_pass = 0;
    }
}

static void ieee488_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    (void)di; (void)key; (void)value;
    /* No samplerate needed */
}

static void ieee488_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ieee488_priv)));
    }
    ieee488_priv *s = (ieee488_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ieee488_priv));
    s->last_talker = -1;
    s->last_iec_addr = -1;
    s->last_iec_sec = -1;
}

static void ieee488_start(struct srd_decoder_inst *di)
{
    ieee488_priv *s = (ieee488_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ieee488");
    s->out_bin = c_reg_out(di, SRD_OUTPUT_BINARY, "ieee488");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "ieee488");

    const char *iec_periph = c_opt_str(di, "iec_periph", "no");
    s->iec_periph = (iec_periph && strcmp(iec_periph, "yes") == 0) ? 1 : 0;

    const char *delim = c_opt_str(di, "delim", "eol");
    s->delim_eol = (delim && strcmp(delim, "eol") == 0) ? 1 : 0;

    /* Check channel availability */
    s->has_clk = c_has_ch(di, PIN_CLK);
    s->has_dio8 = c_has_ch(di, PIN_DIO8);
    s->has_dav = c_has_ch(di, PIN_DAV);
    s->has_atn = c_has_ch(di, PIN_ATN);
    s->has_eoi = c_has_ch(di, PIN_EOI);
    s->has_ifc = c_has_ch(di, PIN_IFC);
    s->has_srq = c_has_ch(di, PIN_SRQ);

    /* Check all 8 DIO lines are connected */
    s->has_all_dio = 1;
    for (int i = 0; i < 8; i++) {
        if (!c_has_ch(di, i)) {
            s->has_all_dio = 0;
            break;
        }
    }

    s->is_serial = s->has_clk;
}


static void ieee488_decode(struct srd_decoder_inst *di)
{
    ieee488_priv *s = (ieee488_priv *)c_decoder_get_private(di);

    if (s->is_serial) {
        if (!s->has_atn)
            return;
        decode_serial(di, s);
    } else if (s->has_dav && s->has_atn && s->has_all_dio) {
        decode_parallel(di, s);
    }
}

static void ieee488_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ieee488_c_decoder = {
    .id = "ieee488_c",
    .name = "IEEE-488(C)",
    .longname = "IEEE-488 GPIB/HPIB/IEC (C)",
    .desc = "IEEE-488 General Purpose Interface Bus (GPIB/HPIB or IEC) decoder (C implementation)",
    .license = "gplv2+",
    .channels = ieee488_channels,
    .num_channels = 1,
    .optional_channels = ieee488_optional_channels,
    .num_optional_channels = 16,
    .options = ieee488_options_arr,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = ieee488_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = ieee488_ann_rows,
    .inputs = ieee488_inputs,
    .num_inputs = 1,
    .outputs = ieee488_outputs,
    .num_outputs = 1,
    .binary = ieee488_binary,
    .num_binary = NUM_BIN,
    .tags = ieee488_tags,
    .num_tags = 2,
    .metadata = ieee488_metadata,
    .reset = ieee488_reset,
    .start = ieee488_start,
    .decode = ieee488_decode,
    .destroy = ieee488_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *iec_periph_vals[] = {
        g_variant_new_string("no"),
        g_variant_new_string("yes"),
    };
    GSList *iec_periph_list = NULL;
    iec_periph_list = g_slist_append(iec_periph_list, iec_periph_vals[0]);
    iec_periph_list = g_slist_append(iec_periph_list, iec_periph_vals[1]);
    ieee488_options_arr[0].id = "iec_periph";
    ieee488_options_arr[0].idn = "dec_ieee488_opt_iec_periph";
    ieee488_options_arr[0].desc = "Decode Commodore IEC bus peripherals details";
    ieee488_options_arr[0].def = g_variant_new_string("no");
    ieee488_options_arr[0].values = iec_periph_list;

    GVariant *delim_vals[] = {
        g_variant_new_string("none"),
        g_variant_new_string("eol"),
    };
    GSList *delim_list = NULL;
    delim_list = g_slist_append(delim_list, delim_vals[0]);
    delim_list = g_slist_append(delim_list, delim_vals[1]);
    ieee488_options_arr[1].id = "delim";
    ieee488_options_arr[1].idn = "dec_ieee488_opt_delim";
    ieee488_options_arr[1].desc = "Payload data delimiter";
    ieee488_options_arr[1].def = g_variant_new_string("eol");
    ieee488_options_arr[1].values = delim_list;

    return &ieee488_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}