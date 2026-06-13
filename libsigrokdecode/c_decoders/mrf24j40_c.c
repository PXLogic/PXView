/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Karl Palsson <karlp@tweak.net.au>
 * Copyright (C) 2022 DreamSourceLab <support@dreamsourcelab.com>
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
    ANN_SREAD = 0,
    ANN_SWRITE,
    ANN_LREAD,
    ANN_LWRITE,
    ANN_WARNING,
    ANN_TX_FRAME,
    ANN_RX_FRAME,
    ANN_TX_RETRY_1,
    ANN_TX_RETRY_2,
    ANN_TX_RETRY_3,
    ANN_TX_FAIL,
    ANN_CCAFAIL,
    NUM_ANN,
};

#define MRF24J40_MAX_FRAME 256
#define MRF24J40_TX 1
#define MRF24J40_RX 0

typedef struct {
    int out_ann;
    uint8_t mosi_bytes[4];
    uint8_t miso_bytes[4];
    int byte_count;
    uint64_t ss_cmd;
    uint64_t es_cmd;
    uint64_t ss_frame[2];  /* [RX=0, TX=1] */
    uint64_t es_frame[2];
    uint8_t framecache[2][MRF24J40_MAX_FRAME];
    int framecache_len[2];
} mrf24j40_state;

static const char *mrf24j40_inputs[] = {"spi", NULL};
static const char *mrf24j40_tags[] = {"IC", "Wireless/RF", NULL};

static const char *mrf24j40_ann_labels[][3] = {
    {"", "sread", "Short register read"},
    {"", "swrite", "Short register write"},
    {"", "lread", "Long register read"},
    {"", "lwrite", "Long register write"},
    {"", "warning", "Warning"},
    {"", "tx-frame", "TX frame"},
    {"", "rx-frame", "RX frame"},
    {"", "tx-retry-1", "1x TX retry"},
    {"", "tx-retry-2", "2x TX retry"},
    {"", "tx-retry-3", "3x TX retry"},
    {"", "tx-fail", "TX fail (too many retries)"},
    {"", "ccafail", "CCAFAIL (channel busy)"},
};

static const int mrf24j40_row_reads_classes[] = {ANN_SREAD, ANN_LREAD, -1};
static const int mrf24j40_row_writes_classes[] = {ANN_SWRITE, ANN_LWRITE, -1};
static const int mrf24j40_row_warnings_classes[] = {ANN_WARNING, -1};
static const int mrf24j40_row_tx_classes[] = {ANN_TX_FRAME, -1};
static const int mrf24j40_row_rx_classes[] = {ANN_RX_FRAME, -1};
static const int mrf24j40_row_retry1_classes[] = {ANN_TX_RETRY_1, -1};
static const int mrf24j40_row_retry2_classes[] = {ANN_TX_RETRY_2, -1};
static const int mrf24j40_row_retry3_classes[] = {ANN_TX_RETRY_3, -1};
static const int mrf24j40_row_txfail_classes[] = {ANN_TX_FAIL, -1};
static const int mrf24j40_row_ccafail_classes[] = {ANN_CCAFAIL, -1};

static const struct srd_c_ann_row mrf24j40_ann_rows[] = {
    {"reads", "Reads", mrf24j40_row_reads_classes, 2},
    {"writes", "Writes", mrf24j40_row_writes_classes, 2},
    {"warnings", "Warnings", mrf24j40_row_warnings_classes, 1},
    {"tx-frames", "TX frames", mrf24j40_row_tx_classes, 1},
    {"rx-frames", "RX frames", mrf24j40_row_rx_classes, 1},
    {"tx-retries-1", "1x TX retries", mrf24j40_row_retry1_classes, 1},
    {"tx-retries-2", "2x TX retries", mrf24j40_row_retry2_classes, 1},
    {"tx-retries-3", "3x TX retries", mrf24j40_row_retry3_classes, 1},
    {"tx-fails", "TX fails", mrf24j40_row_txfail_classes, 1},
    {"ccafails", "CCAFAILs", mrf24j40_row_ccafail_classes, 1},
};

/* Short register name table (0x00-0x3F) */
static const char *mrf24j40_sregs[64] = {
    "RXMCR", "PANIDL", "PANIDH", "SADRL", "SADRH",
    "EADR0", "EADR1", "EADR2", "EADR3", "EADR4",
    "EADR5", "EADR6", "EADR7", "RXFLUSH", "Reserved", "Reserved",
    "ORDER", "TXMCR", "ACKTMOUT", "ESLOTG1", "SYMTICKL",
    "SYMTICKH", "PACON0", "PACON1", "PACON2", "Reserved",
    "TXBCON0", "TXNCON", "TXG1CON", "TXG2CON", "ESLOTG23",
    "ESLOTG45", "ESLOTG67", "TXPEND", "WAKECON", "FRMOFFSET",
    "TXSTAT", "TXBCON1", "GATECLK", "TXTIME", "HSYMTIMRL",
    "HSYMTIMRH", "SOFTRST", "Reserved", "SECCON0", "SECCON1",
    "TXSTBL", "Reserved", "RXSR", "INTSTAT", "INTCON",
    "GPIO", "TRISGPIO", "SLPACK", "RFCTL", "SECCR2",
    "BBREG0", "BBREG1", "BBREG2", "BBREG3", "BBREG4",
    "Reserved", "BBREG6", "CCAEDTH",
};

/* Long register name lookup */
static const char *mrf24j40_get_lreg_name(uint16_t reg)
{
    switch (reg) {
    case 0x200: return "RFCON0";
    case 0x201: return "RFCON1";
    case 0x202: return "RFCON2";
    case 0x203: return "RFCON3";
    case 0x204: return "Reserved";
    case 0x205: return "RFCON5";
    case 0x206: return "RFCON6";
    case 0x207: return "RFCON7";
    case 0x208: return "RFCON8";
    case 0x209: return "SLPCAL0";
    case 0x20A: return "SLPCAL1";
    case 0x20B: return "SLPCAL2";
    case 0x20C: return "Reserved";
    case 0x20D: return "Reserved";
    case 0x20E: return "Reserved";
    case 0x20F: return "RFSTATE";
    case 0x210: return "RSSI";
    case 0x211: return "SLPCON0";
    case 0x220: return "SLPCON1";
    case 0x222: return "WAKETIMEL";
    case 0x223: return "WAKETIMEH";
    case 0x224: return "REMCNTL";
    case 0x225: return "REMCNTH";
    case 0x226: return "MAINCNT0";
    case 0x227: return "MAINCNT1";
    case 0x228: return "MAINCNT2";
    case 0x229: return "MAINCNT3";
    case 0x22F: return "TESTMODE";
    case 0x230: return "ASSOEADR0";
    case 0x231: return "ASSOEADR1";
    case 0x232: return "ASSOEADR2";
    case 0x233: return "ASSOEADR3";
    case 0x234: return "ASSOEADR4";
    case 0x235: return "ASSOEADR5";
    case 0x236: return "ASSOEADR6";
    case 0x237: return "ASSOEADR7";
    case 0x238: return "ASSOSADR0";
    case 0x239: return "ASSOSADR1";
    case 0x240: return "UPNONCE0";
    case 0x241: return "UPNONCE1";
    case 0x242: return "UPNONCE2";
    case 0x243: return "UPNONCE3";
    case 0x244: return "UPNONCE4";
    case 0x245: return "UPNONCE5";
    case 0x246: return "UPNONCE6";
    case 0x247: return "UPNONCE7";
    case 0x248: return "UPNONCE8";
    case 0x249: return "UPNONCE9";
    case 0x24A: return "UPNONCE10";
    case 0x24B: return "UPNONCE11";
    case 0x24C: return "UPNONCE12";
    default: return NULL;
    }
}

static const char *mrf24j40_get_lreg_desc(uint16_t reg)
{
    if (reg < 0x080) return "TX";
    if (reg < 0x100) return "TX beacon";
    if (reg < 0x180) return "TX GTS1";
    if (reg < 0x200) return "TX GTS2";
    if (reg >= 0x200 && reg < 0x280) {
        const char *name = mrf24j40_get_lreg_name(reg);
        if (name) return name;
        return "illegal";
    }
    if (reg < 0x2C0) return "Security keys";
    if (reg < 0x300) return "Reserved";
    return "RX";
}

static void mrf24j40_reset_data(mrf24j40_state *s)
{
    s->byte_count = 0;
}

static void mrf24j40_handle_short(struct srd_decoder_inst *di, mrf24j40_state *s)
{
    int write = s->mosi_bytes[0] & 0x1;
    int reg = (s->mosi_bytes[0] >> 1) & 0x3F;
    const char *reg_desc = (reg < 64) ? mrf24j40_sregs[reg] : "illegal";

    /* Check frame cache for TX/RX frame output */
    for (int rxtx = 0; rxtx < 2; rxtx++) {
        if (s->framecache_len[rxtx] == 0)
            continue;
        int bit0 = s->mosi_bytes[1] & 0x01;
        if (rxtx == MRF24J40_TX && !(strcmp(reg_desc, "TXNCON") == 0 && bit0 == 1))
            continue;
        if (rxtx == MRF24J40_RX && !(strcmp(reg_desc, "RXFLUSH") == 0 && bit0 == 1))
            continue;
        int idx = (rxtx == MRF24J40_TX) ? ANN_TX_FRAME : ANN_RX_FRAME;
        const char *xmitdir = (rxtx == MRF24J40_TX) ? "TX" : "RX";

        /* Build frame hex string */
        char frame_str[MRF24J40_MAX_FRAME * 4];
        int fpos = 0;
        for (int i = 0; i < s->framecache_len[rxtx] && fpos < (int)sizeof(frame_str) - 4; i++) {
            if (i > 0) fpos += snprintf(frame_str + fpos, sizeof(frame_str) - fpos, " ");
            fpos += snprintf(frame_str + fpos, sizeof(frame_str) - fpos, "%02X", s->framecache[rxtx][i]);
        }

        char ann_buf[MRF24J40_MAX_FRAME * 4 + 32];
        snprintf(ann_buf, sizeof(ann_buf), "%s frame: %s", xmitdir, frame_str);
        c_put(di, s->ss_frame[rxtx], s->es_frame[rxtx], s->out_ann, idx, ann_buf);
        s->framecache_len[rxtx] = 0;
    }

    if (write) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: 0x%02X", reg_desc, s->mosi_bytes[1]);
        c_put_v(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_SWRITE,
            s->mosi_bytes[1], buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: 0x%02X", reg_desc, s->miso_bytes[1]);
        c_put_v(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_SREAD,
            s->miso_bytes[1], buf);

        /* Check TXSTAT for retries and CCAFAIL */
        if (strcmp(reg_desc, "TXSTAT") == 0) {
            int numretries = (s->miso_bytes[1] & 0xC0) >> 6;
            if (numretries > 0) {
                int txfail = (s->miso_bytes[1] & 0x01) ? 1 : 0;
                int idx = 6 + numretries + txfail;
                if (idx >= NUM_ANN) idx = NUM_ANN - 1;
                if (txfail) {
                    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_TX_FAIL,
                        "TX fail (>= 4 retries)", "TX fail");
                } else {
                    char retry_buf[32];
                    snprintf(retry_buf, sizeof(retry_buf), "TX retries: %d", numretries);
                    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, idx, retry_buf);
                }
            }
            if (s->miso_bytes[1] & (1 << 5)) {
                c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_CCAFAIL,
                    "CCAFAIL (channel busy)", "CCAFAIL");
            }
        }
    }
}

static void mrf24j40_handle_long(struct srd_decoder_inst *di, mrf24j40_state *s)
{
    uint16_t dword = ((uint16_t)s->mosi_bytes[0] << 8) | s->mosi_bytes[1];
    int write = dword & (1 << 4);
    uint16_t reg = (dword >> 5) & 0x3FF;

    const char *reg_desc = mrf24j40_get_lreg_desc(reg);

    if (write) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: 0x%02X", reg_desc, s->mosi_bytes[2]);
        c_put_v(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_LWRITE,
            s->mosi_bytes[2], buf);
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s: 0x%02X", reg_desc, s->miso_bytes[2]);
        c_put_v(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_LREAD,
            s->miso_bytes[2], buf);
    }

    /* Accumulate frame cache for TX/RX regions */
    for (int rxtx = 0; rxtx < 2; rxtx++) {
        if (rxtx == MRF24J40_RX && strncmp(reg_desc, "RX:", 3) != 0)
            continue;
        if (rxtx == MRF24J40_TX && strncmp(reg_desc, "TX", 2) != 0)
            continue;

        if (s->framecache_len[rxtx] == 0)
            s->ss_frame[rxtx] = s->ss_cmd;
        s->es_frame[rxtx] = s->es_cmd;

        if (s->framecache_len[rxtx] < MRF24J40_MAX_FRAME) {
            if (rxtx == MRF24J40_TX)
                s->framecache[rxtx][s->framecache_len[rxtx]++] = s->mosi_bytes[2];
            else
                s->framecache[rxtx][s->framecache_len[rxtx]++] = s->miso_bytes[2];
        }
    }
}

static void mrf24j40_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    mrf24j40_state *s = (mrf24j40_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        if (fields && n_fields >= 2 && fields[0].u8 == 0 && fields[1].u8 == 1) {
            /* CS deasserted mid-stream */
            if (s->byte_count != 0 && s->byte_count != 2 && s->byte_count != 3) {
                c_put(di, s->ss_cmd, end_sample, s->out_ann, ANN_WARNING, "Misplaced CS!");
                mrf24j40_reset_data(s);
            }
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0) return;
    if (n_fields < 17) return;

    
    int have_mosi = (fields[0].u8 & 1) ? 1 : 0;
    int have_miso = (fields[0].u8 & 2) ? 1 : 0;
    uint8_t mosi = have_mosi ? fields[1].u8 : 0;
    uint8_t miso = have_miso ? fields[9].u8 : 0;

    if (s->byte_count == 0)
        s->ss_cmd = start_sample;

    if (s->byte_count < 4) {
        s->mosi_bytes[s->byte_count] = mosi;
        s->miso_bytes[s->byte_count] = miso;
    }
    s->byte_count++;

    if (s->byte_count < 2) return;

    if (s->mosi_bytes[0] & 0x80) {
        /* Long register access - need 3 bytes */
        if (s->byte_count == 3) {
            s->es_cmd = end_sample;
            mrf24j40_handle_long(di, s);
            mrf24j40_reset_data(s);
        }
    } else {
        /* Short register access - 2 bytes */
        s->es_cmd = end_sample;
        mrf24j40_handle_short(di, s);
        mrf24j40_reset_data(s);
    }
}

static void mrf24j40_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(mrf24j40_state)));
    }
    mrf24j40_state *s = (mrf24j40_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mrf24j40_state));
}

static void mrf24j40_start(struct srd_decoder_inst *di)
{
    mrf24j40_state *s = (mrf24j40_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mrf24j40");
}

static void mrf24j40_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void mrf24j40_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder mrf24j40_c_decoder = {
    .id = "mrf24j40_c",
    .name = "MRF24J40(C)",
    .longname = "Microchip MRF24J40 (C)",
    .desc = "IEEE 802.15.4 2.4 GHz RF tranceiver chip. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mrf24j40_ann_labels,
    .num_annotation_rows = 10,
    .annotation_rows = mrf24j40_ann_rows,
    .inputs = mrf24j40_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = mrf24j40_tags,
    .num_tags = 2,
    .reset = mrf24j40_reset,
    .start = mrf24j40_start,
    .decode = mrf24j40_decode,
    .destroy = mrf24j40_destroy,
    .decode_upper = mrf24j40_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &mrf24j40_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}