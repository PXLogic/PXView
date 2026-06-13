/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012-2020 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2019 Zhiyuan Wan <dv.xw@qq.com>
 * Copyright (C) 2019 Kongou Hikari <hikari@iloli.bid>
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

/* Channel indices */
#define CH_TCKC 0
#define CH_TMSC 1

/* JTAG state machine states (16) */
enum jtag_state {
    ST_TEST_LOGIC_RESET = 0,
    ST_RUN_TEST_IDLE,
    ST_SELECT_DR_SCAN,
    ST_CAPTURE_DR,
    ST_UPDATE_DR,
    ST_PAUSE_DR,
    ST_SHIFT_DR,
    ST_EXIT1_DR,
    ST_EXIT2_DR,
    ST_SELECT_IR_SCAN,
    ST_CAPTURE_IR,
    ST_UPDATE_IR,
    ST_PAUSE_IR,
    ST_SHIFT_IR,
    ST_EXIT1_IR,
    ST_EXIT2_IR,
    NUM_JTAG_STATES,
};

/* cJTAG states (12) */
enum cjtag_state {
    CST_CJTAG_EC = 0,
    CST_CJTAG_SPARE,
    CST_CJTAG_TPDEL,
    CST_CJTAG_TPREV,
    CST_CJTAG_TPST,
    CST_CJTAG_RDYC,
    CST_CJTAG_DLYC,
    CST_CJTAG_SCNFMT,
    CST_CJTAG_CP,
    CST_CJTAG_OAC,
    CST_OSCAN1,
    CST_FOUR_WIRE,
    NUM_CJTAG_STATES,
};

/* Annotation indices */
enum cjtag_ann {
    /* JTAG states: 0-15 */
    ANN_TEST_LOGIC_RESET = 0,
    ANN_RUN_TEST_IDLE,
    ANN_SELECT_DR_SCAN,
    ANN_CAPTURE_DR,
    ANN_UPDATE_DR,
    ANN_PAUSE_DR,
    ANN_SHIFT_DR,
    ANN_EXIT1_DR,
    ANN_EXIT2_DR,
    ANN_SELECT_IR_SCAN,
    ANN_CAPTURE_IR,
    ANN_UPDATE_IR,
    ANN_PAUSE_IR,
    ANN_SHIFT_IR,
    ANN_EXIT1_IR,
    ANN_EXIT2_IR,
    /* cJTAG states: 16-27 */
    ANN_CJTAG_EC = NUM_JTAG_STATES,
    ANN_CJTAG_SPARE,
    ANN_CJTAG_TPDEL,
    ANN_CJTAG_TPREV,
    ANN_CJTAG_TPST,
    ANN_CJTAG_RDYC,
    ANN_CJTAG_DLYC,
    ANN_CJTAG_SCNFMT,
    ANN_CJTAG_CP,
    ANN_CJTAG_OAC,
    ANN_OSCAN1,
    ANN_FOUR_WIRE,
    /* Bit annotations: 28-32 */
    ANN_BIT_TDI = 28,
    ANN_BIT_TDO = 29,
    ANN_BITSTRING_TDI = 30,
    ANN_BITSTRING_TDO = 31,
    ANN_BIT_TMS = 32,
    NUM_ANN = 33,
};

/* Private state */
typedef struct {
    /* JTAG state */
    int state;
    int oldstate;

    /* cJTAG state */
    int cjtagstate;
    int oldcjtagstate;
    int escape_edges;
    int oaclen;
    int oldtms;
    int oacp;
    int oscan1cycle;

    /* TDI/TDO bit collection */
    int bits_tdi[256];
    int bits_tdo[256];
    uint64_t bits_ss_tdi[256];
    uint64_t bits_es_tdi[256];
    uint64_t bits_ss_tdo[256];
    uint64_t bits_es_tdo[256];
    int bits_cnt;

    /* Annotation ranges */
    uint64_t ss_item;
    uint64_t es_item;
    uint64_t ss_bitstring;
    uint64_t es_bitstring;

    /* Flags */
    int first;
    int first_bit;

    /* Output IDs */
    int out_ann;
    int out_python;
} cjtag_priv;

/* JTAG state transition table: next_state[current_state][tms] */
static const int jtag_next_state[16][2] = {
    /* TEST_LOGIC_RESET: tms=0->RUN_TEST_IDLE, tms=1->TEST_LOGIC_RESET */
    { ST_RUN_TEST_IDLE, ST_TEST_LOGIC_RESET },
    /* RUN_TEST_IDLE: tms=0->RUN_TEST_IDLE, tms=1->SELECT_DR_SCAN */
    { ST_RUN_TEST_IDLE, ST_SELECT_DR_SCAN },
    /* SELECT_DR_SCAN: tms=0->CAPTURE_DR, tms=1->SELECT_IR_SCAN */
    { ST_CAPTURE_DR, ST_SELECT_IR_SCAN },
    /* CAPTURE_DR: tms=0->SHIFT_DR, tms=1->EXIT1_DR */
    { ST_SHIFT_DR, ST_EXIT1_DR },
    /* UPDATE_DR: tms=0->RUN_TEST_IDLE, tms=1->SELECT_DR_SCAN */
    { ST_RUN_TEST_IDLE, ST_SELECT_DR_SCAN },
    /* PAUSE_DR: tms=0->PAUSE_DR, tms=1->EXIT2_DR */
    { ST_PAUSE_DR, ST_EXIT2_DR },
    /* SHIFT_DR: tms=0->SHIFT_DR, tms=1->EXIT1_DR */
    { ST_SHIFT_DR, ST_EXIT1_DR },
    /* EXIT1_DR: tms=0->PAUSE_DR, tms=1->UPDATE_DR */
    { ST_PAUSE_DR, ST_UPDATE_DR },
    /* EXIT2_DR: tms=0->SHIFT_DR, tms=1->UPDATE_DR */
    { ST_SHIFT_DR, ST_UPDATE_DR },
    /* SELECT_IR_SCAN: tms=0->CAPTURE_IR, tms=1->TEST_LOGIC_RESET */
    { ST_CAPTURE_IR, ST_TEST_LOGIC_RESET },
    /* CAPTURE_IR: tms=0->SHIFT_IR, tms=1->EXIT1_IR */
    { ST_SHIFT_IR, ST_EXIT1_IR },
    /* UPDATE_IR: tms=0->RUN_TEST_IDLE, tms=1->SELECT_DR_SCAN */
    { ST_RUN_TEST_IDLE, ST_SELECT_DR_SCAN },
    /* PAUSE_IR: tms=0->PAUSE_IR, tms=1->EXIT2_IR */
    { ST_PAUSE_IR, ST_EXIT2_IR },
    /* SHIFT_IR: tms=0->SHIFT_IR, tms=1->EXIT1_IR */
    { ST_SHIFT_IR, ST_EXIT1_IR },
    /* EXIT1_IR: tms=0->PAUSE_IR, tms=1->UPDATE_IR */
    { ST_PAUSE_IR, ST_UPDATE_IR },
    /* EXIT2_IR: tms=0->SHIFT_IR, tms=1->UPDATE_IR */
    { ST_SHIFT_IR, ST_UPDATE_IR },
};

/* Channel definitions */
static struct srd_channel cjtag_channels[] = {
    { "tckc", "TCKC", "Test clock", 0, SRD_CHANNEL_SCLK, "dec_cjtag_chan_tckc" },
    { "tmsc", "TMSC", "Test mode select", 1, SRD_CHANNEL_SDATA, "dec_cjtag_chan_tmsc" },
};

/* Annotation labels */
static const char* cjtag_ann_labels[][3] = {
    /* JTAG states: 0-15 */
    { "", "test-logic-reset", "TEST-LOGIC-RESET" },
    { "", "run-test/idle", "RUN-TEST/IDLE" },
    { "", "select-dr-scan", "SELECT-DR-SCAN" },
    { "", "capture-dr", "CAPTURE-DR" },
    { "", "update-dr", "UPDATE-DR" },
    { "", "pause-dr", "PAUSE-DR" },
    { "", "shift-dr", "SHIFT-DR" },
    { "", "exit1-dr", "EXIT1-DR" },
    { "", "exit2-dr", "EXIT2-DR" },
    { "", "select-ir-scan", "SELECT-IR-SCAN" },
    { "", "capture-ir", "CAPTURE-IR" },
    { "", "update-ir", "UPDATE-IR" },
    { "", "pause-ir", "PAUSE-IR" },
    { "", "shift-ir", "SHIFT-IR" },
    { "", "exit1-ir", "EXIT1-IR" },
    { "", "exit2-ir", "EXIT2-IR" },
    /* cJTAG states: 16-27 */
    { "", "cjtag-ec", "CJTAG_EC" },
    { "", "cjtag-spare", "CJTAG_SPARE" },
    { "", "cjtag-tpdel", "CJTAG_TPDEL" },
    { "", "cjtag-tprev", "CJTAG_TPREV" },
    { "", "cjtag-tpst", "CJTAG_TPST" },
    { "", "cjtag-rdyc", "CJTAG_RDYC" },
    { "", "cjtag-dlyc", "CJTAG_DLYC" },
    { "", "cjtag-scnfmt", "CJTAG_SCNFMT" },
    { "", "cjtag-cp", "CJTAG_CP" },
    { "", "cjtag-oac", "CJTAG_OAC" },
    { "", "oscan1", "OSCAN1" },
    { "", "four-wire", "FOUR_WIRE" },
    /* Bit annotations: 28-32 */
    { "", "bit-tdi", "Bit (TDI)" },
    { "", "bit-tdo", "Bit (TDO)" },
    { "", "bitstring-tdi", "Bitstring (TDI)" },
    { "", "bitstring-tdo", "Bitstring (TDO)" },
    { "", "bit-tms", "Bit (TMS)" },
};

/* Annotation row class arrays */
static const int row_bits_tdi_classes[] = { ANN_BIT_TDI, -1 };
static const int row_bits_tdo_classes[] = { ANN_BIT_TDO, -1 };
static const int row_bitstrings_tdi_classes[] = { ANN_BITSTRING_TDI, -1 };
static const int row_bitstrings_tdo_classes[] = { ANN_BITSTRING_TDO, -1 };
static const int row_bits_tms_classes[] = { ANN_BIT_TMS, -1 };

/* cJTAG states row: indices 16..27 */
static const int row_cjtag_states_classes[] = {
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, -1
};

/* JTAG states row: indices 0..15 */
static const int row_jtag_states_classes[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -1
};

static const struct srd_c_ann_row cjtag_ann_rows[] = {
    { "bits-tdi", "Bits (TDI)", row_bits_tdi_classes, 1 },
    { "bits-tdo", "Bits (TDO)", row_bits_tdo_classes, 1 },
    { "bitstrings-tdi", "Bitstrings (TDI)", row_bitstrings_tdi_classes, 1 },
    { "bitstrings-tdo", "Bitstrings (TDO)", row_bitstrings_tdo_classes, 1 },
    { "bits-tms", "Bits (TMS)", row_bits_tms_classes, 1 },
    { "cjtag-states", "CJTAG states", row_cjtag_states_classes, 12 },
    { "jtag-states", "JTAG states", row_jtag_states_classes, 16 },
};

static const char* cjtag_inputs[] = { "logic", NULL };
static const char* cjtag_outputs[] = { "jtag", NULL };
static const char* cjtag_tags[] = { "Debug/trace", NULL };

/* JTAG state name strings for Python output */


static void cjtag_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(cjtag_priv)));
    }
    cjtag_priv *s = (cjtag_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(cjtag_priv));
    s->state = ST_RUN_TEST_IDLE;
    s->oldstate = ST_RUN_TEST_IDLE;
    s->cjtagstate = CST_FOUR_WIRE;
    s->oldcjtagstate = CST_FOUR_WIRE;
    s->first = 1;
    s->first_bit = 1;
}

static void cjtag_start(struct srd_decoder_inst *di)
{
    cjtag_priv *s = (cjtag_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "cjtag");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "jtag");
}

static void advance_state_machine(cjtag_priv *s, int tms)
{
    s->oldstate = s->state;

    /* cJTAG state handling (OAC processing) */
    if (s->cjtagstate >= 0 && s->cjtagstate != CST_OSCAN1 &&
        s->cjtagstate != CST_FOUR_WIRE) {
        /* In a CJTAG_* state */
        s->oacp++;

        if (s->oacp > 4 && s->oaclen == 12)
            s->cjtagstate = CST_CJTAG_EC;

        if (s->oacp == 8 && tms == 0)
            s->oaclen = 36;
        if (s->oacp > 8 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_SPARE;
        if (s->oacp > 13 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_TPDEL;
        if (s->oacp > 16 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_TPREV;
        if (s->oacp > 18 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_TPST;
        if (s->oacp > 23 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_RDYC;
        if (s->oacp > 25 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_DLYC;
        if (s->oacp > 27 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_SCNFMT;

        if (s->oacp > 8 && s->oaclen == 12)
            s->cjtagstate = CST_CJTAG_CP;
        if (s->oacp > 32 && s->oaclen == 36)
            s->cjtagstate = CST_CJTAG_CP;

        if (s->oacp > s->oaclen) {
            s->cjtagstate = CST_OSCAN1;
            s->oscan1cycle = 1;
            /* Nuclei cJTAG device asserts reset during online activation */
            s->state = ST_TEST_LOGIC_RESET;
        }
        return;
    }

    /* Normal JTAG state transition */
    s->state = jtag_next_state[s->state][tms ? 1 : 0];
}

static void handle_rising_tckc_edge(struct srd_decoder_inst *di,
    cjtag_priv *s, uint64_t samplenum, int tdi, int tdo, int tms)
{
    /* Rising TCK edges always advance the state machine */
    advance_state_machine(s, tms);

    if (s->first) {
        /* Save the start sample for later (no output yet) */
        s->ss_item = samplenum;
        s->first = 0;
    } else {
        /* Output the saved item (from the last CLK edge to the current) */
        s->es_item = samplenum;
        /* Output the old JTAG state annotation */
        c_put(di, s->ss_item, s->es_item, s->out_ann,
            s->oldstate, cjtag_ann_labels[s->oldstate][2]);
        /* Output NEW STATE Python message */
        c_proto(di, s->ss_item, s->es_item, s->out_python, "NEW STATE", C_END);

        /* Output the old cJTAG state annotation */
        c_put(di, s->ss_item, s->es_item, s->out_ann,
            NUM_JTAG_STATES + s->oldcjtagstate,
            cjtag_ann_labels[NUM_JTAG_STATES + s->oldcjtagstate][2]);

        /* If in a CJTAG_* state, output TMS bit annotation */
        if (s->oldcjtagstate != CST_OSCAN1 && s->oldcjtagstate != CST_FOUR_WIRE) {
            char tms_str[4];
            snprintf(tms_str, sizeof(tms_str), "%d", s->oldtms);
            c_put(di, s->ss_item, s->es_item, s->out_ann, ANN_BIT_TMS, tms_str);
        }
    }
    s->oldtms = tms;

    /* Upon SHIFT-DR/IR or EXIT1-DR/IR, collect the current TDI/TDO values */
    if (s->oldstate == ST_SHIFT_DR || s->oldstate == ST_SHIFT_IR ||
        s->oldstate == ST_EXIT1_DR || s->oldstate == ST_EXIT1_IR) {
        if (s->first_bit) {
            s->ss_bitstring = samplenum;
            s->first_bit = 0;
        } else {
            /* Output individual TDI/TDO bit annotations for previous bit */
            if (s->bits_cnt > 0) {
                const char *tdi_str = (s->bits_tdi[0] < 0) ? "None" :
                    (s->bits_tdi[0] ? "1" : "0");
                const char *tdo_str = (s->bits_tdo[0] < 0) ? "None" :
                    (s->bits_tdo[0] ? "1" : "0");
                c_put(di, s->ss_item, samplenum, s->out_ann, ANN_BIT_TDI, tdi_str);
                c_put(di, s->ss_item, samplenum, s->out_ann, ANN_BIT_TDO, tdo_str);
            }
            /* Use current samplenum as ES of the previous bit */
            if (s->bits_cnt > 0) {
                s->bits_es_tdi[0] = samplenum;
                s->bits_es_tdo[0] = samplenum;
            }
        }

        /* Insert new bit at position 0 (MSB first, shift right) */
        if (s->bits_cnt < 255) {
            memmove(&s->bits_tdi[1], &s->bits_tdi[0], s->bits_cnt * sizeof(int));
            memmove(&s->bits_tdo[1], &s->bits_tdo[0], s->bits_cnt * sizeof(int));
            memmove(&s->bits_ss_tdi[1], &s->bits_ss_tdi[0], s->bits_cnt * sizeof(uint64_t));
            memmove(&s->bits_es_tdi[1], &s->bits_es_tdi[0], s->bits_cnt * sizeof(uint64_t));
            memmove(&s->bits_ss_tdo[1], &s->bits_ss_tdo[0], s->bits_cnt * sizeof(uint64_t));
            memmove(&s->bits_es_tdo[1], &s->bits_es_tdo[0], s->bits_cnt * sizeof(uint64_t));
        }
        s->bits_tdi[0] = tdi;
        s->bits_tdo[0] = tdo;
        s->bits_ss_tdi[0] = samplenum;
        s->bits_es_tdi[0] = (uint64_t)-1; /* will be filled later */
        s->bits_ss_tdo[0] = samplenum;
        s->bits_es_tdo[0] = (uint64_t)-1;
        s->bits_cnt++;
    }

    /* Output all TDI/TDO bits if we just switched to UPDATE-* */
    if (s->state == ST_UPDATE_DR || s->state == ST_UPDATE_IR) {
        s->es_bitstring = samplenum;

        if (s->bits_cnt > 1) {
            int is_ir = (s->state == ST_UPDATE_IR);
            const char *dr_ir = is_ir ? "IR" : "DR";

            /* Check if any bit is -1 (None/unknown) — skip bitstring if so */
            int has_invalid = 0;
            for (int i = 1; i < s->bits_cnt; i++) {
                if (s->bits_tdi[i] < 0 || s->bits_tdo[i] < 0) {
                    has_invalid = 1;
                    break;
                }
            }

            if (!has_invalid) {
            /* Build TDI bitstring */
            {
                char bitstr[257];
                int cnt = s->bits_cnt - 1; /* skip index 0 which is current bit */
                int i;
                uint64_t val = 0;
                for (i = 0; i < cnt && i < 256; i++) {
                    bitstr[i] = '0' + s->bits_tdi[i + 1];
                    val |= ((uint64_t)s->bits_tdi[i + 1] << (cnt - 1 - i));
                }
                bitstr[cnt < 256 ? cnt : 256] = '\0';

                char ann_text[512];
                snprintf(ann_text, sizeof(ann_text), "%s TDI: %s (0x%llx), %d bits",
                    dr_ir, bitstr, (unsigned long long)val, cnt);
                c_put(di, s->ss_bitstring, s->es_bitstring, s->out_ann,
                    ANN_BITSTRING_TDI, ann_text);

                /* Python output: "IR TDI" / "DR TDI" with bit bytes */
                int byte_count = (cnt + 7) / 8;
                unsigned char bit_bytes[32];
                memset(bit_bytes, 0, sizeof(bit_bytes));
                for (i = 0; i < cnt && i < 256; i++) {
                    if (s->bits_tdi[i + 1])
                        bit_bytes[i / 8] |= (1 << (i % 8));
                }
                c_proto(di, s->ss_bitstring, s->es_bitstring,
                    s->out_python, is_ir ? "IR TDI" : "DR TDI",
                    C_BYTES(bit_bytes, byte_count), C_END);
            }

            /* Build TDO bitstring */
            {
                char bitstr[257];
                int cnt = s->bits_cnt - 1;
                int i;
                uint64_t val = 0;
                for (i = 0; i < cnt && i < 256; i++) {
                    bitstr[i] = '0' + s->bits_tdo[i + 1];
                    val |= ((uint64_t)s->bits_tdo[i + 1] << (cnt - 1 - i));
                }
                bitstr[cnt < 256 ? cnt : 256] = '\0';

                char ann_text[512];
                snprintf(ann_text, sizeof(ann_text), "%s TDO: %s (0x%llx), %d bits",
                    dr_ir, bitstr, (unsigned long long)val, cnt);
                c_put(di, s->ss_bitstring, s->es_bitstring, s->out_ann,
                    ANN_BITSTRING_TDO, ann_text);

                /* Python output: "IR TDO" / "DR TDO" with bit bytes */
                int byte_count = (cnt + 7) / 8;
                unsigned char bit_bytes[32];
                memset(bit_bytes, 0, sizeof(bit_bytes));
                for (i = 0; i < cnt && i < 256; i++) {
                    if (s->bits_tdo[i + 1])
                        bit_bytes[i / 8] |= (1 << (i % 8));
                }
                c_proto(di, s->ss_bitstring, s->es_bitstring,
                    s->out_python, is_ir ? "IR TDO" : "DR TDO",
                    C_BYTES(bit_bytes, byte_count), C_END);
            } /* end TDO scope */
            } /* end if (!has_invalid) */
        }

        s->bits_cnt = 0;
        s->first_bit = 1;
        s->ss_bitstring = samplenum;
    }

    s->ss_item = samplenum;
}

static void handle_tapc_state(cjtag_priv *s)
{
    s->oldcjtagstate = s->cjtagstate;

    if (s->escape_edges >= 8)
        s->cjtagstate = CST_FOUR_WIRE;
    if (s->escape_edges == 6) {
        s->cjtagstate = CST_CJTAG_OAC;
        s->oacp = 0;
        s->oaclen = 12;
    }

    s->escape_edges = 0;
}

static void cjtag_decode(struct srd_decoder_inst *di)
{
    cjtag_priv *s = (cjtag_priv *)c_decoder_get_private(di);
    while (1) {
        /* Wait for a rising edge on TCKC */
        int ret = c_wait(di, CW_R(CH_TCKC), CW_END);
        if (ret != SRD_OK)
            return;

        int tckc = c_pin(di, CH_TCKC);
        int tmsc = c_pin(di, CH_TMSC);

        /* Check cJTAG state on each rising edge */
        handle_tapc_state(s);

        if (s->cjtagstate == CST_OSCAN1) {
            /* OSCAN1 mode: 3-cycle demultiplexing */
            int tdi = 0, tdo = 0, tms = 0;
            if (s->oscan1cycle == 0) {
                /* nTDI: active low */
                tdi = (tmsc == 0) ? 1 : 0;
                s->oscan1cycle = 1;
            } else if (s->oscan1cycle == 1) {
                /* TMS */
                tms = tmsc;
                s->oscan1cycle = 2;
            } else {
                /* TDO */
                tdo = tmsc;
                handle_rising_tckc_edge(di, s, di_samplenum(di), tdi, tdo, tms);
                s->oscan1cycle = 0;
            }
        } else {
            /* Non-OSCAN1 mode: TMSC is TMS (or TDI/TDO in 4-wire) */
            handle_rising_tckc_edge(di, s, di_samplenum(di), -1, -1, tmsc);
        }

        /* Monitor TMSC edges while TCKC is high */
        while (tckc == 1) {
            ret = c_wait(di, CW_F(CH_TCKC), CW_OR, CW_E(CH_TMSC), CW_END);
            if (ret != SRD_OK)
                return;

            int new_tmsc = c_pin(di, CH_TMSC);
            if (new_tmsc != tmsc) {
                tmsc = new_tmsc;
                s->escape_edges++;
            }

            tckc = c_pin(di, CH_TCKC);
        }
    }
}

static void cjtag_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder cjtag_c_decoder = {
    .id = "cjtag_c",
    .name = "cJTAG(C)",
    .longname = "Compact Joint Test Action Group (IEEE 1149.7)",
    .desc = "Protocol for testing, debugging, and flashing ICs.",
    .license = "gplv2+",
    .channels = cjtag_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = cjtag_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = cjtag_ann_rows,
    .inputs = cjtag_inputs,
    .num_inputs = 1,
    .outputs = cjtag_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = cjtag_tags,
    .num_tags = 1,
    .reset = cjtag_reset,
    .start = cjtag_start,
    .decode = cjtag_decode,
    .destroy = cjtag_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &cjtag_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}