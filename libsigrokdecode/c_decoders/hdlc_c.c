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

/* Channel indices — match Python channels tuple */
#define CH_CLK  0
#define CH_DATA 1
#define CH_EN   2

enum {
    ANN_RX_BIT = 0,
    ANN_DATA,
    ANN_DATA_TYPE,
    ANN_WARNING,
    ANN_TRANSFER,
    NUM_ANN,
};

/* Decoder state struct — C_DECODER_STATE auto-generates hdlc_s typedef,
 * hdlc_reset (calloc), and hdlc_destroy (free). */
C_DECODER_STATE(hdlc, {
    int bitcount;
    uint8_t rxdata;
    uint64_t ss_bits[8];
    int one_count;
    gboolean flag_found;
    uint8_t rxbytes[1024];
    uint64_t rxbytes_ss[1024];
    uint64_t rxbytes_es[1024];
    int rxbytes_cnt;
    uint64_t ss_prev_clock;
    int prev_bit;
    uint64_t ss_flag_start;
    int have_en;
    int en_active_high;
    int cpol;
    int pending_flag;
    int pending_abort;
    uint64_t ss_pending_start;

    /* Output IDs */
    int out_ann;
    int out_python;
    int out_binary;
});

/* ---- Helper: CRC-16 computation — match Python crc16() ---- */
static uint16_t hdlc_crc16(uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0x8408;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFF;
}

/* Channel definitions — match Python channels tuple */
static struct srd_channel hdlc_channels[] = {
    {"clk", "CLK", "Clock", 0, SRD_CHANNEL_SCLK, NULL},
    {"data", "DATA", "Data in", 1, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_channel hdlc_optional_channels[] = {
    {"en", "ENABLE", "RX enabled", 2, SRD_CHANNEL_COMMON, NULL},
};

/* Options — match Python options tuple */
static struct srd_decoder_option hdlc_options[] = {
    {"en_polarity", NULL, "ENABLE polarity", NULL, NULL},
    {"cpol", NULL, "Clock polarity", NULL, NULL},
};

/* Annotation labels — match Python annotations tuple */
static const char *hdlc_ann_labels[][3] = {
    {"", "rx-bit", "RX bit"},
    {"", "data", "data"},
    {"", "data-type", "data-type"},
    {"", "warning", "Warning"},
    {"", "transfer", "transfer"},
};

static const int hdlc_row_rxbits_classes[] = {ANN_RX_BIT, -1};
static const int hdlc_row_datavals_classes[] = {ANN_DATA, -1};
static const int hdlc_row_datatypes_classes[] = {ANN_DATA_TYPE, -1};
static const int hdlc_row_transfers_classes[] = {ANN_TRANSFER, -1};
static const int hdlc_row_other_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row hdlc_ann_rows[] = {
    {"rx-bits", "RX bits", hdlc_row_rxbits_classes, 1},
    {"data-vals", "data", hdlc_row_datavals_classes, 1},
    {"data-types", "type", hdlc_row_datatypes_classes, 1},
    {"transfers", "transfers", hdlc_row_transfers_classes, 1},
    {"other", "Other", hdlc_row_other_classes, 1},
};

static const char *hdlc_inputs[] = {"logic", NULL};
static const char *hdlc_outputs[] = {"hdlc", NULL};
static const char *hdlc_tags[] = {"Embedded/industrial", NULL};

static const struct srd_decoder_binary hdlc_binary[] = {
    {0, "transfer", "transfer"},
};

/* ---- Helper: reset internal state — match Python reset_state() ---- */
static void hdlc_reset_state(hdlc_s *priv)
{
    priv->bitcount = 0;
    priv->rxdata = 0;
    priv->one_count = 0;
    priv->flag_found = FALSE;
    priv->ss_prev_clock = (uint64_t)-1;
    priv->prev_bit = 0;
    priv->ss_flag_start = 0;
    priv->rxbytes_cnt = 0;
    priv->pending_flag = 0;
    priv->pending_abort = 0;
    priv->ss_pending_start = 0;
}

/* ---- Helper: check if ENABLE is asserted — match Python en_asserted() ---- */
static int hdlc_en_asserted(hdlc_s *priv, int en)
{
    return priv->en_active_high ? (en == 1) : (en == 0);
}

/* ---- Helper: display transfer — match Python putt() ---- */
static void hdlc_putt(struct srd_decoder_inst *di, hdlc_s *priv)
{
    if (priv->rxbytes_cnt <= 4)
        return;

    int cnt = priv->rxbytes_cnt;

    /* TRANSFER and CRC annotations — match Python:
     *   self.put(self.rxbytes[0][0], self.rxbytes[-2][0], self.out_ann, [ann_data_type, ['TRANSFER']])
     *   self.put(self.rxbytes[-2][0], self.rxbytes[-1][1], self.out_ann, [ann_data_type, ['CRC']]) */
    c_put(di, priv->rxbytes_ss[0], priv->rxbytes_ss[cnt - 2],
          priv->out_ann, ANN_DATA_TYPE, "TRANSFER");

    c_put(di, priv->rxbytes_ss[cnt - 2], priv->rxbytes_es[cnt - 1],
          priv->out_ann, ANN_DATA_TYPE, "CRC");

    /* CRC check — match Python:
     *   crc = self.crc16(self.rxbytes)
     *   rxCrc = ((self.rxbytes[-1][2] & 0xFF) << 8) | (self.rxbytes[-2][2] & 0xFF) */
    uint16_t crc = hdlc_crc16(priv->rxbytes, cnt - 2);
    uint16_t rxcrc = ((uint16_t)priv->rxbytes[cnt - 1] << 8) | (uint16_t)priv->rxbytes[cnt - 2];

    if (crc != rxcrc) {
        c_put(di, priv->rxbytes_ss[0], priv->rxbytes_es[cnt - 1],
              priv->out_ann, ANN_WARNING, "BAD CRC!");
    } else {
        /* Send to python — match Python:
         *   self.put(self.rxbytes[0][0], self.rxbytes[-2][0], self.out_python,
         *       ['TRANSFER', transData]) */
        int data_cnt = cnt - 2;
        int buf_size = data_cnt * 17;
        unsigned char *py_buf = (unsigned char *)g_malloc(buf_size);
        for (int i = 0; i < data_cnt; i++) {
            uint64_t ss = priv->rxbytes_ss[i];
            uint64_t es = priv->rxbytes_es[i];
            uint8_t val = priv->rxbytes[i];
            memcpy(py_buf + i * 17, &ss, 8);
            memcpy(py_buf + i * 17 + 8, &es, 8);
            py_buf[i * 17 + 16] = val;
        }
        c_proto(di, priv->rxbytes_ss[0], priv->rxbytes_es[cnt - 2],
                priv->out_python, "TRANSFER", C_BYTES(py_buf, buf_size), C_END);
        g_free(py_buf);

        /* Send to binary — match Python:
         *   for x in self.rxbytes[0:-2]:
         *       self.put(x[0], x[0], self.out_binary, [0, x[2].to_bytes(1, byteorder='big')]) */
        for (int i = 0; i < cnt - 2; i++) {
            c_put_bin(di, priv->rxbytes_ss[i], priv->rxbytes_ss[i],
                      priv->out_binary, 0, 1, &priv->rxbytes[i]);
        }
    }

    /* Transfer annotation — match Python:
     *   self.put(self.rxbytes[0][0], self.rxbytes[-1][1], self.out_ann,
     *       [ann_transfer, [' '.join(format(x.val, '02X') for x in transData)]]) */
    char hex_str[3072];
    int pos = 0;
    for (int i = 0; i < cnt - 2 && pos < (int)sizeof(hex_str) - 4; i++) {
        if (i > 0)
            pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, " ");
        pos += snprintf(hex_str + pos, sizeof(hex_str) - pos, "%02X", priv->rxbytes[i]);
    }
    c_put(di, priv->rxbytes_ss[0], priv->rxbytes_es[cnt - 1],
          priv->out_ann, ANN_TRANSFER, hex_str);
}

/* ---- Helper: shift bit — match Python shift_bit() ---- */
static void hdlc_shift_bit(hdlc_s *priv, int data, uint64_t samplenum)
{
    (void)samplenum;
    if (priv->flag_found) {
        if (priv->bitcount < 8)
            priv->ss_bits[priv->bitcount] = samplenum;
        priv->rxdata |= data << priv->bitcount;
        priv->bitcount++;
    }
}

/* ---- Helper: handle bit — match Python handle_bit() ---- */
static void hdlc_handle_bit(struct srd_decoder_inst *di, hdlc_s *priv,
                            int data, uint64_t samplenum)
{
    (void)samplenum;
    /* Display previous bit — match Python:
     *   if(self.ss_prev_clock != -1):
     *       self.put(self.ss_prev_clock, self.samplenum, self.out_ann, [ann_bit, ['%d' % self.prev_bit]]) */
    if (priv->ss_prev_clock != (uint64_t)-1) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", priv->prev_bit);
        c_put(di, priv->ss_prev_clock, samplenum, priv->out_ann, ANN_RX_BIT, bit_str);
    }
    priv->ss_prev_clock = samplenum;
    priv->prev_bit = data;

    /* Byte complete — match Python:
     *   if self.bitcount == 8:
     *       self.put(self.rxbits[0][1], self.samplenum, self.out_ann, [ann_data, ['%02X' % self.rxdata]])
     *       self.rxbytes.append([self.rxbits[0][1], self.samplenum, self.rxdata]) */
    if (priv->bitcount == 8) {
        char data_str[8];
        snprintf(data_str, sizeof(data_str), "%02X", priv->rxdata);
        c_put(di, priv->ss_bits[0], samplenum, priv->out_ann, ANN_DATA, data_str);
        if (priv->rxbytes_cnt < 1024) {
            priv->rxbytes[priv->rxbytes_cnt] = priv->rxdata;
            priv->rxbytes_ss[priv->rxbytes_cnt] = priv->ss_bits[0];
            priv->rxbytes_es[priv->rxbytes_cnt] = samplenum;
            priv->rxbytes_cnt++;
        }
        priv->rxdata = 0;
        priv->bitcount = 0;
    }

    /* Display abort packet — match Python:
     *   if self.abort is not None: */
    if (priv->pending_abort) {
        c_put(di, priv->ss_pending_start, samplenum,
              priv->out_ann, ANN_DATA_TYPE, "ABORT");
        priv->pending_abort = 0;
    }

    /* Display flag — match Python:
     *   if self.flag is not None: */
    if (priv->pending_flag) {
        c_put(di, priv->ss_pending_start, samplenum,
              priv->out_ann, ANN_DATA_TYPE, "FLAG");
        priv->pending_flag = 0;
        hdlc_putt(di, priv);
        priv->rxbytes_cnt = 0;
    }

    /* Count number of 1 — match Python:
     *   if data == 1: ... else: ... */
    if (data == 1) {
        if (priv->one_count < 5)
            hdlc_shift_bit(priv, data, samplenum);
        priv->one_count++;
    } else {
        if (priv->one_count == 6) {
            /* Found flag */
            priv->flag_found = TRUE;
            priv->pending_flag = 1;
            priv->ss_pending_start = priv->ss_flag_start;
            priv->rxdata = 0;
            priv->bitcount = 0;
        } else if (priv->one_count > 6) {
            /* Abort */
            priv->pending_abort = 1;
            priv->ss_pending_start = priv->ss_flag_start;
            priv->flag_found = FALSE;
            priv->rxdata = 0;
            priv->bitcount = 0;
            priv->rxbytes_cnt = 0;
        } else if (priv->one_count < 5) {
            hdlc_shift_bit(priv, data, samplenum);
        }

        priv->one_count = 0;
        priv->ss_flag_start = samplenum;
    }
}

/* ---- start callback — match Python start() ---- */
static void hdlc_start(struct srd_decoder_inst *di)
{
    hdlc_s *priv = (hdlc_s *)c_decoder_get_private(di);

    priv->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "hdlc");
    priv->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "hdlc");
    priv->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "hdlc");

    const char *en_pol = c_opt_str(di, "en_polarity", "active-high");
    priv->en_active_high = (en_pol && strcmp(en_pol, "active-low") == 0) ? 0 : 1;
    priv->cpol = (int)c_opt_int(di, "cpol", 1);
    priv->have_en = c_has_ch(di, CH_EN);

    /* Match Python:
     *   if not self.have_en:
     *       self.put(0, 0, self.out_python, ['CS-CHANGE', None, None]) */
    if (!priv->have_en) {
        c_proto(di, 0, 0, priv->out_python, "CS-CHANGE",
                C_I8(-1), C_I8(-1), C_END);
    }
}

/* ---- decode callback — match Python decode() ---- */
static void hdlc_decode(struct srd_decoder_inst *di)
{
    hdlc_s *priv = (hdlc_s *)c_decoder_get_private(di);
    int ret;

    /* Grab first sample — match Python:
     *   (clk, data, en) = self.wait({})
     *   self.find_clk_edge(clk, data, en, True) */
    ret = c_wait(di, CW_SKIP(0), CW_END);
    if (ret != SRD_OK)
        return;

    /* Main decode loop — match Python:
     *   while True:
     *       (clk, data, en) = self.wait(wait_cond)
     *       self.find_clk_edge(clk, data, en, False)
     *
     * wait_cond = [{0: 'e'}]  or  [{0: 'e'}, {3: 'e'}] */
    while (1) {
        if (priv->have_en)
            ret = c_wait(di, CW_E(CH_CLK), CW_OR, CW_E(CH_EN), CW_END);
        else
            ret = c_wait(di, CW_E(CH_CLK), CW_END);

        if (ret != SRD_OK)
            return;

        uint64_t samplenum = di_samplenum(di);
        uint64_t matched = di_matched(di);

        /* Handle ENABLE — match Python find_clk_edge:
         *   if self.have_en and not self.en_asserted(en):
         *       self.reset_state()
         *       return */
        if (priv->have_en) {
            int en_val = c_pin(di, CH_EN);
            if (!hdlc_en_asserted(priv, en_val)) {
                hdlc_reset_state(priv);
                continue;
            }
            /* Skip if CLK didn't change — match Python:
             *   if first or (not self.matched & (0b1 << 0)):
             *       return */
            if (!(matched & (1ULL << 0)))
                continue;
        }

        /* Check clock edge type — match Python:
         *   if self.options['cpol'] != clk:
         *       return */
        int clk = c_pin(di, CH_CLK);
        if (priv->cpol != clk)
            continue;

        int data = c_pin(di, CH_DATA);
        hdlc_handle_bit(di, priv, data, samplenum);
    }
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder hdlc_c_def = {
    .id = "hdlc_c",
    .name = "HDLC(C)",
    .longname = "High-Level Data Link Control (C)",
    .desc = "HDLC protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = hdlc_channels,
    .num_channels = 2,
    .optional_channels = hdlc_optional_channels,
    .num_optional_channels = 1,
    .options = hdlc_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = hdlc_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = hdlc_ann_rows,
    .inputs = hdlc_inputs,
    .num_inputs = 1,
    .outputs = hdlc_outputs,
    .num_outputs = 1,
    .binary = hdlc_binary,
    .num_binary = 1,
    .tags = hdlc_tags,
    .num_tags = 1,
    .state_size = sizeof(hdlc_s),
    .reset = hdlc_reset,
    .start = hdlc_start,
    .decode = hdlc_decode,
    .end = NULL,
    .metadata = NULL,
    .destroy = hdlc_destroy,
    .decode_upper = NULL,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    hdlc_options[0].def = g_variant_new_string("active-high");
    hdlc_options[1].def = g_variant_new_int64(1);
    return &hdlc_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}