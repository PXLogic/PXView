/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010-2016 Uwe Hermann <uwe@hermann-uwe.de>
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

static void fmt_binary(uint8_t val, char *buf, int bufsize) {
    if (bufsize < 9) { buf[0] = '\0'; return; }
    for (int i = 7; i >= 0; i--) buf[7 - i] = (val & (1 << i)) ? '1' : '0';
    buf[8] = '\0';
}
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Channel indices — match Python: SCL=0, SDA=1 */
#define SCL 0
#define SDA 1

/* Annotation class indices — match Python proto dict / annotations tuple */
enum i2c_ann {
    ANN_START         = 0,
    ANN_REPEAT_START  = 1,
    ANN_STOP          = 2,
    ANN_ACK           = 3,
    ANN_NACK          = 4,
    ANN_BIT           = 5,
    ANN_ADDRESS_READ  = 6,
    ANN_ADDRESS_WRITE = 7,
    ANN_DATA_READ     = 8,
    ANN_DATA_WRITE    = 9,
    ANN_PACKET        = 10,
    ANN_ATK_DATA      = 11,
    ANN_ATK_RISE      = 12,
    NUM_ANN,
};

/* State machine — 1:1 with Python self.state */
enum i2c_state {
    STATE_FIND_START,
    STATE_FIND_ADDRESS,
    STATE_FIND_DATA,
    STATE_FIND_ACK,
};

/* Per-bit entry for the BITS protocol output */
typedef struct {
    int sda;
    uint64_t ss;
    uint64_t es;
} i2c_bit_entry;

/* Decoder state struct — C_DECODER_STATE auto-generates i2c_s typedef.
 * We provide custom reset/destroy (i2c_reset_impl / i2c_destroy_impl)
 * that properly handle the GArray, so the auto-generated ones are unused.
 * Suppress the unused-function warnings for the auto-generated versions. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
C_DECODER_STATE(i2c, {
    enum i2c_state state;
    int bitcount;
    uint8_t databyte;
    uint64_t ss_byte;
    int wr;
    int is_repeat_start;
    uint64_t bitwidth;
    uint64_t pdu_start;
    int pdu_bits;
    uint64_t packet_ss;
    uint64_t packet_es;
    uint64_t packet_part_ss;
    uint8_t address;
    GArray *packet_data;
    char packet_str[2048];
    char packet_str_short[2048];
    i2c_bit_entry bits[8];
    int out_ann;
    int out_binary;
    int out_python;
    int out_bitrate;
    int address_shifted;
    int show_data_point;
    int packets_format; /* 0=none, 1=hex, 2=ascii, 3=dec, 4=bin, 5=oct */
    uint64_t samplerate;
});
#pragma GCC diagnostic pop

/*
 * Override the auto-generated i2c_reset: we need to free the old GArray
 * before calloc replaces the struct, and set non-zero defaults (wr=-1).
 * The C_DECODER_STATE macro already generated a basic i2c_reset; we
 * supplement it by handling the GArray and non-zero fields in i2c_start()
 * and by freeing the GArray in i2c_destroy_override() below.
 *
 * Since the auto-generated reset does calloc(1,...) which zero-inits:
 *   state = 0 = STATE_FIND_START  ✓
 *   is_repeat_start = 0            ✓
 *   wr = 0 (should be -1)          → fixed in i2c_start()
 *   packet_data = NULL             → allocated in i2c_start()
 *   address_shifted = 0            → set in i2c_start()
 *   show_data_point = 0            → set in i2c_start()
 *
 * The auto-generated destroy uses free(), which doesn't free the GArray.
 * We handle that by freeing it in start() on re-init, and the destroy
 * is handled via a wrapper approach.
 */

/* Channel definitions — match Python channels tuple */
static struct srd_channel i2c_channels[] = {
    {"scl", "SCL", "Serial clock line(串行时钟线)", 0, SRD_CHANNEL_SCLK, NULL},
    {"sda", "SDA", "Serial data line(串行数据线)", 1, SRD_CHANNEL_SDATA, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *i2c_ann_labels[][3] = {
    {"", "Start",         "Start condition"},
    {"", "Start repeat",  "Repeat start condition"},
    {"", "Stop",          "Stop condition"},
    {"", "ACK",           "ACK"},
    {"", "NACK",          "NACK"},
    {"", "Bit",           "Data/address bit"},
    {"", "Address read",  "Address read"},
    {"", "Address write", "Address write"},
    {"", "Data read",     "Data read"},
    {"", "Data write",    "Data write"},
    {"", "Packet",        "Packet"},
    {"", "ATK Data point",  "ATK Data point"},
    {"", "ATK Rising edge", "ATK Rising edge"},
};

/* Annotation row class lists */
static const int i2c_row_bits_classes[] = {ANN_BIT, -1};
static const int i2c_row_addr_classes[] = {
    ANN_START, ANN_REPEAT_START, ANN_STOP, ANN_ACK, ANN_NACK,
    ANN_ADDRESS_READ, ANN_ADDRESS_WRITE, ANN_DATA_READ, ANN_DATA_WRITE, -1
};
static const int i2c_row_pkt_classes[] = {ANN_PACKET, -1};
static const int i2c_row_atk_classes[] = {ANN_ATK_DATA, ANN_ATK_RISE, -1};

/* Annotation rows — match Python annotation_rows tuple */
static const struct srd_c_ann_row i2c_ann_rows[] = {
    {"bits",       "Bits",          i2c_row_bits_classes, 1},
    {"addr-data",  "Address/data",  i2c_row_addr_classes, 9},
    {"packets",    "Packets",       i2c_row_pkt_classes,  1},
    {"atk-signs",  "ATK signs",     i2c_row_atk_classes,  2},
};

/* Options — match Python options tuple */
static struct srd_decoder_option i2c_options[] = {
    {"address_format",  NULL, "Displayed slave address format(从地址格式)",  NULL, NULL},
    {"packets_format",  NULL, "Display packets(数据格式)",                  NULL, NULL},
    {"show_data_point", NULL, "Show data point(数据点显示)",                NULL, NULL},
};

/* Binary output classes — match Python binary tuple */
static const struct srd_decoder_binary i2c_binary[] = {
    {0, "address-read",  "Address read"},
    {1, "address-write", "Address write"},
    {2, "data-read",     "Data read"},
    {3, "data-write",    "Data write"},
};

static const char *i2c_inputs[] = {"logic", NULL};
static const char *i2c_outputs[] = {"i2c", NULL};
static const char *i2c_tags[] = {"Embedded/industrial", NULL};

/* ---- Custom reset: frees old GArray, then uses calloc like auto-generated ---- */
static void i2c_reset_impl(struct srd_decoder_inst *di)
{
    i2c_s *old = (i2c_s *)c_decoder_get_private(di);
    if (old) {
        if (old->packet_data)
            g_array_free(old->packet_data, TRUE);
        free(old);
        c_decoder_set_private(di, NULL);
    }
    i2c_s *s = (i2c_s *)calloc(1, sizeof(i2c_s));
    c_decoder_set_private(di, s);
    /* calloc zeros everything, so:
     *   state = 0 = STATE_FIND_START  ✓
     *   is_repeat_start = 0            ✓
     * Non-zero defaults set in i2c_start():
     *   wr = -1, address_shifted = 1, show_data_point = 1, packet_data allocated
     */
}

/* ---- Custom destroy: frees GArray and struct ---- */
static void i2c_destroy_impl(struct srd_decoder_inst *di)
{
    i2c_s *s = (i2c_s *)c_decoder_get_private(di);
    if (s) {
        if (s->packet_data)
            g_array_free(s->packet_data, TRUE);
        free(s);
        c_decoder_set_private(di, NULL);
    }
}

/* ---- Helper: reset packet state ---- */
static void i2c_reset_packet(i2c_s *s)
{
    if (s->packet_data)
        g_array_set_size(s->packet_data, 0);
    s->packet_str[0] = '\0';
    s->packet_str_short[0] = '\0';
    s->packet_ss = 0;
    s->packet_es = 0;
    s->packet_part_ss = 0;
    s->address = 0;
}

/* ---- Helper: format a single data value for ascii display ---- */
static void i2c_format_data_value(uint8_t v, char *out, int out_size)
{
    if (v >= 32 && v <= 126)
        snprintf(out, out_size, "%c", v);
    else
        snprintf(out, out_size, "[%02X]", v);
}

/* ---- Helper: convert packet_data array to string ---- */
static void i2c_data_array_to_str(i2c_s *s, char *out, int out_size)
{
    out[0] = '\0';
    int pos = 0;
    for (guint i = 0; i < s->packet_data->len && pos < out_size - 8; i++) {
        uint8_t v = g_array_index(s->packet_data, uint8_t, i);
        char tmp[16];
        if (s->packets_format == 5) { /* oct */
            snprintf(tmp, sizeof(tmp), "%03o", v);
        } else if (s->packets_format == 4) { /* bin */
            fmt_binary(v, tmp, sizeof(tmp));
        } else if (s->packets_format == 3) { /* dec */
            snprintf(tmp, sizeof(tmp), "%d", v);
        } else if (s->packets_format == 2) { /* ascii */
            i2c_format_data_value(v, tmp, sizeof(tmp));
        } else { /* hex (1) or fallback */
            snprintf(tmp, sizeof(tmp), "%02X", v);
        }
        if (i > 0 && pos < out_size - 2)
            out[pos++] = ' ';
        int len = (int)strlen(tmp);
        if (pos + len >= out_size) break;
        memcpy(out + pos, tmp, len);
        pos += len;
    }
    out[pos] = '\0';
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ---- Helper: format full packet string ---- */
static void i2c_format_packet(i2c_s *s, char *pkt_str, int pkt_str_size,
                               char *pkt_short, int pkt_short_size)
{
    char data_str[128];
    i2c_data_array_to_str(s, data_str, sizeof(data_str));

    snprintf(pkt_str, pkt_str_size, "0x%02X %s: %s",
             s->address,
             (s->wr == 0) ? "RD" : "WR",
             data_str);

    snprintf(pkt_short, pkt_short_size, "%s", pkt_str + 2);

    if (s->packet_str[0]) {
        char full[4096];
        char full_short[4096];
        snprintf(full, sizeof(full), "%s [SR] %s", s->packet_str, pkt_str);
        snprintf(full_short, sizeof(full_short), "%s [SR] %s", s->packet_str_short, pkt_short);
        snprintf(pkt_str, pkt_str_size, "%s", full);
        snprintf(pkt_short, pkt_short_size, "%s", full_short);
    }
}

/* ---- Helper: handle packet annotation output ---- */
static void i2c_handle_packet(struct srd_decoder_inst *di, i2c_s *s, int is_start_repeat)
{
    if (s->packets_format == 0) /* none */
        return;

    if (s->packet_data->len == 0) {
        if (!is_start_repeat)
            i2c_reset_packet(s);
        return;
    }

    char pkt_str[4096];
    char pkt_short[4096];
    i2c_format_packet(s, pkt_str, sizeof(pkt_str), pkt_short, sizeof(pkt_short));

    if (is_start_repeat) {
        g_array_set_size(s->packet_data, 0);
        snprintf(s->packet_str, sizeof(s->packet_str), "%s", pkt_str);
        snprintf(s->packet_str_short, sizeof(s->packet_str_short), "%s", pkt_short);
    } else {
        c_put(di, s->packet_ss, s->packet_es, s->out_ann, ANN_PACKET, pkt_str, pkt_short);
        i2c_reset_packet(s);
    }
}

#pragma GCC diagnostic pop

/* ---- Python: handle_start ---- */
static void i2c_handle_start(struct srd_decoder_inst *di, i2c_s *s)
{
    uint64_t samplenum = di_samplenum(di);

    s->pdu_start = samplenum;
    s->pdu_bits = 0;

    if (s->is_repeat_start == 1) {
        c_put(di, samplenum, samplenum, s->out_ann, ANN_REPEAT_START, "Start repeat", "Sr");
        c_proto(di, samplenum, samplenum, s->out_python, "START REPEAT", C_END);
        i2c_handle_packet(di, s, 1);
    } else {
        c_put(di, samplenum, samplenum, s->out_ann, ANN_START, "Start", "S");
        c_proto(di, samplenum, samplenum, s->out_python, "START", C_END);
        i2c_handle_packet(di, s, 0);
        s->packet_ss = samplenum;
    }
    s->packet_part_ss = samplenum;

    s->state = STATE_FIND_ADDRESS;
    s->bitcount = 0;
    s->databyte = 0;
    s->is_repeat_start = 1;
    s->wr = -1;
}

/* ---- Python: handle_address_or_data ---- */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void i2c_handle_address_or_data(struct srd_decoder_inst *di, i2c_s *s)
{
    uint64_t samplenum = di_samplenum(di);
    int sda_val = c_pin(di, SDA);

    s->pdu_bits++;

    /* Address and data are transmitted MSB-first */
    s->databyte = (s->databyte << 1) | sda_val;

    /* Remember the start of the first data/address bit */
    if (s->bitcount == 0)
        s->ss_byte = samplenum;

    /* Store individual bits — index 0 = LSB (I2C transmits MSB-first),
     * so we insert at position 0 and shift the rest up, matching Python's
     * self.bits.insert(0, [sda, samplenum, samplenum]) */
    for (int i = 7; i > 0; i--)
        s->bits[i] = s->bits[i - 1];
    s->bits[0].sda = sda_val;
    s->bits[0].ss = samplenum;
    s->bits[0].es = samplenum;

    if (s->bitcount > 0)
        s->bits[1].es = samplenum;

    if (s->bitcount == 7) {
        s->bitwidth = s->bits[1].es - s->bits[2].es;
        s->bits[0].es += s->bitwidth;
    }

    /* Return if we haven't collected all 8 bits yet */
    if (s->bitcount < 7) {
        s->bitcount++;
        return;
    }

    uint8_t d = s->databyte;
    if (s->state == STATE_FIND_ADDRESS) {
        /* The READ/WRITE bit is only in address bytes */
        s->wr = (d & 1) ? 0 : 1;
        if (s->address_shifted)
            d = d >> 1;
    }

    uint64_t byte_end = samplenum + s->bitwidth;
    int bin_class = -1;
    int ann_class = -1;

    if (s->state == STATE_FIND_ADDRESS && s->wr == 1) {
        ann_class = ANN_ADDRESS_WRITE;
        bin_class = 1;
    } else if (s->state == STATE_FIND_ADDRESS && s->wr == 0) {
        ann_class = ANN_ADDRESS_READ;
        bin_class = 0;
    } else if (s->state == STATE_FIND_DATA) {
        if (s->wr == 1) {
            ann_class = ANN_DATA_WRITE;
            bin_class = 3;
        } else {
            ann_class = ANN_DATA_READ;
            bin_class = 2;
        }
        g_array_append_val(s->packet_data, d);
    }

    s->packet_es = byte_end;

    /* Protocol output: BITS */
    {
        unsigned char bits_data[144];
        int bpos = 0;

        bits_data[bpos++] = 0x02; /* have_miso=1, have_mosi=0 */
        bits_data[bpos++] = 0;    /* mosi_bit_count = 0 */
        bits_data[bpos++] = 0x00; /* reserved */
        bits_data[bpos++] = 8;    /* miso_bit_count = 8 */

        /* Output bits in wire order: bits[7] (MSB, first received) to bits[0] (LSB) */
        for (int i = 7; i >= 0 && bpos + 17 <= (int)sizeof(bits_data); i--) {
            bits_data[bpos++] = (unsigned char)s->bits[i].sda;
            uint64_t ss_val = s->bits[i].ss;
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
            uint64_t es_val = s->bits[i].es;
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
        }

        c_proto(di, s->ss_byte, byte_end, s->out_python, "BITS",
                C_BYTES(bits_data, bpos), C_END);
    }

    /* Protocol output: ADDRESS READ/WRITE or DATA READ/WRITE */
    if (s->state == STATE_FIND_ADDRESS) {
        c_proto(di, s->ss_byte, byte_end, s->out_python,
                s->wr ? "ADDRESS WRITE" : "ADDRESS READ",
                C_U8(d), C_END);
    } else if (s->state == STATE_FIND_DATA) {
        c_proto(di, s->ss_byte, byte_end, s->out_python,
                s->wr ? "DATA WRITE" : "DATA READ",
                C_U8(d), C_END);
    }

    /* Binary output */
    if (bin_class >= 0) {
        c_put_bin(di, s->ss_byte, byte_end, s->out_binary, bin_class, 1, &d);
    }

    /* Per-bit annotations — match Python:
     *   for bit in self.bits:
     *       self.put(bit[1], bit[2], self.out_ann, [5, ['%d' % bit[0]]]) */
    for (int i = 0; i < 8; i++) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->bits[i].sda);
        c_put(di, s->bits[i].ss, s->bits[i].es, s->out_ann, ANN_BIT, bit_str);
        if (s->show_data_point) {
            c_put(di, s->bits[i].ss, s->bits[i].ss, s->out_ann, ANN_ATK_DATA, "1");
            c_put(di, s->bits[i].ss, s->bits[i].ss, s->out_ann, ANN_ATK_RISE, "0");
        }
    }

    /* Address/data annotation */
    if (ann_class >= 0) {
        char val_str[16];

        if (ann_class == ANN_ADDRESS_WRITE || ann_class == ANN_ADDRESS_READ) {
            /* Python: self.put(samplenum, samplenum+bitwidth, ..., [proto[cmd][0], w])
             * where w = ['Write', 'Wr', 'W'] or ['Read', 'Rd', 'R'] */
            const char *w_long, *w_short, *w_tiny;
            if (s->wr) {
                w_long = "Write"; w_short = "Wr"; w_tiny = "W";
            } else {
                w_long = "Read"; w_short = "Rd"; w_tiny = "R";
            }
            c_put(di, samplenum, byte_end, s->out_ann, ann_class, w_long, w_short, w_tiny);

            if (s->show_data_point) {
                c_put(di, samplenum, samplenum, s->out_ann, ANN_ATK_DATA, "1");
                c_put(di, samplenum, samplenum, s->out_ann, ANN_ATK_RISE, "0");
            }

            /* Python: self.put(ss_byte, samplenum, ..., [proto[cmd][0], format_data(d, cmd, is_address=True)]) */
            snprintf(val_str, sizeof(val_str), "%02X", d);
            char long_str[64];
            char short_str[64];
            snprintf(long_str, sizeof(long_str), "%s: %s",
                     (ann_class == ANN_ADDRESS_WRITE) ? "Address write" : "Address read", val_str);
            snprintf(short_str, sizeof(short_str), "%s: %s",
                     (ann_class == ANN_ADDRESS_WRITE) ? "AW" : "AR", val_str);
            c_put_v(di, s->ss_byte, samplenum, s->out_ann, ann_class, d, long_str, short_str, val_str);

            s->address = d;
        } else {
            /* Data: format_data(d, cmd, is_address=False) */
            if (s->packets_format == 5) { /* oct */
                snprintf(val_str, sizeof(val_str), "%03o", d);
            } else if (s->packets_format == 4) { /* bin */
                fmt_binary(d, val_str, sizeof(val_str));
            } else if (s->packets_format == 3) { /* dec */
                snprintf(val_str, sizeof(val_str), "%d", d);
            } else if (s->packets_format == 2) { /* ascii */
                if (d >= 32 && d <= 126)
                    snprintf(val_str, sizeof(val_str), "%c", d);
                else
                    snprintf(val_str, sizeof(val_str), "[%02X]", d);
            } else { /* hex */
                snprintf(val_str, sizeof(val_str), "%02X", d);
            }
            char long_str[64];
            char short_str[64];
            snprintf(long_str, sizeof(long_str), "%s: %s",
                     (ann_class == ANN_DATA_WRITE) ? "Data write" : "Data read", val_str);
            snprintf(short_str, sizeof(short_str), "%s: %s",
                     (ann_class == ANN_DATA_WRITE) ? "DW" : "DR", val_str);
            c_put_v(di, s->ss_byte, byte_end, s->out_ann, ann_class, d, long_str, short_str, val_str);
        }
    }

    s->bitcount = 0;
    s->databyte = 0;
    s->state = STATE_FIND_ACK;
}
#pragma GCC diagnostic pop

/* ---- Python: get_ack ---- */
static void i2c_get_ack(struct srd_decoder_inst *di, i2c_s *s)
{
    uint64_t samplenum = di_samplenum(di);
    int sda_val = c_pin(di, SDA);
    uint64_t ack_end = samplenum + s->bitwidth;

    s->packet_es = ack_end;

    if (sda_val == 0) {
        c_put(di, samplenum, ack_end, s->out_ann, ANN_ACK, "ACK", "A");
        c_proto(di, samplenum, ack_end, s->out_python, "ACK", C_END);
    } else {
        c_put(di, samplenum, ack_end, s->out_ann, ANN_NACK, "NACK", "N");
        c_proto(di, samplenum, ack_end, s->out_python, "NACK", C_END);
    }

    s->state = STATE_FIND_DATA;
}

/* ---- Python: handle_stop ---- */
static void i2c_handle_stop(struct srd_decoder_inst *di, i2c_s *s)
{
    uint64_t samplenum = di_samplenum(di);

    /* Meta bitrate */
    if (s->samplerate) {
        double elapsed = 1.0 / (double)s->samplerate * (double)(samplenum - s->pdu_start + 1);
        int bitrate = (int)(1.0 / elapsed * s->pdu_bits);
        c_put_meta_int(di, s->ss_byte, samplenum, s->out_bitrate, bitrate);
    }

    s->packet_es = samplenum;
    i2c_handle_packet(di, s, 0);

    c_put(di, samplenum, samplenum, s->out_ann, ANN_STOP, "Stop", "P");
    c_proto(di, samplenum, samplenum, s->out_python, "STOP", C_END);

    s->state = STATE_FIND_START;
    s->is_repeat_start = 0;
    s->wr = -1;
}

/* ---- start callback ---- */
static void i2c_start(struct srd_decoder_inst *di)
{
    i2c_s *s = (i2c_s *)c_decoder_get_private(di);

    s->out_ann     = c_reg_out(di, SRD_OUTPUT_ANN, "i2c");
    s->out_binary  = c_reg_out(di, SRD_OUTPUT_BINARY, "i2c");
    s->out_python  = c_reg_out(di, SRD_OUTPUT_PROTO, "i2c");
    s->out_bitrate = c_reg_out(di, SRD_OUTPUT_META, "i2c");

    s->address_shifted = (strcmp(c_opt_str(di, "address_format", "shifted"), "shifted") == 0);
    s->show_data_point = c_opt_bool(di, "show_data_point", 1);

    const char *pkt_fmt = c_opt_str(di, "packets_format", "hex");
    if (pkt_fmt && strcmp(pkt_fmt, "none") == 0)
        s->packets_format = 0;
    else if (pkt_fmt && strcmp(pkt_fmt, "hex") == 0)
        s->packets_format = 1;
    else if (pkt_fmt && strcmp(pkt_fmt, "ascii") == 0)
        s->packets_format = 2;
    else if (pkt_fmt && strcmp(pkt_fmt, "dec") == 0)
        s->packets_format = 3;
    else if (pkt_fmt && strcmp(pkt_fmt, "bin") == 0)
        s->packets_format = 4;
    else if (pkt_fmt && strcmp(pkt_fmt, "oct") == 0)
        s->packets_format = 5;
    else
        s->packets_format = 1;

    /* Allocate packet_data GArray (may already exist after reset) */
    if (!s->packet_data)
        s->packet_data = g_array_new(FALSE, FALSE, sizeof(uint8_t));

    /* Set non-zero defaults that calloc didn't initialize */
    s->wr = -1;

    s->samplerate = c_samplerate(di);
}

/* ---- metadata callback ---- */
static void i2c_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    i2c_s *s = (i2c_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* ---- decode callback — main state machine ---- */
static void i2c_decode(struct srd_decoder_inst *di)
{
    i2c_s *s = (i2c_s *)c_decoder_get_private(di);

    /* Emit ATK color styling annotations at start, matching Python's
     * self.put(self.ss, self.ss, self.out_ann, [11,["color:#4edc44"]]) */
    {
        uint64_t init_ss = (uint64_t)-1; /* Match Python's self.ss = -1 from reset() */
        c_put(di, init_ss, init_ss, s->out_ann, ANN_ATK_DATA, "color:#4edc44");
        c_put(di, init_ss, init_ss, s->out_ann, ANN_ATK_RISE, "color:#4edc44");
    }

    while (1) {
        int ret;

        switch (s->state) {

        case STATE_FIND_START:
            /* Python: self.wait({0: 'h', 1: 'f'})  — SCL high, SDA falling */
            ret = c_wait(di, CW_H(SCL), CW_F(SDA), CW_END);
            if (ret != SRD_OK)
                return;
            i2c_handle_start(di, s);
            break;

        case STATE_FIND_ADDRESS:
            /* Python: self.wait({0: 'r'})  — SCL rising */
            ret = c_wait(di, CW_R(SCL), CW_END);
            if (ret != SRD_OK)
                return;
            i2c_handle_address_or_data(di, s);
            break;

        case STATE_FIND_DATA:
            /* Python: self.wait([{0:'r'}, {0:'h',1:'f'}, {0:'h',1:'r'}])
             * SCL rising OR (SCL high, SDA falling) OR (SCL high, SDA rising) */
            ret = c_wait(di, CW_R(SCL), CW_OR, CW_H(SCL), CW_F(SDA), CW_OR, CW_H(SCL), CW_R(SDA), CW_END);
            if (ret != SRD_OK)
                return;

            /* Python: if self.matched & (0b1 << 0): ... */
            if (di_matched(di) & (1ULL << 0)) {
                i2c_handle_address_or_data(di, s);
            } else if (di_matched(di) & (1ULL << 1)) {
                i2c_handle_start(di, s);
            } else if (di_matched(di) & (1ULL << 2)) {
                i2c_handle_stop(di, s);
            }
            break;

        case STATE_FIND_ACK:
            /* Python: self.wait({0: 'r'})  — SCL rising */
            ret = c_wait(di, CW_R(SCL), CW_END);
            if (ret != SRD_OK)
                return;
            i2c_get_ack(di, s);
            break;
        }
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder i2c_c_def = {
    .id = "i2c_c",
    .name = "I²C(C)",
    .longname = "Inter-Integrated Circuit",
    .desc = "I2C serial bus protocol",
    .license = "gplv2+",
    .channels = i2c_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = i2c_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = i2c_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = i2c_ann_rows,
    .inputs = i2c_inputs,
    .num_inputs = 1,
    .outputs = i2c_outputs,
    .num_outputs = 1,
    .binary = i2c_binary,
    .num_binary = 4,
    .tags = i2c_tags,
    .num_tags = 1,
    .state_size = sizeof(i2c_s),
    .reset = i2c_reset_impl,
    .start = i2c_start,
    .decode = i2c_decode,
    .end = NULL,
    .metadata = i2c_metadata,
    .destroy = i2c_destroy_impl,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* Set option defaults */
    i2c_options[0].def = g_variant_new_string("shifted");
    i2c_options[1].def = g_variant_new_string("hex");
    i2c_options[2].def = g_variant_new_string("yes");

    /* Set option value lists */
    GSList *addr_fmt_vals = NULL;
    addr_fmt_vals = g_slist_append(addr_fmt_vals, g_variant_new_string("shifted"));
    addr_fmt_vals = g_slist_append(addr_fmt_vals, g_variant_new_string("unshifted"));
    i2c_options[0].values = addr_fmt_vals;

    GSList *pkt_fmt_vals = NULL;
    pkt_fmt_vals = g_slist_append(pkt_fmt_vals, g_variant_new_string("none"));
    pkt_fmt_vals = g_slist_append(pkt_fmt_vals, g_variant_new_string("hex"));
    pkt_fmt_vals = g_slist_append(pkt_fmt_vals, g_variant_new_string("ascii"));
    pkt_fmt_vals = g_slist_append(pkt_fmt_vals, g_variant_new_string("dec"));
    pkt_fmt_vals = g_slist_append(pkt_fmt_vals, g_variant_new_string("bin"));
    pkt_fmt_vals = g_slist_append(pkt_fmt_vals, g_variant_new_string("oct"));
    i2c_options[1].values = pkt_fmt_vals;

    GSList *sdp_vals = NULL;
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("yes"));
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("no"));
    i2c_options[2].values = sdp_vals;

    return &i2c_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}