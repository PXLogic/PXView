/*
 * cjtag_oscan0_c.c — cJTAG OScan1 protocol decoder (C implementation)
 *
 * Compact JTAG (IEEE 1149.7) OScan1 format decoder.
 * Supports 4-wire JTAG mode and cJTAG OScan1 mode with
 * escape sequence detection and OAC activation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* JTAG TAP states */
enum jtag_state {
    TEST_LOGIC_RESET = 0,
    RUN_TEST_IDLE = 1,
    SELECT_DR_SCAN = 2,
    CAPTURE_DR = 3,
    UPDATE_DR = 4,
    PAUSE_DR = 5,
    SHIFT_DR = 6,
    EXIT1_DR = 7,
    EXIT2_DR = 8,
    SELECT_IR_SCAN = 9,
    CAPTURE_IR = 10,
    UPDATE_IR = 11,
    PAUSE_IR = 12,
    SHIFT_IR = 13,
    EXIT1_IR = 14,
    EXIT2_IR = 15,
};

/* cJTAG states */
enum cjtag_state {
    CJTAG_4WIRE = 0,
    CJTAG_OAC,
    CJTAG_EC,
    CJTAG_SPARE,
    CJTAG_TPDEL,
    CJTAG_TPREV,
    CJTAG_TPST,
    CJTAG_RDYC,
    CJTAG_DLYC,
    CJTAG_SCNFMT,
    CJTAG_CP,
    CJTAG_OSCAN1,
};

enum cjtag_ann {
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
    ANN_BIT_TDI,
    ANN_BIT_TDO,
    ANN_BITSTRING_TDI,
    ANN_BITSTRING_TDO,
    ANN_BIT_TMS,
    ANN_STATE_TAPC,
    NUM_ANN,
};

#define TDI 0
#define TDO 1
#define TCK 2
#define TMS 3
#define TRST 4
#define SRST 5
#define RTCK 6

static const char *jtag_state_names[] = {
    "TEST-LOGIC-RESET", "RUN-TEST/IDLE",
    "SELECT-DR-SCAN", "CAPTURE-DR", "UPDATE-DR", "PAUSE-DR",
    "SHIFT-DR", "EXIT1-DR", "EXIT2-DR",
    "SELECT-IR-SCAN", "CAPTURE-IR", "UPDATE-IR", "PAUSE-IR",
    "SHIFT-IR", "EXIT1-IR", "EXIT2-IR",
};

static const char *cjtag_state_names[] = {
    "4-WIRE", "CJTAG-OAC", "CJTAG-EC", "CJTAG-SPARE",
    "CJTAG-TPDEL", "CJTAG-TPREV", "CJTAG-TPST", "CJTAG-RDYC",
    "CJTAG-DLYC", "CJTAG-SCNFMT", "CJTAG-CP", "OSCAN1",
};

/* JTAG state transition table: next_state[current][tms] */
static const int next_state[16][2] = {
    { 1, 0 },   /* TEST-LOGIC-RESET */
    { 1, 2 },   /* RUN-TEST/IDLE */
    { 3, 9 },   /* SELECT-DR-SCAN */
    { 6, 7 },   /* CAPTURE-DR */
    { 1, 2 },   /* UPDATE-DR */
    { 5, 8 },   /* PAUSE-DR */
    { 6, 7 },   /* SHIFT-DR */
    { 5, 4 },   /* EXIT1-DR */
    { 6, 4 },   /* EXIT2-DR */
    { 10, 0 },  /* SELECT-IR-SCAN */
    { 13, 14 }, /* CAPTURE-IR */
    { 1, 2 },   /* UPDATE-IR */
    { 12, 15 }, /* PAUSE-IR */
    { 13, 14 }, /* SHIFT-IR */
    { 12, 11 }, /* EXIT1-IR */
    { 13, 11 }, /* EXIT2-IR */
};

typedef struct {
    int jtag_state;
    int old_jtag_state;
    int cjtag_state;
    int old_cjtag_state;
    int escape_edges;
    int oaclen;
    int oacp;
    int oscan1cycle;
    int oldtms;

    /* TDI/TDO bit collection */
    int bits_tdi[256];
    int bits_tdo[256];
    int bits_cnt;
    int data_ready;

    /* Sample positions */
    uint64_t ss_item;
    uint64_t es_item;
    uint64_t ss_bitstring;
    uint64_t es_bitstring;
    int first;
    int out_ann;
    int out_python;
} cjtag_priv;

static struct srd_channel cjtag_channels[] = {
    { "tdi", "TDI", "Test data input", 0, SRD_CHANNEL_SDATA, "dec_cjtag_oscan0_chan_tdi" },
    { "tdo", "TDO", "Test data output", 1, SRD_CHANNEL_SDATA, "dec_cjtag_oscan0_chan_tdo" },
    { "tck", "TCK", "Test clock", 2, SRD_CHANNEL_SCLK, "dec_cjtag_oscan0_chan_tck" },
    { "tms", "TMS", "Test mode select", 3, SRD_CHANNEL_COMMON, "dec_cjtag_oscan0_chan_tms" },
};

static struct srd_channel cjtag_optional_channels[] = {
    { "trst", "TRST#", "Test reset", 4, SRD_CHANNEL_COMMON, "dec_cjtag_oscan0_opt_chan_trst" },
    { "srst", "SRST#", "System reset", 5, SRD_CHANNEL_COMMON, "dec_cjtag_oscan0_opt_chan_srst" },
    { "rtck", "RTCK", "Return clock signal", 6, SRD_CHANNEL_SCLK, "dec_cjtag_oscan0_opt_chan_rtck" },
};

static const char *cjtag_ann_labels[][3] = {
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
    { "", "bit-tdi", "Bit (TDI)" },
    { "", "bit-tdo", "Bit (TDO)" },
    { "", "bitstring-tdi", "Bitstring (TDI)" },
    { "", "bitstring-tdo", "Bitstring (TDO)" },
    { "", "bit-tms", "Bit (TMS)" },
    { "", "state-tapc", "TAPC State" },
};

static const int cjtag_row_bits_tdi_classes[] = { ANN_BIT_TDI, -1 };
static const int cjtag_row_bits_tdo_classes[] = { ANN_BIT_TDO, -1 };
static const int cjtag_row_bitstrings_tdi_classes[] = { ANN_BITSTRING_TDI, -1 };
static const int cjtag_row_bitstrings_tdo_classes[] = { ANN_BITSTRING_TDO, -1 };
static const int cjtag_row_bit_tms_classes[] = { ANN_BIT_TMS, -1 };
static const int cjtag_row_state_tapc_classes[] = { ANN_STATE_TAPC, -1 };
static const int cjtag_row_states_classes[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, -1
};

static const struct srd_c_ann_row cjtag_ann_rows[] = {
    { "bits-tdi", "Bits (TDI)", cjtag_row_bits_tdi_classes, 1 },
    { "bits-tdo", "Bits (TDO)", cjtag_row_bits_tdo_classes, 1 },
    { "bitstrings-tdi", "Bitstring (TDI)", cjtag_row_bitstrings_tdi_classes, 1 },
    { "bitstrings-tdo", "Bitstring (TDO)", cjtag_row_bitstrings_tdo_classes, 1 },
    { "bit-tms", "Bit (TMS)", cjtag_row_bit_tms_classes, 1 },
    { "state-tapc", "TAPC State", cjtag_row_state_tapc_classes, 1 },
    { "states", "States", cjtag_row_states_classes, 16 },
};

static const char *cjtag_inputs[] = { "logic" };
static const char *cjtag_outputs[] = { "jtag" };
static const char *cjtag_tags[] = { "Debug/trace" };

static void cjtag_advance_state_machine(cjtag_priv *p, int tms)
{
    p->old_jtag_state = p->jtag_state;

    if (p->cjtag_state >= CJTAG_OAC && p->cjtag_state < CJTAG_OSCAN1) {
        /* cJTAG activation sequence */
        p->oacp++;
        if (p->oacp > 4 && p->oaclen == 12)
            p->cjtag_state = CJTAG_EC;

        if (p->oacp == 8 && tms == 0)
            p->oaclen = 36;
        if (p->oacp > 8 && p->oaclen == 36)
            p->cjtag_state = CJTAG_SPARE;
        if (p->oacp > 13 && p->oaclen == 36)
            p->cjtag_state = CJTAG_TPDEL;
        if (p->oacp > 16 && p->oaclen == 36)
            p->cjtag_state = CJTAG_TPREV;
        if (p->oacp > 18 && p->oaclen == 36)
            p->cjtag_state = CJTAG_TPST;
        if (p->oacp > 23 && p->oaclen == 36)
            p->cjtag_state = CJTAG_RDYC;
        if (p->oacp > 25 && p->oaclen == 36)
            p->cjtag_state = CJTAG_DLYC;
        if (p->oacp > 27 && p->oaclen == 36)
            p->cjtag_state = CJTAG_SCNFMT;

        if (p->oacp > 8 && p->oaclen == 12)
            p->cjtag_state = CJTAG_CP;
        if (p->oacp > 32 && p->oaclen == 36)
            p->cjtag_state = CJTAG_CP;

        if (p->oacp > p->oaclen) {
            p->cjtag_state = CJTAG_OSCAN1;
            p->oscan1cycle = 1;
            p->jtag_state = TEST_LOGIC_RESET;
        }
    } else {
        /* Standard JTAG state machine */
        p->jtag_state = next_state[p->jtag_state][tms];
    }
}

static void cjtag_handle_rising_tck_edge(struct srd_decoder_inst *di, cjtag_priv *p,
                                          int tdi, int tdo, int tck, int tms,
                                          uint64_t samplenum)
{
    (void)tck;
    (void)samplenum;
    p->old_cjtag_state = p->cjtag_state;

    cjtag_advance_state_machine(p, tms);

    if (p->first) {
        p->ss_item = samplenum;
        p->first = 0;
    } else {
        p->es_item = samplenum;
        /* Output the old JTAG state */
        if (p->old_jtag_state >= 0 && p->old_jtag_state < 16) {
            c_put(di, p->ss_item, p->es_item, p->out_ann,
                      p->old_jtag_state, jtag_state_names[p->old_jtag_state]);
        }
        /* Output cJTAG state */
        if (p->old_cjtag_state >= 0 && p->old_cjtag_state <= CJTAG_OSCAN1) {
            c_put(di, p->ss_item, p->es_item, p->out_ann,
                      ANN_STATE_TAPC, cjtag_state_names[p->old_cjtag_state]);
        }
        /* Output TMS bit for cJTAG states */
        if (p->old_cjtag_state >= CJTAG_OAC && p->old_cjtag_state <= CJTAG_CP) {
            char tms_str[4];
            snprintf(tms_str, sizeof(tms_str), "%d", p->oldtms);
            c_put(di, p->ss_item, p->es_item, p->out_ann, ANN_BIT_TMS, tms_str);
        }
    }
    p->oldtms = tms;

    /* Collect TDI/TDO in SHIFT states */
    if (p->jtag_state == SHIFT_DR || p->jtag_state == SHIFT_IR) {
        if (p->bits_cnt > 0) {
            if (p->bits_cnt == 1)
                p->ss_bitstring = samplenum;

            if (p->bits_cnt > 1) {
                char tdi_str[4], tdo_str[4];
                snprintf(tdi_str, sizeof(tdi_str), "%d", p->bits_tdi[0]);
                snprintf(tdo_str, sizeof(tdo_str), "%d", p->bits_tdo[0]);
                c_put(di, p->ss_item, samplenum, p->out_ann, ANN_BIT_TDI, tdi_str);
                c_put(di, p->ss_item, samplenum, p->out_ann, ANN_BIT_TDO, tdo_str);
            }

            /* Shift bits: insert at position 0 */
            if (p->bits_cnt < 255) {
                memmove(&p->bits_tdi[1], &p->bits_tdi[0], p->bits_cnt * sizeof(int));
                memmove(&p->bits_tdo[1], &p->bits_tdo[0], p->bits_cnt * sizeof(int));
            }

            p->bits_tdi[0] = tdi;
            p->bits_tdo[0] = tdo;
        }

        p->bits_cnt++;
    }

    /* Output bitstring when transitioning from SHIFT to EXIT1 */
    if ((p->old_jtag_state == SHIFT_DR && p->jtag_state == EXIT1_DR) ||
        (p->old_jtag_state == SHIFT_IR && p->jtag_state == EXIT1_IR)) {
        if (p->bits_cnt > 0) {
            if (p->bits_cnt == 1) {
                /* Only shifted one bit */
                p->ss_bitstring = samplenum;
                p->bits_tdi[0] = tdi;
                p->bits_tdo[0] = tdo;
            } else {
                /* Output the previous bit */
                char tdi_str[4], tdo_str[4];
                snprintf(tdi_str, sizeof(tdi_str), "%d", p->bits_tdi[0]);
                snprintf(tdo_str, sizeof(tdo_str), "%d", p->bits_tdo[0]);
                c_put(di, p->ss_item, samplenum, p->out_ann, ANN_BIT_TDI, tdi_str);
                c_put(di, p->ss_item, samplenum, p->out_ann, ANN_BIT_TDO, tdo_str);

                /* Add the last bit: insert at position 0 */
                if (p->bits_cnt < 255) {
                    memmove(&p->bits_tdi[1], &p->bits_tdi[0], p->bits_cnt * sizeof(int));
                    memmove(&p->bits_tdo[1], &p->bits_tdo[0], p->bits_cnt * sizeof(int));
                }
                p->bits_tdi[0] = tdi;
                p->bits_tdo[0] = tdo;
            }

            p->bits_cnt++;
            p->data_ready = 1;
        }
    }

    /* Output bitstring when transitioning from EXIT to PAUSE */
    if (p->old_jtag_state == EXIT1_DR || p->old_jtag_state == EXIT1_IR ||
        p->old_jtag_state == EXIT2_DR || p->old_jtag_state == EXIT2_IR) {
        if (p->data_ready && p->bits_cnt > 1) {
            p->data_ready = 0;
            p->es_bitstring = samplenum;

            /* bits_cnt includes the phantom first SHIFT clock where no data was stored,
               so actual data count is bits_cnt - 1 (matching Python's len(bits_tdi)) */
            int cnt = p->bits_cnt - 1;
            if (cnt > 256) cnt = 256;
            const char *dr_ir = (p->old_jtag_state == EXIT1_IR || p->old_jtag_state == EXIT2_IR) ? "IR" : "DR";

            /* Build bitstring */
            uint64_t tdi_val = 0, tdo_val = 0;
            for (int i = 0; i < cnt; i++) {
                tdi_val |= ((uint64_t)p->bits_tdi[i] << (cnt - 1 - i));
                tdo_val |= ((uint64_t)p->bits_tdo[i] << (cnt - 1 - i));
            }

            char tdi_str[128], tdo_str[128];
            snprintf(tdi_str, sizeof(tdi_str), "%s TDI:  (0x%llX), %d bits",
                dr_ir, (unsigned long long)tdi_val, cnt);
            snprintf(tdo_str, sizeof(tdo_str), "%s TDO:  (0x%llX), %d bits",
                     dr_ir, (unsigned long long)tdo_val, cnt);

            /* Output in interleaved order: TDI bitstring, TDI last bit, TDO bitstring, TDO last bit */
            c_put(di, p->ss_bitstring, samplenum, p->out_ann,
                      ANN_BITSTRING_TDI, tdi_str);

            char tdi_last[4];
            snprintf(tdi_last, sizeof(tdi_last), "%d", p->bits_tdi[0]);
            c_put(di, p->ss_item, samplenum, p->out_ann, ANN_BIT_TDI, tdi_last);

            c_put(di, p->ss_bitstring, samplenum, p->out_ann,
                      ANN_BITSTRING_TDO, tdo_str);

            char tdo_last[4];
            snprintf(tdo_last, sizeof(tdo_last), "%d", p->bits_tdo[0]);
            c_put(di, p->ss_item, samplenum, p->out_ann, ANN_BIT_TDO, tdo_last);

            /* Output Python protocol data */
            {
                int is_ir = (p->old_jtag_state == EXIT1_IR || p->old_jtag_state == EXIT2_IR);
                int byte_count = (cnt + 7) / 8;
                unsigned char tdi_bytes[32], tdo_bytes[32];
                memset(tdi_bytes, 0, sizeof(tdi_bytes));
                memset(tdo_bytes, 0, sizeof(tdo_bytes));
                for (int i = 0; i < cnt; i++) {
                    if (p->bits_tdi[i])
                        tdi_bytes[i / 8] |= (1 << (i % 8));
                    if (p->bits_tdo[i])
                        tdo_bytes[i / 8] |= (1 << (i % 8));
                }
                c_proto(di, p->ss_bitstring, samplenum, p->out_python,
                                     is_ir ? "IR TDI" : "DR TDI", C_BYTES(tdi_bytes, byte_count), C_END);
                c_proto(di, p->ss_bitstring, samplenum, p->out_python,
                                     is_ir ? "IR TDO" : "DR TDO", C_BYTES(tdo_bytes, byte_count), C_END);
            }

            p->bits_cnt = 0;
        }
    }

    p->ss_item = samplenum;
}

static void cjtag_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(cjtag_priv)));
    cjtag_priv *p = (cjtag_priv *)c_decoder_get_private(di);
    memset(p, 0, sizeof(cjtag_priv));
    p->jtag_state = TEST_LOGIC_RESET;
    p->old_jtag_state = TEST_LOGIC_RESET;
    p->cjtag_state = CJTAG_4WIRE;
    p->old_cjtag_state = CJTAG_4WIRE;
    p->out_ann = -1;
    p->out_python = -1;
    p->first = 1;
}

static void cjtag_start(struct srd_decoder_inst *di)
{
    cjtag_priv *p = (cjtag_priv *)c_decoder_get_private(di);
    p->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "jtag");
    p->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "jtag");
}

static void cjtag_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    (void)di;
    (void)key;
    (void)value;
}

static void cjtag_decode(struct srd_decoder_inst *di)
{
    cjtag_priv *p = (cjtag_priv *)c_decoder_get_private(di);
    while (1) {
        /* Wait for rising TCK edge */
        int ret = c_wait(di, CW_R(TCK), CW_END);
        if (ret != SRD_OK)
            return;

        int tdi = c_pin(di, TDI);
        int tdo = c_pin(di, TDO);
        int tck = c_pin(di, TCK);
        int tms = c_pin(di, TMS);

        /* Handle TAPC state (escape detection) */
        p->old_cjtag_state = p->cjtag_state;

        if (p->escape_edges >= 8) {
            p->cjtag_state = CJTAG_4WIRE;
        }
        if (p->escape_edges == 6) {
            p->cjtag_state = CJTAG_OAC;
            p->oacp = 0;
            p->oaclen = 12;
        }
        p->escape_edges = 0;

        /* Handle OScan1 3-cycle protocol or 4-wire mode */
        int tdi_real = tdi, tdo_real = tdo, tms_real = tms;

        if (p->cjtag_state == CJTAG_OSCAN1) {
            if (p->oscan1cycle == 0) {
                /* nTDI cycle: TMS=0 -> TDI=1, TMS=1 -> TDI=0 */
                tdi_real = (tms == 0) ? 1 : 0;
                p->oscan1cycle = 1;
            } else if (p->oscan1cycle == 1) {
                /* TMS cycle */
                tms_real = tms;
                p->oscan1cycle = 2;
            } else {
                /* TDO cycle */
                tdo_real = tms;
                cjtag_handle_rising_tck_edge(di, p, tdi_real, tdo_real, tck, tms_real, di_samplenum(di));
                p->oscan1cycle = 0;
                goto after_tck_rising;
            }
        } else {
            cjtag_handle_rising_tck_edge(di, p, tdi, tdo, tck, tms, di_samplenum(di));
        }

after_tck_rising:
        /* Wait for TCK falling edge or TMS change while TCK is high.
         * Match Python's while(tck == 1) loop that monitors TMS changes. */
        {
            int tck_now = c_pin(di, TCK);
            while (tck_now == 1) {
                ret = c_wait(di, CW_F(TCK), CW_OR, CW_E(TMS), CW_END);
                if (ret != SRD_OK)
                    return;

                int tms_new = c_pin(di, TMS);
                if (tms_new != tms) {
                    tms = tms_new;
                    p->escape_edges++;
                }
                tck_now = c_pin(di, TCK);
            }
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

struct srd_c_decoder cjtag_oscan0_c_decoder = {
    .id = "cjtag_oscan0_c",
    .name = "cJTAG OScan1(C)",
    .longname = "Compact Joint Test Action Group (IEEE 1149.7) (C)",
    .desc = "Protocol for testing, debugging, and flashing ICs, OScan1 format (C implementation)",
    .license = "gplv2+",
    .channels = cjtag_channels,
    .num_channels = 4,
    .optional_channels = cjtag_optional_channels,
    .num_optional_channels = 3,
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
    .metadata = cjtag_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &cjtag_oscan0_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}