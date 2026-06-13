/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2011 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
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

/* Channel indices — match Python: CLK=0, MISO=1, MOSI=2, CS=3 */
#define CH_CLK  0
#define CH_MISO 1
#define CH_MOSI 2
#define CH_CS   3

#define MAX_TRANSFER_BYTES 256

/* Annotation class indices — match Python annotations tuple */
enum spi_ann {
    ANN_MISO_DATA = 0,
    ANN_MOSI_DATA,
    ANN_MISO_BIT,
    ANN_MOSI_BIT,
    ANN_WARNING,
    ANN_MISO_TRANSFER,
    ANN_MOSI_TRANSFER,
    ANN_ATK_DATA_POINT,
    ANN_ATK_RISING_EDGE,
    ANN_ATK_FALLING_EDGE,
    NUM_ANN,
};

/* Decoder state struct — C_DECODER_STATE auto-generates spi_s typedef,
 * spi_reset (calloc), and spi_destroy (free). We supplement non-zero
 * defaults in spi_start(). */
C_DECODER_STATE(spi, {
    int bitcount;
    uint64_t mosidata;
    uint64_t misodata;

    uint64_t miso_bits_ss[64];
    uint64_t miso_bits_es[64];
    int miso_bits_val[64];
    uint64_t mosi_bits_ss[64];
    uint64_t mosi_bits_es[64];
    int mosi_bits_val[64];

    uint64_t ss_block;
    uint64_t ss_transfer;
    int cs_was_deasserted;

    uint64_t misobytes_val[MAX_TRANSFER_BYTES];
    int misobytes_cnt;
    uint64_t mosibytes_val[MAX_TRANSFER_BYTES];
    int mosibytes_cnt;

    /* Cached options */
    int cpol;
    int cpha;
    int bit_order;       /* 0=msb-first, 1=lsb-first */
    int cs_polarity;     /* 0=active-low, 1=active-high */
    int have_cs;
    int have_miso;
    int have_mosi;
    int sample_edge_rise; /* 1=rising, 0=falling */
    int wordsize;
    int bw;              /* (wordsize + 7) / 8 */
    int format;          /* 0=hex, 1=dec, 2=oct, 3=bin, 4=ascii */
    int show_data_point;
    uint64_t samplerate;

    /* Output IDs */
    int out_ann;
    int out_python;
    int out_binary;
    int out_bitrate;
});

/* Channel definitions — match Python channels tuple */
static struct srd_channel spi_channels[] = {
    {"clk", "CLK", "Clock(串行时钟)", 0, SRD_CHANNEL_SCLK, NULL},
};

static struct srd_channel spi_optional_channels[] = {
    {"miso", "MISO", "Master in, slave out(主入从出)", 1, SRD_CHANNEL_SDATA, NULL},
    {"mosi", "MOSI", "Master out, slave in(主出从入)", 2, SRD_CHANNEL_SDATA, NULL},
    {"cs", "CS#", "Chip-select(片选信号)", 3, SRD_CHANNEL_COMMON, NULL},
};

/* Options — match Python options tuple */
static struct srd_decoder_option spi_options[] = {
    {"cs_polarity",     NULL, "CS# polarity(片选极性)",     NULL, NULL},
    {"cpol",            NULL, "Clock polarity(时钟极性)",   NULL, NULL},
    {"cpha",            NULL, "Clock phase(时钟相位)",      NULL, NULL},
    {"bitorder",        NULL, "Bit order(位序)",            NULL, NULL},
    {"wordsize",        NULL, "Word size(字长)",            NULL, NULL},
    {"format",          NULL, "Data format(数据格式)",      NULL, NULL},
    {"show_data_point", NULL, "Show data point(数据点显示)", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *spi_ann_labels[][3] = {
    {"", "MISO", "MISO data"},
    {"", "MOSI", "MOSI data"},
    {"", "MISO bit", "MISO bit"},
    {"", "MOSI bit", "MOSI bit"},
    {"", "Warning", "Warning"},
    {"", "MISO transfer", "MISO transfer"},
    {"", "MOSI transfer", "MOSI transfer"},
    {"", "ATK Data point", "ATK Data point"},
    {"", "ATK Rising edge", "ATK Rising edge"},
    {"", "ATK Falling edge", "ATK Falling edge"},
};

/* Annotation row class lists */
static const int spi_row_miso_bits_classes[] = {ANN_MISO_BIT, -1};
static const int spi_row_miso_data_classes[] = {ANN_MISO_DATA, -1};
static const int spi_row_miso_transfer_classes[] = {ANN_MISO_TRANSFER, -1};
static const int spi_row_mosi_bits_classes[] = {ANN_MOSI_BIT, -1};
static const int spi_row_mosi_data_classes[] = {ANN_MOSI_DATA, -1};
static const int spi_row_mosi_transfer_classes[] = {ANN_MOSI_TRANSFER, -1};
static const int spi_row_other_classes[] = {ANN_WARNING, -1};
static const int spi_row_atk_classes[] = {ANN_ATK_DATA_POINT, ANN_ATK_RISING_EDGE, ANN_ATK_FALLING_EDGE, -1};

static const struct srd_c_ann_row spi_ann_rows[] = {
    {"miso-bits",       "MISO bits",      spi_row_miso_bits_classes, 1},
    {"miso-data-vals",  "MISO data",      spi_row_miso_data_classes, 1},
    {"miso-transfers",  "MISO transfers", spi_row_miso_transfer_classes, 1},
    {"mosi-bits",       "MOSI bits",      spi_row_mosi_bits_classes, 1},
    {"mosi-data-vals",  "MOSI data",      spi_row_mosi_data_classes, 1},
    {"mosi-transfers",  "MOSI transfers", spi_row_mosi_transfer_classes, 1},
    {"other",           "Other",          spi_row_other_classes, 1},
    {"atk-signs",       "ATK signs",      spi_row_atk_classes, 3},
};

static const struct srd_decoder_binary spi_binary[] = {
    {0, "miso", "MISO"},
    {1, "mosi", "MOSI"},
};

static const char *spi_inputs[] = {"logic", NULL};
static const char *spi_outputs[] = {"spi", NULL};
static const char *spi_tags[] = {"Embedded/industrial", NULL};

/* ---- Helper: format a single data value for annotation ---- */
static void spi_format_value(uint64_t val, int wordsize, int format,
                             char *out, int out_size)
{
    if (format == 0) { /* hex */
        snprintf(out, out_size, "%02llx", (unsigned long long)val);
    } else if (format == 1) { /* dec */
        snprintf(out, out_size, "%llu", (unsigned long long)val);
    } else if (format == 2) { /* oct */
        snprintf(out, out_size, "%03llo", (unsigned long long)val);
    } else if (format == 3) { /* bin */
        int width = wordsize > 8 ? wordsize : 8;
        char tmp[65];
        for (int i = 0; i < width; i++) {
            int bit_idx = width - 1 - i;
            tmp[i] = ((val >> bit_idx) & 1) ? '1' : '0';
        }
        tmp[width] = '\0';
        snprintf(out, out_size, "%s", tmp);
    } else { /* ascii */
        if (val >= 32 && val <= 126) {
            snprintf(out, out_size, "%c", (char)val);
        } else {
            snprintf(out, out_size, "%02llX", (unsigned long long)val);
        }
    }
}

/* ---- Helper: format transfer bytes for annotation ---- */
static void spi_format_transfer(uint64_t *vals, int cnt, int format,
                                int wordsize, char *out, int out_size)
{
    int pos = 0;
    out[0] = '\0';
    for (int i = 0; i < cnt && pos < out_size - 16; i++) {
        if (i > 0 && format != 4)
            pos += snprintf(out + pos, out_size - pos, " ");
        if (format == 0) {
            pos += snprintf(out + pos, out_size - pos, "%02llX",
                            (unsigned long long)vals[i]);
        } else if (format == 1) {
            pos += snprintf(out + pos, out_size - pos, "%llu",
                            (unsigned long long)vals[i]);
        } else if (format == 2) {
            pos += snprintf(out + pos, out_size - pos, "%03llo",
                            (unsigned long long)vals[i]);
        } else if (format == 3) {
            char btmp[65];
            int bwidth = wordsize > 8 ? wordsize : 8;
            for (int b = 0; b < bwidth; b++) {
                int bit_idx = bwidth - 1 - b;
                btmp[b] = ((vals[i] >> bit_idx) & 1) ? '1' : '0';
            }
            btmp[bwidth] = '\0';
            pos += snprintf(out + pos, out_size - pos, "%s", btmp);
        } else { /* ascii */
            if (vals[i] >= 32 && vals[i] <= 126) {
                pos += snprintf(out + pos, out_size - pos, "%c",
                                (char)vals[i]);
            } else {
                pos += snprintf(out + pos, out_size - pos, "\\x%02llX",
                                (unsigned long long)vals[i]);
            }
        }
    }
}

/* ---- Helper: check if CS is asserted ---- */
static int spi_cs_asserted(spi_s *s, int cs_val)
{
    return (s->cs_polarity == 0) ? (cs_val == 0) : (cs_val == 1);
}

/* ---- Helper: reset per-word decoder state (Python: reset_decoder_state) ---- */
static void spi_reset_word(spi_s *s)
{
    s->bitcount = 0;
    s->mosidata = 0;
    s->misodata = 0;
    /* Note: miso_bits/mosi_bits arrays don't need clearing;
     * they are overwritten before being read. */
}

/* ---- Helper: output data for a complete word (Python: putdata) ---- */
static void spi_put_data(struct srd_decoder_inst *di, spi_s *s)
{
    uint64_t ss = s->ss_block;
    uint64_t es = 0;

    /* Binary output — match Python:
     *   if self.have_miso:
     *       ss, es = self.misobits[-1][1], self.misobits[0][2]
     *       bdata = so.to_bytes(self.bw, byteorder='big')
     *       self.put(ss, es, self.out_binary, [0, bdata])
     * misobits[-1] = first bit (index 0 in C), misobits[0] = last bit */
    if (s->have_miso) {
        ss = s->miso_bits_ss[0];
        es = s->miso_bits_es[s->wordsize - 1];
        unsigned char bdata[8];
        for (int i = 0; i < s->bw; i++)
            bdata[i] = (unsigned char)(s->misodata >> (8 * (s->bw - 1 - i)));
        c_put_bin(di, ss, es, s->out_binary, 0, s->bw, bdata);
    }
    if (s->have_mosi) {
        ss = s->mosi_bits_ss[0];
        es = s->mosi_bits_es[s->wordsize - 1];
        unsigned char bdata[8];
        for (int i = 0; i < s->bw; i++)
            bdata[i] = (unsigned char)(s->mosidata >> (8 * (s->bw - 1 - i)));
        c_put_bin(di, ss, es, s->out_binary, 1, s->bw, bdata);
    }

    /* BITS protocol output — per-bit timestamps */
    {
        int mosi_cnt = s->have_mosi ? s->wordsize : 0;
        int miso_cnt = s->have_miso ? s->wordsize : 0;
        unsigned char bits_data[2200];
        int bpos = 0;

        bits_data[bpos++] = (unsigned char)((s->have_mosi ? 1 : 0) |
                                             (s->have_miso ? 2 : 0));
        bits_data[bpos++] = (unsigned char)mosi_cnt;

        for (int i = 0; i < mosi_cnt && bpos + 17 <= (int)sizeof(bits_data); i++) {
            bits_data[bpos++] = (unsigned char)s->mosi_bits_val[i];
            uint64_t ss_val = s->mosi_bits_ss[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
            uint64_t es_val = s->mosi_bits_es[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
        }

        bits_data[bpos++] = 0x00; /* reserved */
        bits_data[bpos++] = (unsigned char)miso_cnt;

        for (int i = 0; i < miso_cnt && bpos + 17 <= (int)sizeof(bits_data); i++) {
            bits_data[bpos++] = (unsigned char)s->miso_bits_val[i];
            uint64_t ss_val = s->miso_bits_ss[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
            uint64_t es_val = s->miso_bits_es[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
        }

        c_proto(di, ss, es, s->out_python, "BITS",
                C_BYTES(bits_data, bpos), C_END);
    }

    /* DATA protocol output — match Python: ['DATA', si, so] */
    {
        unsigned char data_data[17];
        int dpos = 0;
        data_data[dpos++] = (unsigned char)((s->have_mosi ? 1 : 0) |
                                             (s->have_miso ? 2 : 0));
        uint64_t mv = s->have_mosi ? s->mosidata : 0;
        uint64_t sv = s->have_miso ? s->misodata : 0;
        for (int i = 0; i < 8; i++)
            data_data[dpos++] = (unsigned char)(mv >> (8 * i));
        for (int i = 0; i < 8; i++)
            data_data[dpos++] = (unsigned char)(sv >> (8 * i));
        c_proto(di, ss, es, s->out_python, "DATA",
                C_BYTES(data_data, dpos), C_END);
    }

    /* Append to transfer byte lists — match Python:
     *   self.misobytes.append(Data(ss=ss, es=es, val=so))
     *   self.mosibytes.append(Data(ss=ss, es=es, val=si)) */
    if (s->have_miso && s->misobytes_cnt < MAX_TRANSFER_BYTES) {
        s->misobytes_val[s->misobytes_cnt] = s->misodata;
        s->misobytes_cnt++;
    }
    if (s->have_mosi && s->mosibytes_cnt < MAX_TRANSFER_BYTES) {
        s->mosibytes_val[s->mosibytes_cnt] = s->mosidata;
        s->mosibytes_cnt++;
    }

    /* Bit annotations — match Python:
     *   for bit in self.misobits:
     *       self.put(bit[1], bit[2], self.out_ann, [2, ['%d' % bit[0]]]) */
    if (s->have_miso) {
        for (int i = 0; i < s->wordsize; i++) {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", s->miso_bits_val[i]);
            c_put(di, s->miso_bits_ss[i], s->miso_bits_es[i],
                  s->out_ann, ANN_MISO_BIT, bit_str);
        }
    }
    if (s->have_mosi) {
        for (int i = 0; i < s->wordsize; i++) {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", s->mosi_bits_val[i]);
            c_put(di, s->mosi_bits_ss[i], s->mosi_bits_es[i],
                  s->out_ann, ANN_MOSI_BIT, bit_str);
        }
    }

    /* Data word annotations — match Python:
     *   self.put(ss, es, self.out_ann, [0, [self.miso_data]]) */
    if (s->have_miso) {
        char miso_str[128];
        spi_format_value(s->misodata, s->wordsize, s->format,
                         miso_str, sizeof(miso_str));
        c_put(di, ss, es, s->out_ann, ANN_MISO_DATA, miso_str);
    }
    if (s->have_mosi) {
        char mosi_str[128];
        spi_format_value(s->mosidata, s->wordsize, s->format,
                         mosi_str, sizeof(mosi_str));
        c_put(di, ss, es, s->out_ann, ANN_MOSI_DATA, mosi_str);
    }
}

/* ---- Helper: output transfer annotations when CS deasserts ---- */
static void spi_put_transfer(struct srd_decoder_inst *di, spi_s *s,
                             uint64_t samplenum)
{
    (void)samplenum;
    /* Match Python:
     *   if self.have_miso:
     *       formatted_miso = self.format_data(self.misobytes, fmt)
     *       self.put(self.ss_transfer, self.samplenum, self.out_ann, [5, [formatted_miso]])
     */
    if (s->have_miso) {
        char transfer_str[4096];
        spi_format_transfer(s->misobytes_val, s->misobytes_cnt,
                            s->format, s->wordsize,
                            transfer_str, sizeof(transfer_str));
        c_put(di, s->ss_transfer, samplenum, s->out_ann,
              ANN_MISO_TRANSFER, transfer_str);
    }
    if (s->have_mosi) {
        char transfer_str[4096];
        spi_format_transfer(s->mosibytes_val, s->mosibytes_cnt,
                            s->format, s->wordsize,
                            transfer_str, sizeof(transfer_str));
        c_put(di, s->ss_transfer, samplenum, s->out_ann,
              ANN_MOSI_TRANSFER, transfer_str);
    }

    /* TRANSFER protocol output — match Python:
     *   self.put(self.ss_transfer, self.samplenum, self.out_python,
     *       ['TRANSFER', self.mosibytes, self.misobytes]) */
    {
        int mosi_cnt = s->have_mosi ? s->mosibytes_cnt : 0;
        int miso_cnt = s->have_miso ? s->misobytes_cnt : 0;
        int n_fields = 5 + mosi_cnt + miso_cnt;
        unsigned char *xfer_data = (unsigned char *)g_malloc(n_fields);
        int xpos = 0;
        xfer_data[xpos++] = (unsigned char)((s->have_mosi ? 1 : 0) |
                                             (s->have_miso ? 2 : 0));
        xfer_data[xpos++] = (unsigned char)(mosi_cnt & 0xFF);
        xfer_data[xpos++] = (unsigned char)((mosi_cnt >> 8) & 0xFF);
        xfer_data[xpos++] = (unsigned char)(miso_cnt & 0xFF);
        xfer_data[xpos++] = (unsigned char)((miso_cnt >> 8) & 0xFF);
        for (int i = 0; i < mosi_cnt; i++)
            xfer_data[xpos++] = (unsigned char)s->mosibytes_val[i];
        for (int i = 0; i < miso_cnt; i++)
            xfer_data[xpos++] = (unsigned char)s->misobytes_val[i];
        c_proto(di, s->ss_transfer, samplenum, s->out_python, "TRANSFER",
                C_BYTES(xfer_data, xpos), C_END);
        g_free(xfer_data);
    }
}

/* ---- start callback — match Python: start() ---- */
static void spi_start(struct srd_decoder_inst *di)
{
    spi_s *s = (spi_s *)c_decoder_get_private(di);

    s->out_ann     = c_reg_out(di, SRD_OUTPUT_ANN, "spi");
    s->out_python  = c_reg_out(di, SRD_OUTPUT_PROTO, "spi");
    s->out_binary  = c_reg_out(di, SRD_OUTPUT_BINARY, "spi");
    s->out_bitrate = c_reg_out(di, SRD_OUTPUT_META, "spi");

    /* Options — match Python options */
    const char *cs_pol_str = c_opt_str(di, "cs_polarity", "active-low");
    s->cs_polarity = (strcmp(cs_pol_str, "active-low") == 0) ? 0 : 1;

    s->cpol = (int)c_opt_int(di, "cpol", 0);
    s->cpha = (int)c_opt_int(di, "cpha", 0);

    const char *bitorder_str = c_opt_str(di, "bitorder", "msb-first");
    s->bit_order = (strcmp(bitorder_str, "msb-first") == 0) ? 0 : 1;

    s->wordsize = (int)c_opt_int(di, "wordsize", 8);
    if (s->wordsize < 1 || s->wordsize > 64)
        s->wordsize = 8;
    s->bw = (s->wordsize + 7) / 8;

    const char *format_str = c_opt_str(di, "format", "hex");
    if (strcmp(format_str, "hex") == 0)
        s->format = 0;
    else if (strcmp(format_str, "dec") == 0)
        s->format = 1;
    else if (strcmp(format_str, "oct") == 0)
        s->format = 2;
    else if (strcmp(format_str, "bin") == 0)
        s->format = 3;
    else if (strcmp(format_str, "ascii") == 0)
        s->format = 4;
    else
        s->format = 0;

    s->show_data_point = c_opt_bool(di, "show_data_point", 1);

    /* Channel presence — match Python:
     *   self.have_miso = self.has_channel(1)
     *   self.have_mosi = self.has_channel(2)
     *   self.have_cs = self.has_channel(3) */
    s->have_miso = c_has_ch(di, CH_MISO);
    s->have_mosi = c_has_ch(di, CH_MOSI);
    s->have_cs   = c_has_ch(di, CH_CS);

    /* SPI mode → sample edge direction — match Python:
     *   mode = spi_mode[self.options['cpol'], self.options['cpha']] */
    int mode;
    if (s->cpol == 0 && s->cpha == 0)
        mode = 0;
    else if (s->cpol == 0 && s->cpha == 1)
        mode = 1;
    else if (s->cpol == 1 && s->cpha == 0)
        mode = 2;
    else
        mode = 3;
    s->sample_edge_rise = (mode == 0 || mode == 3) ? 1 : 0;

    /* Non-zero defaults that calloc didn't set */
    s->ss_transfer = (uint64_t)-1;
    s->samplerate = c_samplerate(di);
}

/* ---- metadata callback — match Python: metadata() ---- */
static void spi_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    spi_s *s = (spi_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

/* ---- decode callback — main state machine ---- */
static void spi_decode(struct srd_decoder_inst *di)
{
    spi_s *s = (spi_s *)c_decoder_get_private(di);
    int ret;

    /* Emit ATK color styling annotations at start — match Python:
     *   self.put(self.samplenum, self.samplenum, self.out_ann, [7,["color:#F32FDC"]])
     *   self.put(self.samplenum, self.samplenum, self.out_ann, [8,["color:#F32FDC"]])
     *   self.put(self.samplenum, self.samplenum, self.out_ann, [9,["color:#F32FDC"]]) */
    {
        uint64_t init_ss = 0;
        c_put(di, init_ss, init_ss, s->out_ann,
              ANN_ATK_DATA_POINT, "color:#F32FDC");
        c_put(di, init_ss, init_ss, s->out_ann,
              ANN_ATK_RISING_EDGE, "color:#F32FDC");
        c_put(di, init_ss, init_ss, s->out_ann,
              ANN_ATK_FALLING_EDGE, "color:#F32FDC");
    }

    /* Initial wait to get first sample — match Python:
     *   (clk, miso, mosi, cs) = self.wait({})
     *   self.find_clk_edge(miso, mosi, clk, cs, True)
     *
     * In find_clk_edge(first=True):
     *   if self.have_cs: emit CS-CHANGE(None, cs), set transfer start
     *   return early (skip CLK edge check)
     */
    if (s->have_cs) {
        /* Match Python: self.wait({}) returns immediately at sample 0,
         * allowing us to read the initial CS state before any edge. */
        ret = c_wait(di, CW_END);
    } else {
        /* No CS channel — emit CS-CHANGE(None, None) and skip to main loop */
        c_proto(di, 0, 0, s->out_python, "CS-CHANGE",
                C_I8(-1), C_I8(-1), C_END);
        ret = c_wait(di, CW_E(CH_CLK), CW_END);
    }
    if (ret != SRD_OK)
        return;

    /* Handle initial CS state — match Python find_clk_edge(first=True):
     *   if self.have_cs:
     *       oldcs = None
     *       self.put(self.samplenum, self.samplenum, self.out_python,
     *                ['CS-CHANGE', oldcs, cs])
     *       if self.cs_asserted(cs):
     *           self.ss_transfer = self.samplenum
     *           self.misobytes = []
     *           self.mosibytes = []
     *       self.reset_decoder_state() */
    if (s->have_cs) {
        uint64_t sn = di_samplenum(di);
        int cs = c_pin(di, CH_CS);
        c_proto(di, sn, sn, s->out_python, "CS-CHANGE",
                C_I8(-1), C_I8(cs), C_END);
        if (spi_cs_asserted(s, cs)) {
            s->ss_transfer = sn;
            s->misobytes_cnt = 0;
            s->mosibytes_cnt = 0;
        }
        spi_reset_word(s);
    }

    /* Main decode loop — match Python:
     *   while True:
     *       (clk, miso, mosi, cs) = self.wait(wait_cond)
     *       self.find_clk_edge(miso, mosi, clk, cs, False)
     *
     * wait_cond = [{0: 'e'}]  or  [{0: 'e'}, {3: 'e'}]
     * In v4: c_wait(di, CW_E(CH_CLK), CW_END)  or  c_wait(di, CW_E(CH_CLK), CW_OR, CW_E(CH_CS), CW_END)
     * Condition group 0 = CLK edge → matched bit 0
     * Condition group 1 = CS edge  → matched bit 1
     */
    while (1) {
        if (s->have_cs)
            ret = c_wait(di, CW_E(CH_CLK), CW_OR, CW_E(CH_CS), CW_END);
        else
            ret = c_wait(di, CW_E(CH_CLK), CW_END);

        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);
        uint64_t matched = di_matched(di);

        int clk  = c_pin(di, CH_CLK);
        int miso = s->have_miso ? c_pin(di, CH_MISO) : 0;
        int mosi = s->have_mosi ? c_pin(di, CH_MOSI) : 0;
        int cs   = s->have_cs   ? c_pin(di, CH_CS)   : 1;

        int clk_matched = (matched & (1ULL << 0)) != 0;
        int cs_matched  = s->have_cs && (matched & (1ULL << 1)) != 0;

        /* ---- CS change handling — match Python find_clk_edge:
         *   if self.have_cs and (self.matched & (0b1 << self.have_cs)):
         *       oldcs = 1 - cs
         *       self.put(..., ['CS-CHANGE', oldcs, cs])
         *       if self.cs_asserted(cs): start transfer
         *       elif self.ss_transfer != -1: end transfer
         *       self.reset_decoder_state() */
        if (cs_matched) {
            int oldcs = 1 - cs;
            c_proto(di, samplenum, samplenum, s->out_python, "CS-CHANGE",
                    C_I8(oldcs), C_I8(cs), C_END);

            if (spi_cs_asserted(s, cs)) {
                s->ss_transfer = samplenum;
                s->misobytes_cnt = 0;
                s->mosibytes_cnt = 0;
            } else if (s->ss_transfer != (uint64_t)-1) {
                spi_put_transfer(di, s, samplenum);
            }

            spi_reset_word(s);
        }

        /* Skip if CS is not asserted — match Python:
         *   if self.have_cs and not self.cs_asserted(cs):
         *       return */
        if (s->have_cs && !spi_cs_asserted(s, cs))
            continue;

        /* Skip if CLK didn't change — match Python:
         *   if first or (not self.matched & (0b1 << 0)):
         *       return */
        if (!clk_matched)
            continue;

        /* ---- Check for correct sampling edge — match Python:
         *   mode = spi_mode[cpol, cpha]
         *   if (mode == 0 and clk == 1) or (mode == 3 and clk == 1):
         *       # rising edge sampling
         *   elif (mode == 1 and clk == 0) or (mode == 2 and clk == 0):
         *       # falling edge sampling
         *   else:
         *       return */
        int correct_edge = 0; /* 0=none, 1=rising, 2=falling */
        if ((s->cpol == 0 && s->cpha == 0 && clk == 1) ||
            (s->cpol == 1 && s->cpha == 1 && clk == 1)) {
            correct_edge = 1; /* rising edge */
        } else if ((s->cpol == 0 && s->cpha == 1 && clk == 0) ||
                   (s->cpol == 1 && s->cpha == 0 && clk == 0)) {
            correct_edge = 2; /* falling edge */
        }

        if (!correct_edge)
            continue;

        /* ATK data point annotations — match Python:
         *   if self.show_data_point:
         *       if rising:  put([8, ['%d' % CLK]])  (ATK Rising edge)
         *       if falling: put([9, ['%d' % CLK]])  (ATK Falling edge)
         *       put([7, ['%d' % MISO]])  (ATK Data point)
         *       put([7, ['%d' % MOSI]])  (ATK Data point) */
        if (s->show_data_point) {
            char dp_str[8];
            if (correct_edge == 1) {
                snprintf(dp_str, sizeof(dp_str), "%d", CH_CLK);
                c_put(di, samplenum, samplenum, s->out_ann,
                      ANN_ATK_RISING_EDGE, dp_str);
            } else {
                snprintf(dp_str, sizeof(dp_str), "%d", CH_CLK);
                c_put(di, samplenum, samplenum, s->out_ann,
                      ANN_ATK_FALLING_EDGE, dp_str);
            }
            snprintf(dp_str, sizeof(dp_str), "%d", CH_MISO);
            c_put(di, samplenum, samplenum, s->out_ann,
                  ANN_ATK_DATA_POINT, dp_str);
            snprintf(dp_str, sizeof(dp_str), "%d", CH_MOSI);
            c_put(di, samplenum, samplenum, s->out_ann,
                  ANN_ATK_DATA_POINT, dp_str);
        }

        /* ---- handle_bit — match Python handle_bit() ---- */

        /* If this is the first bit of a dataword, save its sample number.
         * Python: if self.bitcount == 0: self.ss_block = self.samplenum */
        if (s->bitcount == 0) {
            s->ss_block = samplenum;
            s->cs_was_deasserted = s->have_cs ? !spi_cs_asserted(s, cs) : 0;
        }

        /* Update previous bit's end sample — match Python:
         *   if self.bitcount > 0 and self.have_miso:
         *       self.misobits[1][2] = self.samplenum */
        if (s->bitcount > 0) {
            if (s->have_miso && s->bitcount <= s->wordsize)
                s->miso_bits_es[s->bitcount - 1] = samplenum;
            if (s->have_mosi && s->bitcount <= s->wordsize)
                s->mosi_bits_es[s->bitcount - 1] = samplenum;
        }

        /* Receive MISO bit into shift register — match Python:
         *   if bo == 'msb-first':
         *       self.misodata |= miso << (ws - 1 - self.bitcount)
         *   else:
         *       self.misodata |= miso << self.bitcount */
        if (s->have_miso) {
            if (s->bit_order == 0) /* msb-first */
                s->misodata |= (uint64_t)miso << (s->wordsize - 1 - s->bitcount);
            else /* lsb-first */
                s->misodata |= (uint64_t)miso << s->bitcount;

            if (s->bitcount < s->wordsize) {
                s->miso_bits_ss[s->bitcount] = samplenum;
                s->miso_bits_es[s->bitcount] = samplenum;
                s->miso_bits_val[s->bitcount] = miso;
            }
        }

        /* Receive MOSI bit into shift register */
        if (s->have_mosi) {
            if (s->bit_order == 0) /* msb-first */
                s->mosidata |= (uint64_t)mosi << (s->wordsize - 1 - s->bitcount);
            else /* lsb-first */
                s->mosidata |= (uint64_t)mosi << s->bitcount;

            if (s->bitcount < s->wordsize) {
                s->mosi_bits_ss[s->bitcount] = samplenum;
                s->mosi_bits_es[s->bitcount] = samplenum;
                s->mosi_bits_val[s->bitcount] = mosi;
            }
        }

        s->bitcount++;

        /* Continue to receive if not enough bits were received yet.
         * Python: if self.bitcount != ws: return */
        if (s->bitcount != s->wordsize)
            continue;

        /* Word complete — output data */
        spi_put_data(di, s);

        /* Meta bitrate — match Python:
         *   if self.samplerate:
         *       elapsed = 1 / float(self.samplerate)
         *       elapsed *= (self.samplenum - self.ss_block + 1)
         *       bitrate = int(1 / elapsed * ws) */
        if (s->samplerate > 0) {
            double elapsed = 1.0 / (double)s->samplerate;
            elapsed *= (double)(samplenum - s->ss_block + 1);
            int bitrate = (int)(1.0 / elapsed * s->wordsize);
            c_put_meta_int(di, s->ss_block, samplenum,
                           s->out_bitrate, bitrate);
        }

        /* CS deasserted warning — match Python:
         *   if self.have_cs and self.cs_was_deasserted:
         *       self.putw([4, ['CS# was deasserted during this data word!']]) */
        if (s->have_cs && s->cs_was_deasserted) {
            c_put(di, s->ss_block, samplenum, s->out_ann, ANN_WARNING,
                  "CS# was deasserted during this data word!");
        }

        /* Reset per-word state — match Python: self.reset_decoder_state() */
        spi_reset_word(s);
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder spi_c_def = {
    .id = "spi_c",
    .name = "SPI(C)",
    .longname = "Serial Peripheral Interface (C)",
    .desc = "SPI protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = spi_channels,
    .num_channels = 1,
    .optional_channels = spi_optional_channels,
    .num_optional_channels = 3,
    .options = spi_options,
    .num_options = 7,
    .num_annotations = NUM_ANN,
    .ann_labels = spi_ann_labels,
    .num_annotation_rows = 8,
    .annotation_rows = spi_ann_rows,
    .inputs = spi_inputs,
    .num_inputs = 1,
    .outputs = spi_outputs,
    .num_outputs = 1,
    .binary = spi_binary,
    .num_binary = 2,
    .tags = spi_tags,
    .num_tags = 1,
    .state_size = sizeof(spi_s),
    .reset = spi_reset,
    .start = spi_start,
    .decode = spi_decode,
    .end = NULL,
    .metadata = spi_metadata,
    .destroy = spi_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* Set option defaults — match Python options tuple */
    spi_options[0].def = g_variant_new_string("active-low");
    spi_options[1].def = g_variant_new_uint64(0);  /* cpol */
    spi_options[2].def = g_variant_new_uint64(0);  /* cpha */
    spi_options[3].def = g_variant_new_string("msb-first");
    spi_options[4].def = g_variant_new_uint64(8);  /* wordsize */
    spi_options[5].def = g_variant_new_string("hex");
    spi_options[6].def = g_variant_new_string("yes");

    /* Set option value lists */
    GSList *cs_pol_vals = NULL;
    cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-low"));
    cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-high"));
    spi_options[0].values = cs_pol_vals;

    GSList *cpol_vals = NULL;
    cpol_vals = g_slist_append(cpol_vals, g_variant_new_uint64(0));
    cpol_vals = g_slist_append(cpol_vals, g_variant_new_uint64(1));
    spi_options[1].values = cpol_vals;

    GSList *cpha_vals = NULL;
    cpha_vals = g_slist_append(cpha_vals, g_variant_new_uint64(0));
    cpha_vals = g_slist_append(cpha_vals, g_variant_new_uint64(1));
    spi_options[2].values = cpha_vals;

    GSList *bitorder_vals = NULL;
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb-first"));
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb-first"));
    spi_options[3].values = bitorder_vals;

    /* wordsize has no value list restriction */

    GSList *format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("ascii"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("dec"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("hex"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("oct"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("bin"));
    spi_options[5].values = format_vals;

    GSList *sdp_vals = NULL;
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("yes"));
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("no"));
    spi_options[6].values = sdp_vals;

    return &spi_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}