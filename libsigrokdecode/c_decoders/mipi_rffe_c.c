/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2010-2016 Uwe Hermann <uwe@hermann-uwe.de>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    STATE_FIND_SSC,
    STATE_FIND_SLAVE_ADDRESS,
    STATE_FIND_COMMAND,
    STATE_FIND_BYTE_COUNT,
    STATE_FIND_ADDRESS,
    STATE_FIND_DATA,
    STATE_FIND_PARITY,
    STATE_FIND_BUS_PARK,
};

enum {
    CMD_NONE = 0,
    CMD_ERW,
    CMD_ERR,
    CMD_ERWL,
    CMD_ERRL,
    CMD_RW,
    CMD_RR,
    CMD_R0W,
};

enum {
    ANN_SSC = 0,
    ANN_SA,
    ANN_ERW,
    ANN_ERR,
    ANN_ERWL,
    ANN_ERRL,
    ANN_RW,
    ANN_RR,
    ANN_R0W,
    ANN_BC,
    ANN_P,
    ANN_ADDRESS,
    ANN_BP,
    ANN_DATA,
    ANN_CMD_WARNINGS,
    ANN_BIC,
    ANN_BC_WARNINGS,
    ANN_IJE,
    ANN_PW,
    NUM_ANN,
};

#define CH_SCLK  0
#define CH_SDATA 1

typedef struct {
    uint64_t samplerate;
    
    int bitcount;
    uint32_t databyte;
    int state;
    int extended;       /* -1=undetermined, 0=basic, 1=extended */
    int cmdkey;         /* CMD_NONE, CMD_ERW, etc. */
    int BC;
    int bits;           /* remaining data bits */
    int Pcount;
    int BPcount;
    int ADDcount;
    uint64_t BPss;
    uint64_t SSCs;
    uint64_t Pes;
    uint64_t ss;
    uint64_t es;
    int sdata;
    int Pdata;
    int Pkey;
    int parity;
    int isWrite;
    int isLong;
    int _display;       /* 1=display errors, 0=not */
    uint64_t DATAss;
    int out_ann;
    int out_python;
} mipi_rffe_state;

/* Proto annotation mapping: cmd -> [ann_class, long_name, short_name] */
static const struct { int ann_class; const char *long_name; const char *short_name; }
proto_map[] = {
    [0]  = { ANN_SSC,          "Sequence Start Condition",       "SSC" },
    [1]  = { ANN_SA,           "Slave Address",                   "SA" },
    [2]  = { ANN_ERW,          "Extended Register Write",        "ERW" },
    [3]  = { ANN_ERR,          "Extended Register Read",         "ERR" },
    [4]  = { ANN_ERWL,         "Extended Register Write Long",  "ERWL" },
    [5]  = { ANN_ERRL,         "Extended Register Read Long",   "ERRL" },
    [6]  = { ANN_RW,           "Register Write",                  "RW" },
    [7]  = { ANN_RR,           "Register Read",                   "RR" },
    [8]  = { ANN_R0W,          "Register 0 Write",               "R0W" },
    [9]  = { ANN_BC,           "Byte",                            "BC" },
    [10] = { ANN_P,            "Parity",                          "P" },
    [11] = { ANN_ADDRESS,      "Address",                         "A" },
    [12] = { ANN_BP,           "Bus Pack",                       "BP" },
    [13] = { ANN_DATA,         "Data ",                        "DATA" },
    [14] = { ANN_CMD_WARNINGS, "Command Warnings",         "CMD_WARN" },
    [15] = { ANN_BIC,          "Bus Idle Condition ",           "BIC" },
    [16] = { ANN_BC_WARNINGS,  "BC Warnings",               "BC_WARN" },
    [17] = { ANN_IJE,          "Illegal Jump Edge",        "IJE_WAEN" },
    [18] = { ANN_PW,           "Parity warnings",            "P_WAEN" },
};

/* Map cmdkey enum to proto index for command annotation */
static const int cmdkey_to_proto[] = {
    [CMD_NONE] = 0,
    [CMD_ERW]  = 2,
    [CMD_ERR]  = 3,
    [CMD_ERWL] = 4,
    [CMD_ERRL] = 5,
    [CMD_RW]   = 6,
    [CMD_RR]   = 7,
    [CMD_R0W]  = 8,
};

static void rffe_put(mipi_rffe_state *s, struct srd_decoder_inst *di,
                     uint64_t ss, uint64_t es, int ann_class, const char **txts)
{
    struct srd_c_annotation ann;
    ann.ann_class = ann_class;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, ss, es, s->out_ann, &ann);
}



static void rffe_init(mipi_rffe_state *s)
{
    s->cmdkey = CMD_NONE;
    s->ADDcount = 0;
    s->Pcount = 0;
    s->BPcount = 0;
    s->BC = 0;
    s->bitcount = 0;
    s->databyte = 0;
    s->Pes = 0;
    s->state = STATE_FIND_SSC;
    s->extended = -1;
    s->parity = 0;
    s->Pdata = 0;
    s->Pkey = 0;
}

static void rffe_Parity(mipi_rffe_state *s)
{
    s->parity = 1;
    if (s->Pcount == 1) {
        int add_val = 0;
        if (s->cmdkey == CMD_ERW)  add_val = 0;
        if (s->cmdkey == CMD_ERR)  add_val = 2;
        if (s->cmdkey == CMD_ERWL) add_val = 6;
        if (s->cmdkey == CMD_ERRL) add_val = 7;
        if (s->cmdkey == CMD_RW)   add_val = 2;
        if (s->cmdkey == CMD_RR)   add_val = 3;
        if (s->cmdkey == CMD_R0W)  add_val = 1;
        s->Pdata = s->Pdata + (add_val * (1 << (s->Pkey + 1)));
    }
    while (s->Pdata) {
        s->parity = !s->parity;
        s->Pdata = s->Pdata & (s->Pdata - 1);
    }
    s->Pdata = 0;
    s->Pkey = 0;
}

static void rffe_cmdset(mipi_rffe_state *s, struct srd_decoder_inst *di,
                        int cmd, int state)
{
    s->ss = s->DATAss;
    s->es = di_samplenum(di);
    int pidx = cmdkey_to_proto[cmd];
    const char *txts[] = {proto_map[pidx].long_name, proto_map[pidx].short_name, NULL};
    rffe_put(s, di, s->ss, s->es, proto_map[pidx].ann_class, txts);
    s->state = state;
    s->bitcount = 0;
    s->extended = -1;
    s->cmdkey = cmd;
}

/* Read a data byte with optional IJE detection */
static int rffe_read_sclk_fall(mipi_rffe_state *s, struct srd_decoder_inst *di,
                               uint8_t *sdata_out)
{
    if (s->_display) {
        int ret = c_wait(di, CW_F(CH_SCLK), CW_OR, CW_L(CH_SCLK), CW_E(CH_SDATA), CW_END);
        if (ret != SRD_OK)
            return -1;

        if (di_matched(di) & (1 << 0)) {
            /* SCLK falling edge */
            uint8_t sdata = c_pin(di, CH_SDATA);
            if (sdata_out) *sdata_out = sdata;
            return 0;
        }
        if (di_matched(di) & (1 << 1)) {
            /* IJE: SCLK low + SDATA edge */
            s->ss = di_samplenum(di);
            ret = c_wait(di, CW_F(CH_SCLK), CW_OR, CW_L(CH_SCLK), CW_E(CH_SDATA), CW_END);
            if (ret != SRD_OK)
                return -1;

            if (di_matched(di) & (1 << 0)) {
                s->es = di_samplenum(di);
                const char *ije_txts[] = {proto_map[17].long_name, proto_map[17].short_name, NULL};
                rffe_put(s, di, s->ss, s->es, ANN_IJE, ije_txts);
                uint8_t sdata = c_pin(di, CH_SDATA);
                if (sdata_out) *sdata_out = sdata;
                return 0;
            }
            if (di_matched(di) & (1 << 1)) {
                s->es = di_samplenum(di);
                const char *ije_txts[] = {proto_map[17].long_name, proto_map[17].short_name, NULL};
                rffe_put(s, di, s->ss, s->es, ANN_IJE, ije_txts);
                /* Continue reading */
                return rffe_read_sclk_fall(s, di, sdata_out);
            }
            return -1;
        }
        return -1;
    } else {
        int ret = c_wait(di, CW_F(CH_SCLK), CW_END);
        if (ret != SRD_OK)
            return -1;

        uint8_t sdata = c_pin(di, CH_SDATA);
        if (sdata_out) *sdata_out = sdata;
        return 0;
    }
}

static void rffe_handle(mipi_rffe_state *s, struct srd_decoder_inst *di,
                        int cmd, int next_state, int key, int key0)
{
    int key1 = key;
    if (key > 7)
        key = 7;

    if (s->bitcount == 0)
        s->DATAss = s->BPss; /* approximate start */

    /* Read key bits */
    if (s->bitcount < key) {
        uint8_t sdata;
        if (rffe_read_sclk_fall(s, di, &sdata) < 0)
            return;

        /* Wait for SCLK rising edge */
        int ret = c_wait(di, CW_R(CH_SCLK), CW_END);
        if (ret != SRD_OK)
            return;

        s->databyte <<= 1;
        s->databyte |= sdata;
        s->bitcount++;
        return;
    }

    /* Read remaining bits */
    {
        uint8_t sdata;
        if (rffe_read_sclk_fall(s, di, &sdata) < 0)
            return;

        /* Wait for SCLK rising edge */
        int ret = c_wait(di, CW_R(CH_SCLK), CW_END);
        if (ret != SRD_OK)
            return;

        s->databyte <<= 1;
        s->databyte |= sdata;
    }

    uint32_t d = s->databyte;
    s->ss = s->DATAss;
    s->es = di_samplenum(di);

    if (cmd != 10) { /* not 'P' */
        s->Pdata = d;
        s->Pkey = key;
    }

    if (cmd == 9) { /* BC */
        s->BC = d + 1;
        if (s->cmdkey == CMD_ERW || s->cmdkey == CMD_ERR) {
            if (s->BC < 1 || s->BC > 16) {
                const char *bcw_txts[] = {proto_map[16].long_name, proto_map[16].short_name, NULL};
                rffe_put(s, di, s->ss, s->es, ANN_BC_WARNINGS, bcw_txts);
                s->databyte = 0;
                rffe_init(s);
                return;
            }
        } else {
            if (s->BC < 1 || s->BC > 8) {
                const char *bcw_txts[] = {proto_map[16].long_name, proto_map[16].short_name, NULL};
                rffe_put(s, di, s->ss, s->es, ANN_BC_WARNINGS, bcw_txts);
                s->databyte = 0;
                rffe_init(s);
                return;
            }
        }
    }

    if (cmd == 10) { /* P - Parity */
        s->Pes = s->BPss;
        if (s->_display) {
            rffe_Parity(s);
            if (s->parity != (int)d) {
                const char *pw_txts[] = {proto_map[18].long_name, proto_map[18].short_name, NULL};
                rffe_put(s, di, s->ss, s->es, ANN_PW, pw_txts);
            }
        }
        char p_str[32], p_short[16], p_tiny[8];
        snprintf(p_str, sizeof(p_str), "%s: %d", proto_map[10].long_name, d);
        snprintf(p_short, sizeof(p_short), "%s: %d", proto_map[10].short_name, d);
        snprintf(p_tiny, sizeof(p_tiny), "%d", d);
        const char *p_txts[] = {p_str, p_short, p_tiny, NULL};
        rffe_put(s, di, s->ss, s->es, ANN_P, p_txts);

        s->bitcount = 0;
        s->databyte = 0;
        s->state = next_state;
        return;
    }

    char data_str[64], data_short[48], data_tiny[16];
    snprintf(data_str, sizeof(data_str), "%s[%d:%d]: %02X",
             proto_map[cmd].long_name, key1, key0, d);
    snprintf(data_short, sizeof(data_short), "%s[%d:%d]: %02X",
             proto_map[cmd].short_name, key1, key0, d);
    snprintf(data_tiny, sizeof(data_tiny), "%02X", d);
    const char *data_txts[] = {data_str, data_short, data_tiny, NULL};
    rffe_put(s, di, s->ss, s->es, proto_map[cmd].ann_class, data_txts);

    s->bitcount = 0;
    s->databyte = 0;
    if (cmd == 13) /* DATA */
        s->bits -= 8;
    s->state = next_state;
}

static void rffe_handle_CMD(mipi_rffe_state *s, struct srd_decoder_inst *di)
{
    if (s->bitcount == 0) {
        s->DATAss = s->BPss;

        int ret = c_wait(di, CW_F(CH_SCLK), CW_END);
        if (ret != SRD_OK)
            return;

        uint8_t sdata = c_pin(di, CH_SDATA);
        if (sdata) {
            /* Wait for SCLK rise */
            ret = c_wait(di, CW_R(CH_SCLK), CW_END);
            if (ret != SRD_OK)
                return;

            rffe_cmdset(s, di, CMD_R0W, STATE_FIND_DATA);
            return;
        }
    }

    if (s->bitcount == 1) {
        if (s->sdata)
            s->extended = 0;
        else
            s->extended = 1;
    }

    if (s->bitcount == 2) {
        if (!s->extended) {
            if (s->sdata) {
                rffe_cmdset(s, di, CMD_RR, STATE_FIND_ADDRESS);
                return;
            } else {
                rffe_cmdset(s, di, CMD_RW, STATE_FIND_ADDRESS);
                return;
            }
        } else {
            if (s->sdata)
                s->isWrite = 0;
            else
                s->isWrite = 1;
        }
    }

    if (s->bitcount == 3) {
        if (s->extended) {
            if (!s->sdata) {
                if (!s->isWrite) {
                    rffe_cmdset(s, di, CMD_ERR, STATE_FIND_BYTE_COUNT);
                    return;
                } else {
                    rffe_cmdset(s, di, CMD_ERW, STATE_FIND_BYTE_COUNT);
                    return;
                }
            }
        } else {
            s->ss = s->DATAss;
            s->es = s->BPss;
            const char *cmdw_txts[] = {proto_map[14].long_name, proto_map[14].short_name, NULL};
            rffe_put(s, di, s->ss, s->es, ANN_CMD_WARNINGS, cmdw_txts);
            rffe_init(s);
            return;
        }
    }

    if (s->bitcount == 4) {
        if (s->sdata) {
            rffe_cmdset(s, di, CMD_ERRL, STATE_FIND_BYTE_COUNT);
            return;
        } else {
            rffe_cmdset(s, di, CMD_ERWL, STATE_FIND_BYTE_COUNT);
            return;
        }
    }

    /* Read next command bit */
    if (s->bitcount < 4) {
        
        if (s->_display) {
            int ret = c_wait(di, CW_F(CH_SCLK), CW_OR, CW_L(CH_SCLK), CW_E(CH_SDATA), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1 << 0)) {
                s->sdata = c_pin(di, CH_SDATA);
            }
            if (di_matched(di) & (1 << 1)) {
                s->ss = di_samplenum(di);
                ret = c_wait(di, CW_F(CH_SCLK), CW_OR, CW_L(CH_SCLK), CW_E(CH_SDATA), CW_END);
                if (ret != SRD_OK)
                    return;

                if (di_matched(di) & (1 << 0)) {
                    s->es = di_samplenum(di);
                    const char *ije_txts[] = {proto_map[17].long_name, proto_map[17].short_name, NULL};
                    rffe_put(s, di, s->ss, s->es, ANN_IJE, ije_txts);
                    s->sdata = c_pin(di, CH_SDATA);
                }
                if (di_matched(di) & (1 << 1)) {
                    s->es = di_samplenum(di);
                    const char *ije_txts[] = {proto_map[17].long_name, proto_map[17].short_name, NULL};
                    rffe_put(s, di, s->ss, s->es, ANN_IJE, ije_txts);
                }
            }
        } else {
            int ret = c_wait(di, CW_F(CH_SCLK), CW_END);
            if (ret != SRD_OK)
                return;
            s->sdata = c_pin(di, CH_SDATA);
        }

        /* Wait for SCLK rise */
        int ret = c_wait(di, CW_R(CH_SCLK), CW_END);
        if (ret != SRD_OK)
            return;

        s->bitcount++;
    }
}

static struct srd_channel mipi_rffe_channels[] = {
    {"sclk", "SCLK", "Serial clock line", 0, SRD_CHANNEL_SCLK, "dec_mipi_rffe_chan_sclk"},
    {"sdata", "SDATA", "Serial data line", 1, SRD_CHANNEL_SDATA, "dec_mipi_rffe_chan_sdata"},
};

static struct srd_decoder_option mipi_rffe_options[] = {
    {"error_display", "dec_mipi_rffe_opt_error_display", "Error display options", NULL, NULL},
};

static const char *mipi_rffe_ann_labels[][3] = {
    {"", "ssc", "Sequence Start Condition"},
    {"", "sa", "Slave Address"},
    {"", "erw", "Extended register write"},
    {"", "err", "Extended register read"},
    {"", "erwl", "Extended register write long"},
    {"", "errl", "Extended register read long"},
    {"", "rw", "Register write"},
    {"", "rr", "Register read"},
    {"", "r0w", "Register 0 write"},
    {"", "bc", "Byte"},
    {"", "p", "Parity"},
    {"", "address", "Address"},
    {"", "bp", "Bus pack"},
    {"", "data", "DATA"},
    {"", "cmd_warnings", "Command warnings"},
    {"", "bic", "Bus Idle Condition"},
    {"", "bc_warnings", "BC warnings"},
    {"", "ije", "Illegal Jump Edge"},
    {"", "pw", "Parity warnings"},
};

static const int mipi_rffe_row_cmd_classes[] = {
    ANN_SSC, ANN_SA, ANN_ERW, ANN_ERR, ANN_ERWL, ANN_ERRL,
    ANN_RW, ANN_RR, ANN_R0W, ANN_BC, ANN_P, ANN_ADDRESS, ANN_BP, ANN_DATA, ANN_BIC
};
static const int mipi_rffe_row_warnings_classes[] = {ANN_CMD_WARNINGS, ANN_BC_WARNINGS, ANN_IJE, ANN_PW};
static const struct srd_c_ann_row mipi_rffe_ann_rows[] = {
    {"command-data", "Command/Data", mipi_rffe_row_cmd_classes, 15},
    {"warnings", "Warnings", mipi_rffe_row_warnings_classes, 4},
};

static const char *mipi_rffe_inputs[] = {"logic"};
static const char *mipi_rffe_outputs[] = {"mipi_rffe"};
static const char *mipi_rffe_tags[] = {"Embedded/industrial"};

static void mipi_rffe_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(mipi_rffe_state)));
    mipi_rffe_state *s = (mipi_rffe_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mipi_rffe_state));
    s->out_ann = 0;
    s->out_python = -1;
    s->extended = -1;
    rffe_init(s);
}

static void mipi_rffe_start(struct srd_decoder_inst *di)
{
    mipi_rffe_state *s = (mipi_rffe_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mipi_rffe");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "mipi_rffe");

    const char *err_disp = c_opt_str(di, "error_display", "display");
    s->_display = (strcmp(err_disp, "display") == 0) ? 1 : 0;
    s->samplerate = c_samplerate(di);
}

static void mipi_rffe_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    mipi_rffe_state *s = (mipi_rffe_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void mipi_rffe_decode(struct srd_decoder_inst *di)
{
    mipi_rffe_state *s = (mipi_rffe_state *)c_decoder_get_private(di);
    while (1) {
        if (s->state == STATE_FIND_SSC) {
            /* Wait for SCLK low + SDATA rising edge */
            int ret = c_wait(di, CW_L(CH_SCLK), CW_R(CH_SDATA), CW_END);
            if (ret != SRD_OK)
                return;

            s->BPss = di_samplenum(di);

            /* Wait for SCLK high OR SCLK low + SDATA falling edge */
            ret = c_wait(di, CW_H(CH_SCLK), CW_OR, CW_L(CH_SCLK), CW_F(CH_SDATA), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1 << 0))
                continue; /* SCLK high, not SSC */

            /* SCLK low + SDATA falling edge */
            ret = c_wait(di, CW_L(CH_SCLK), CW_E(CH_SDATA), CW_OR, CW_R(CH_SCLK), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1 << 0))
                continue; /* SDATA edge while SCLK low */

            /* SCLK rising edge = SSC found */
            s->ss = s->BPss;
            s->es = di_samplenum(di);
            const char *ssc_txts[] = {proto_map[0].long_name, proto_map[0].short_name, NULL};
            rffe_put(s, di, s->ss, s->es, ANN_SSC, ssc_txts);
            s->state = STATE_FIND_SLAVE_ADDRESS;

        } else if (s->state == STATE_FIND_SLAVE_ADDRESS) {
            s->BPss = di_samplenum(di);
            rffe_handle(s, di, 1, STATE_FIND_COMMAND, 3, 0); /* SA */

        } else if (s->state == STATE_FIND_COMMAND) {
            s->BPss = di_samplenum(di);
            rffe_handle_CMD(s, di);

        } else if (s->state == STATE_FIND_BYTE_COUNT) {
            s->BPss = di_samplenum(di);
            if (s->cmdkey == CMD_ERW || s->cmdkey == CMD_ERR)
                rffe_handle(s, di, 9, STATE_FIND_PARITY, 3, 0); /* BC */
            else
                rffe_handle(s, di, 9, STATE_FIND_PARITY, 2, 0); /* BC */
            s->bits = s->BC * 8;

        } else if (s->state == STATE_FIND_ADDRESS) {
            s->BPss = di_samplenum(di);
            if (s->cmdkey == CMD_RW || s->cmdkey == CMD_RR)
                rffe_handle(s, di, 11, STATE_FIND_PARITY, 4, 0); /* ADDRESS */
            else if (s->cmdkey == CMD_ERR || s->cmdkey == CMD_ERW)
                rffe_handle(s, di, 11, STATE_FIND_PARITY, 7, 0); /* ADDRESS */
            else {
                if (s->Pcount == 1)
                    rffe_handle(s, di, 11, STATE_FIND_PARITY, 15, 8); /* ADDRESS high */
                else
                    rffe_handle(s, di, 11, STATE_FIND_PARITY, 7, 0); /* ADDRESS */
            }

        } else if (s->state == STATE_FIND_DATA) {
            s->BPss = di_samplenum(di);
            if (s->cmdkey == CMD_R0W)
                rffe_handle(s, di, 13, STATE_FIND_PARITY, 6, 0); /* DATA */
            else if (s->cmdkey == CMD_RW || s->cmdkey == CMD_RR)
                rffe_handle(s, di, 13, STATE_FIND_PARITY, 7, 0); /* DATA */
            else
                rffe_handle(s, di, 13, STATE_FIND_PARITY, s->bits - 1, s->bits - 8); /* DATA */

        } else if (s->state == STATE_FIND_PARITY) {
            s->BPss = di_samplenum(di);
            s->Pcount++;
            rffe_handle(s, di, 10, STATE_FIND_PARITY, 0, 0); /* P */

            if (s->cmdkey == CMD_R0W) {
                s->state = STATE_FIND_BUS_PARK;
            } else if (s->cmdkey == CMD_ERW) {
                if (s->Pcount == 1) { s->ADDcount = 1; s->state = STATE_FIND_ADDRESS; }
                else if (s->Pcount == 2) { s->state = STATE_FIND_DATA; }
                else if (s->Pcount == s->BC + 2) { s->state = STATE_FIND_BUS_PARK; continue; }
                else if (s->Pcount > 2) { s->state = STATE_FIND_DATA; }
            } else if (s->cmdkey == CMD_ERR) {
                if (s->Pcount == 1) { s->ADDcount = 1; s->state = STATE_FIND_ADDRESS; }
                else if (s->Pcount == 2) { s->BPcount = 1; s->state = STATE_FIND_BUS_PARK; }
                else if (s->Pcount == s->BC + 2) { s->BPcount = 2; s->state = STATE_FIND_BUS_PARK; continue; }
                else if (s->Pcount > 2) { s->state = STATE_FIND_DATA; }
            } else if (s->cmdkey == CMD_ERWL) {
                if (s->Pcount == 1) { s->ADDcount = 2; s->state = STATE_FIND_ADDRESS; }
                else if (s->Pcount == 2) { s->ADDcount = 1; s->state = STATE_FIND_ADDRESS; }
                else if (s->Pcount == 3) { s->state = STATE_FIND_DATA; }
                else if (s->Pcount == s->BC + 3) { s->state = STATE_FIND_BUS_PARK; continue; }
                else if (s->Pcount > 3) { s->state = STATE_FIND_DATA; }
            } else if (s->cmdkey == CMD_ERRL) {
                if (s->Pcount == 1) { s->ADDcount = 2; s->state = STATE_FIND_ADDRESS; }
                else if (s->Pcount == 2) { s->ADDcount = 1; s->state = STATE_FIND_ADDRESS; }
                else if (s->Pcount == 3) { s->BPcount = 1; s->state = STATE_FIND_BUS_PARK; }
                else if (s->Pcount == s->BC + 3) { s->BPcount = 2; s->state = STATE_FIND_BUS_PARK; continue; }
                else if (s->Pcount > 3) { s->state = STATE_FIND_DATA; }
            } else if (s->cmdkey == CMD_RW) {
                if (s->Pcount == 1) { s->state = STATE_FIND_DATA; }
                else if (s->Pcount == 2) { s->BPcount = 1; s->state = STATE_FIND_BUS_PARK; }
            } else if (s->cmdkey == CMD_RR) {
                s->state = STATE_FIND_BUS_PARK;
                s->BPcount = (s->Pcount == 1) ? 1 : 2;
            }

        } else if (s->state == STATE_FIND_BUS_PARK) {
            int ret = c_wait(di, CW_L(CH_SCLK), CW_L(CH_SDATA), CW_END);
            if (ret != SRD_OK)
                return;

            s->ss = di_samplenum(di);
            s->es = di_samplenum(di);
            const char *bp_txts[] = {proto_map[12].long_name, proto_map[12].short_name, NULL};
            rffe_put(s, di, s->ss, s->es, ANN_BP, bp_txts);

            if (s->cmdkey == CMD_ERR || s->cmdkey == CMD_ERRL || s->cmdkey == CMD_RR) {
                int key = (s->BPcount == 1) ? 0 : 1;
                if (key)
                    rffe_init(s);
                else
                    s->state = STATE_FIND_DATA;
            } else {
                rffe_init(s);
            }
        }
    }
}

static void mipi_rffe_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder mipi_rffe_c_decoder = {
    .id = "mipi_rffe_c",
    .name = "MIPI_RFFE(C)",
    .longname = "RF Front-End Control Interface (C)",
    .desc = "Two-wire, single-master, serial bus. (C implementation)",
    .license = "gplv2+",
    .channels = mipi_rffe_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = mipi_rffe_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = mipi_rffe_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = mipi_rffe_ann_rows,
    .reset = mipi_rffe_reset,
    .start = mipi_rffe_start,
    .decode = mipi_rffe_decode,
    .metadata = mipi_rffe_metadata,
    .destroy = mipi_rffe_destroy,
    .state_size = 0,
    .inputs = mipi_rffe_inputs,
    .num_inputs = 1,
    .outputs = mipi_rffe_outputs,
    .num_outputs = 1,
    .tags = mipi_rffe_tags,
    .num_tags = 1,
    .binary = NULL,
    .num_binary = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    mipi_rffe_options[0].def = g_variant_new_string("display");
    {
        GVariant *v0 = g_variant_new_string("display");
        GVariant *v1 = g_variant_new_string("not_display");
        GSList *vals = g_slist_append(NULL, v0);
        vals = g_slist_append(vals, v1);
        mipi_rffe_options[0].values = vals;
    }
    return &mipi_rffe_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}