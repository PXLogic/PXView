#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum lpc_state {
    LPC_IDLE,
    LPC_GET_START,
    LPC_GET_CT_DR,
    LPC_GET_ADDR,
    LPC_GET_FW_IDSEL,
    LPC_GET_FW_ADDR,
    LPC_GET_FW_MSIZE,
    LPC_GET_TAR,
    LPC_GET_SYNC,
    LPC_GET_TIMEOUT,
    LPC_GET_FW_DATA,
    LPC_GET_DATA,
    LPC_GET_TAR2,
};

typedef struct {
    enum lpc_state state;
    int oldlframe;
    int oldlad;
    uint64_t addr;
    int direction;
    int cur_nibble;
    int cycle_type;
    uint8_t databyte;
    int tarcount;
    int synccount;
    int timeoutcount;
    int start_field;
    int cycle_count;
    uint64_t dataword;
    int msize;
    uint64_t ss_block;
    uint64_t es_block;
    int out_ann;
} lpc_decoder_state;

#define CH_LFRAME 0
#define CH_LCLK 1
#define CH_LAD0 2
#define CH_LAD1 3
#define CH_LAD2 4
#define CH_LAD3 5
#define CH_LRESET 6
#define CH_LDRQ 7
#define CH_SERIRQ 8
#define CH_CLKRUN 9
#define CH_LPME 10
#define CH_LPCPD 11
#define CH_LSMI 12

#define ANN_WARN 0
#define ANN_START 1
#define ANN_CYCLE_TYPE 2
#define ANN_ADDR 3
#define ANN_TAR1 4
#define ANN_SYNC 5
#define ANN_TIMEOUT 6
#define ANN_DATA 7
#define ANN_TAR2 8

static const char* lpc_start_names[] = {
    "Start of cycle for a target",
    "Reserved",
    "Grant for bus master 0",
    "Grant for bus master 1",
    "Reserved",
    "TPM",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Start of FW Memory Read",
    "Start of FW Memory Write",
    "Stop/abort",
};

static const char* lpc_ct_dr_names[] = {
    "I/O read",
    "I/O read",
    "I/O write",
    "I/O write",
    "Memory read",
    "Memory read",
    "Memory write",
    "Memory write",
    "DMA read",
    "DMA read",
    "DMA write",
    "DMA write",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
};

static const int lpc_ct_dr_wr[] = {
    0,
    0,
    1,
    1,
    0,
    0,
    1,
    1,
    0,
    0,
    1,
    1,
    0,
    0,
    0,
    0,
};



static const char* lpc_sync_names[] = {
    "Ready",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Short wait",
    "Long wait",
    "Reserved",
    "Reserved",
    "Ready more (DMA only)",
    "Error",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
};

static struct srd_channel lpc_channels[] = {
    { "lframe", "LFRAME#", "Frame", 0, SRD_CHANNEL_COMMON, "dec_lpc_chan_lframe" },
    { "lclk", "LCLK", "Clock", 1, SRD_CHANNEL_SCLK, "dec_lpc_chan_lclk" },
    { "lad0", "LAD[0]", "Addr/control/data 0", 2, SRD_CHANNEL_SDATA, "dec_lpc_chan_lad0" },
    { "lad1", "LAD[1]", "Addr/control/data 1", 3, SRD_CHANNEL_SDATA, "dec_lpc_chan_lad1" },
    { "lad2", "LAD[2]", "Addr/control/data 2", 4, SRD_CHANNEL_SDATA, "dec_lpc_chan_lad2" },
    { "lad3", "LAD[3]", "Addr/control/data 3", 5, SRD_CHANNEL_SDATA, "dec_lpc_chan_lad3" },
};

static struct srd_channel lpc_optional_channels[] = {
    { "lreset", "LRESET#", "Reset", 6, SRD_CHANNEL_COMMON, "dec_lpc_chan_lreset" },
    { "ldrq", "LDRQ#", "Encoded DMA / bus master request", 7, SRD_CHANNEL_COMMON, "dec_lpc_chan_ldrq" },
    { "serirq", "SERIRQ", "Serialized IRQ", 8, SRD_CHANNEL_COMMON, "dec_lpc_chan_serirq" },
    { "clkrun", "CLKRUN#", "Clock run", 9, SRD_CHANNEL_COMMON, "dec_lpc_chan_clkrun" },
    { "lpme", "LPME#", "LPC power management event", 10, SRD_CHANNEL_COMMON, "dec_lpc_chan_lpme" },
    { "lpcpd", "LPCPD#", "Power down", 11, SRD_CHANNEL_COMMON, "dec_lpc_chan_lpcpd" },
    { "lsmi", "LSMI#", "System Management Interrupt", 12, SRD_CHANNEL_COMMON, "dec_lpc_chan_lsmi" },
};

static const char* lpc_ann_labels[][3] = {
    { "", "warnings", "Warnings" },
    { "", "start", "Start" },
    { "", "cycle-type", "Cycle-type/direction" },
    { "", "addr", "Address" },
    { "", "tar1", "Turn-around cycle 1" },
    { "", "sync", "Sync" },
    { "", "timeout", "Time Out" },
    { "", "data", "Data" },
    { "", "tar2", "Turn-around cycle 2" },
};

static const int lpc_row_data_classes[] = { ANN_START, ANN_CYCLE_TYPE, ANN_ADDR, ANN_TAR1, ANN_SYNC, ANN_TIMEOUT, ANN_DATA, ANN_TAR2, -1 };
static const int lpc_row_warnings_classes[] = { ANN_WARN, -1 };
static const struct srd_c_ann_row lpc_ann_rows[] = {
    { "data", "Data", lpc_row_data_classes, 8 },
    { "warnings", "Warnings", lpc_row_warnings_classes, 1 },
};

static const char* lpc_inputs[] = { "logic", NULL };
static const char* lpc_tags[] = { "PC", NULL };

/* Helper: convert 4-bit value to binary string (replaces %04b which MSVC doesn't support) */
static const char* to_bin4(int val)
{
    static char buf[5];
    buf[0] = '0' + ((val >> 3) & 1);
    buf[1] = '0' + ((val >> 2) & 1);
    buf[2] = '0' + ((val >> 1) & 1);
    buf[3] = '0' + (val & 1);
    buf[4] = '\0';
    return buf;
}

static void lpc_putb(struct srd_decoder_inst* di, lpc_decoder_state* s, int cls, const char* text)
{
    c_put(di, s->ss_block, s->es_block, s->out_ann, cls, text);
}

static void lpc_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(lpc_decoder_state)));
    }
    lpc_decoder_state* s = (lpc_decoder_state*)c_decoder_get_private(di);
    memset(s, 0, sizeof(lpc_decoder_state));
    s->state = LPC_IDLE;
    s->oldlframe = -1;
    s->oldlad = -1;
    s->out_ann = 0;
}

static void lpc_start(struct srd_decoder_inst* di)
{
    lpc_decoder_state* s = (lpc_decoder_state*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "lpc");
}

static void lpc_handle_get_start(struct srd_decoder_inst* di, lpc_decoder_state* s, int lframe, int lad)
{
    (void)lad;
    s->es_block = di_samplenum(di);
    /* Match Python: use oldlad (previous iteration's LAD) for START annotation */
    if (s->oldlad >= 0 && s->oldlad <= 15) {
        char short1[8], short2[4];
        snprintf(short1, sizeof(short1), "START");
        snprintf(short2, sizeof(short2), "St");
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_START,
            lpc_start_names[s->oldlad], short1, short2, "S");
    }
    s->ss_block = di_samplenum(di);

    if (lframe != 1)
        return;

    if (s->oldlad == 0x0 || s->oldlad == 0x5) {
        s->start_field = s->oldlad;
        s->state = LPC_GET_CT_DR;
    } else if (s->oldlad == 0xD || s->oldlad == 0xE) {
        s->start_field = s->oldlad;
        if (s->oldlad == 0xE)
            s->direction = 1;
        else
            s->direction = 0;
        s->state = LPC_GET_FW_IDSEL;
    } else {
        s->state = LPC_IDLE;
    }
}

static void lpc_handle_get_ct_dr(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    if (s->oldlad >= 0 && s->oldlad <= 15) {
        s->cycle_type = s->oldlad;
        s->direction = lpc_ct_dr_wr[s->oldlad];
    }

    s->es_block = di_samplenum(di);
    if (s->oldlad >= 0 && s->oldlad <= 15) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Cycle type: %s", lpc_ct_dr_names[s->oldlad]);
        lpc_putb(di, s, ANN_CYCLE_TYPE, buf);
    }
    s->ss_block = di_samplenum(di);

    s->state = LPC_GET_ADDR;
    s->addr = 0;
    s->cur_nibble = 0;
}

static void lpc_handle_get_fw_idsel(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    s->es_block = di_samplenum(di);
    char buf[64];
    snprintf(buf, sizeof(buf), "IDSEL: 0x%x", s->oldlad);
    lpc_putb(di, s, ANN_ADDR, buf);
    s->ss_block = di_samplenum(di);

    s->state = LPC_GET_FW_ADDR;
    s->addr = 0;
    s->cur_nibble = 0;
}

static void lpc_handle_get_fw_addr(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    int addr_nibbles = 7;
    int offset = ((addr_nibbles - 1) - s->cur_nibble) * 4;

    if (offset < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Warning: Invalid address shift: %d", offset);
        lpc_putb(di, s, ANN_WARN, buf);
        s->state = LPC_IDLE;
        return;
    }
    s->addr |= ((uint64_t)s->oldlad << offset);

    if (s->cur_nibble < addr_nibbles - 1) {
        s->cur_nibble++;
        return;
    }

    s->es_block = di_samplenum(di);
    char buf[64];
    snprintf(buf, sizeof(buf), "Address: 0x%07x", (unsigned int)s->addr);
    lpc_putb(di, s, ANN_ADDR, buf);
    s->ss_block = di_samplenum(di);

    s->state = LPC_GET_FW_MSIZE;
}

static void lpc_handle_get_fw_msize(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    s->es_block = di_samplenum(di);
    char buf[64];
    snprintf(buf, sizeof(buf), "MSIZE: 0x%x", s->oldlad);
    lpc_putb(di, s, ANN_ADDR, buf);
    s->ss_block = di_samplenum(di);

    s->msize = s->oldlad;

    if (s->direction == 1) {
        s->state = LPC_GET_FW_DATA;
        s->cycle_count = 0;
        s->dataword = 0;
        s->cur_nibble = 0;
    } else {
        s->state = LPC_GET_TAR;
        s->tarcount = 0;
    }
}

static void lpc_handle_get_addr(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    int addr_nibbles;

    if (s->cycle_type >= 0x0 && s->cycle_type <= 0x3) {
        addr_nibbles = 4;   /* I/O read (0,1) and I/O write (2,3) */
    } else if (s->cycle_type >= 0x4 && s->cycle_type <= 0x7) {
        addr_nibbles = 8;   /* Memory read (4,5) and Memory write (6,7) */
    } else {
        addr_nibbles = 0;
    }

    if (addr_nibbles == 0) {
        s->state = s->direction ? LPC_GET_DATA : LPC_GET_TAR;
        if (s->state == LPC_GET_DATA) {
            s->cycle_count = 0;
        } else {
            s->tarcount = 0;
        }
        return;
    }

    int offset = ((addr_nibbles - 1) - s->cur_nibble) * 4;
    if (offset < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Warning: Invalid address shift: %d", offset);
        lpc_putb(di, s, ANN_WARN, buf);
        s->state = LPC_IDLE;
        return;
    }
    s->addr |= ((uint64_t)s->oldlad << offset);

    if (s->cur_nibble < addr_nibbles - 1) {
        s->cur_nibble++;
        return;
    }

    s->es_block = di_samplenum(di);
    char buf[64];
    if (addr_nibbles <= 4)
        snprintf(buf, sizeof(buf), "Address: 0x%04x", (unsigned int)s->addr);
    else
        snprintf(buf, sizeof(buf), "Address: 0x%08x", (unsigned int)s->addr);
    lpc_putb(di, s, ANN_ADDR, buf);
    s->ss_block = di_samplenum(di);

    if (s->direction == 1) {
        s->state = LPC_GET_DATA;
        s->cycle_count = 0;
    } else {
        s->state = LPC_GET_TAR;
        s->tarcount = 0;
    }
}

static void lpc_handle_get_tar(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    s->es_block = di_samplenum(di);
    char buf[64];
    snprintf(buf, sizeof(buf), "TAR, cycle %d: %s", s->tarcount, to_bin4(s->oldlad));
    lpc_putb(di, s, ANN_TAR1, buf);
    s->ss_block = di_samplenum(di);

    if (s->oldlad != 0xF) {
        char wbuf[80];
        snprintf(wbuf, sizeof(wbuf), "TAR, cycle %d: %s (expected 1111)", s->tarcount, to_bin4(s->oldlad));
        lpc_putb(di, s, ANN_WARN, wbuf);
    }

    if (s->tarcount != 1) {
        s->tarcount++;
        return;
    }

    s->tarcount = 0;
    s->state = LPC_GET_SYNC;
}

static void lpc_handle_get_sync(struct srd_decoder_inst* di, lpc_decoder_state* s, int lframe)
{
    const char* sync_name = (s->oldlad >= 0 && s->oldlad <= 15) ? lpc_sync_names[s->oldlad] : "Unknown";

    s->es_block = di_samplenum(di);
    char buf[64];
    snprintf(buf, sizeof(buf), "SYNC, cycle %d: %s", s->synccount, to_bin4(s->oldlad));
    lpc_putb(di, s, ANN_SYNC, buf);
    s->ss_block = di_samplenum(di);

    if (strcmp(sync_name, "Reserved") == 0) {
        char wbuf[80];
        snprintf(wbuf, sizeof(wbuf), "SYNC, cycle %d: %s (reserved value)", s->synccount, to_bin4(s->oldlad));
        lpc_putb(di, s, ANN_WARN, wbuf);
    }

    if (strcmp(sync_name, "Short wait") != 0 && strcmp(sync_name, "Long wait") != 0) {
        s->cycle_count = 0;
        if (lframe == 0) {
            s->state = LPC_GET_TIMEOUT;
        } else if (s->start_field == 0xD || s->start_field == 0xE) {
            s->state = LPC_GET_FW_DATA;
            s->cycle_count = 0;
            s->dataword = 0;
            s->cur_nibble = 0;
        } else {
            s->state = LPC_GET_DATA;
        }
    }
}

static void lpc_handle_get_timeout(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    if (s->oldlframe != 0) {
        lpc_putb(di, s, ANN_WARN, "TIMEOUT cycle, LFRAME# must be low for 4 LCLk cycles");
        s->timeoutcount = 0;
        s->state = LPC_IDLE;
        return;
    }

    s->es_block = di_samplenum(di);
    char buf[32];
    snprintf(buf, sizeof(buf), "Timeout %d", s->timeoutcount);
    lpc_putb(di, s, ANN_TIMEOUT, buf);
    s->ss_block = di_samplenum(di);

    if (s->timeoutcount != 3) {
        s->timeoutcount++;
        return;
    }

    s->timeoutcount = 0;
    s->state = LPC_IDLE;
}

static void lpc_handle_get_fw_data(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    int data_nibbles;

    if (s->msize == 0x0)
        data_nibbles = 2;
    else if (s->msize == 0x1)
        data_nibbles = 4;
    else if (s->msize == 0x2)
        data_nibbles = 8;
    else if (s->msize == 0x4)
        data_nibbles = 32;
    else if (s->msize == 0x7)
        data_nibbles = 256;
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Warning: Invalid MSIZE: %d", s->msize);
        lpc_putb(di, s, ANN_WARN, buf);
        s->state = LPC_IDLE;
        return;
    }

    int nibble_swap = s->cur_nibble % 2;
    int offset = ((data_nibbles - 1) - s->cur_nibble) * 4;
    if (nibble_swap)
        offset += 4;
    else
        offset -= 4;

    if (offset < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Warning: Invalid data shift: %d", offset);
        lpc_putb(di, s, ANN_WARN, buf);
        s->state = LPC_IDLE;
        return;
    }
    s->dataword |= ((uint64_t)s->oldlad << offset);

    if (s->cur_nibble < data_nibbles - 1) {
        s->cur_nibble++;
        return;
    }

    s->es_block = di_samplenum(di);
    char buf[80];
    if (data_nibbles <= 8)
        snprintf(buf, sizeof(buf), "DATA: 0x%0*llx", data_nibbles, (unsigned long long)s->dataword);
    else
        snprintf(buf, sizeof(buf), "DATA: 0x%0*llx...", 8, (unsigned long long)s->dataword);
    lpc_putb(di, s, ANN_DATA, buf);
    s->ss_block = di_samplenum(di);

    s->cycle_count = 0;
    s->state = LPC_GET_TAR2;
}

static void lpc_handle_get_data(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    if (s->cycle_count == 0) {
        s->databyte = (uint8_t)s->oldlad;
    } else if (s->cycle_count == 1) {
        s->databyte |= (uint8_t)(s->oldlad << 4);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Warning: Invalid cycle_count: %d", s->cycle_count);
        lpc_putb(di, s, ANN_WARN, buf);
        s->state = LPC_IDLE;
        return;
    }

    if (s->cycle_count != 1) {
        s->cycle_count++;
        return;
    }

    s->es_block = di_samplenum(di);
    char buf[32];
    snprintf(buf, sizeof(buf), "DATA: 0x%02x", s->databyte);
    lpc_putb(di, s, ANN_DATA, buf);
    s->ss_block = di_samplenum(di);

    s->cycle_count = 0;
    s->state = LPC_GET_TAR2;
}

static void lpc_handle_get_tar2(struct srd_decoder_inst* di, lpc_decoder_state* s)
{
    s->es_block = di_samplenum(di);
    char buf[64];
    snprintf(buf, sizeof(buf), "TAR, cycle %d: %s", s->tarcount, to_bin4(s->oldlad));
    lpc_putb(di, s, ANN_TAR2, buf);
    s->ss_block = di_samplenum(di);

    if (s->oldlad != 0xF) {
        char wbuf[80];
        snprintf(wbuf, sizeof(wbuf), "Warning: TAR, cycle %d: %s (expected 1111)", s->tarcount, to_bin4(s->oldlad));
        lpc_putb(di, s, ANN_WARN, wbuf);
    }

    if (s->tarcount != 1) {
        s->tarcount++;
        return;
    }

    s->tarcount = 0;
    s->state = LPC_IDLE;
}

static void lpc_decode(struct srd_decoder_inst* di)
{
    lpc_decoder_state* s = (lpc_decoder_state*)c_decoder_get_private(di);
    while (1) {
        /* Wait for LCLK rising edge; in IDLE state also wait for LFRAME falling */
        int ret;
        if (s->state == LPC_IDLE && s->oldlframe != 0)
            ret = c_wait(di, CW_R(CH_LCLK), CW_OR, CW_F(CH_LFRAME), CW_END);
        else
            ret = c_wait(di, CW_R(CH_LCLK), CW_END);
        if (ret != SRD_OK)
            return;

        /* If only LFRAME fell (not LCLK rising), skip processing — LAD data
         * is only valid at LCLK rising edge. Match Python which goes back to
         * loop top and waits for LCLK rising after LFRAME falls. */
        if (s->state == LPC_IDLE && s->oldlframe != 0) {
            uint64_t matched = di_matched(di);
            if (!(matched & 0b01)) {
                /* LCLK didn't rise, only LFRAME fell — skip this iteration */
                s->oldlframe = c_pin(di, CH_LFRAME);
                continue;
            }
        }

        int lframe = c_pin(di, CH_LFRAME);
        int lad0 = c_pin(di, CH_LAD0);
        int lad1 = c_pin(di, CH_LAD1);
        int lad2 = c_pin(di, CH_LAD2);
        int lad3 = c_pin(di, CH_LAD3);
        int lad = (lad3 << 3) | (lad2 << 2) | (lad1 << 1) | lad0;

        if (lframe == 0 && s->oldlframe == 0) {
            s->state = LPC_GET_TIMEOUT;
        }

        switch (s->state) {
        case LPC_IDLE:
            if (lframe == 0) {
                s->ss_block = di_samplenum(di);
                s->state = LPC_GET_START;
                s->oldlad = -1;
            }
            /* If lframe != 0, we already waited for LFRAME falling via the
             * combined c_wait above. On the next iteration, if LFRAME fell,
             * lframe will be 0 and we'll enter LPC_GET_START. */
            break;

        case LPC_GET_START:
            lpc_handle_get_start(di, s, lframe, lad);
            break;

        case LPC_GET_CT_DR:
            lpc_handle_get_ct_dr(di, s);
            break;

        case LPC_GET_ADDR:
            lpc_handle_get_addr(di, s);
            break;

        case LPC_GET_FW_IDSEL:
            lpc_handle_get_fw_idsel(di, s);
            break;

        case LPC_GET_FW_ADDR:
            lpc_handle_get_fw_addr(di, s);
            break;

        case LPC_GET_FW_MSIZE:
            lpc_handle_get_fw_msize(di, s);
            break;

        case LPC_GET_TAR:
            lpc_handle_get_tar(di, s);
            break;

        case LPC_GET_SYNC:
            lpc_handle_get_sync(di, s, lframe);
            break;

        case LPC_GET_TIMEOUT:
            lpc_handle_get_timeout(di, s);
            break;

        case LPC_GET_FW_DATA:
            lpc_handle_get_fw_data(di, s);
            break;

        case LPC_GET_DATA:
            lpc_handle_get_data(di, s);
            break;

        case LPC_GET_TAR2:
            lpc_handle_get_tar2(di, s);
            break;
        }

        s->oldlframe = lframe;
        s->oldlad = lad;
    }
}

static void lpc_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder lpc_c_decoder = {
    .id = "lpc_c",
    .name = "LPC(C)",
    .longname = "Low Pin Count (C)",
    .desc = "LPC bus protocol decoder (C implementation, faster than Python)",
    .license = "gplv2+",
    .channels = lpc_channels,
    .num_channels = 6,
    .optional_channels = lpc_optional_channels,
    .num_optional_channels = 7,
    .options = NULL,
    .num_options = 0,
    .num_annotations = 9,
    .ann_labels = lpc_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = lpc_ann_rows,
    .inputs = lpc_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = lpc_tags,
    .num_tags = 1,
    .reset = lpc_reset,
    .start = lpc_start,
    .decode = lpc_decode,
    .destroy = lpc_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &lpc_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}