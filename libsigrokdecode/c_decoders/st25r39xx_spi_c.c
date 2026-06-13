/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * ST25R39xx NFC chip SPI protocol decoder (C implementation).
 * Ported from Python decoder st25r39xx_spi.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation enumeration ===== */
enum {
    ANN_BURST_READ = 0,
    ANN_BURST_WRITE,
    ANN_BURST_READB,
    ANN_BURST_WRITEB,
    ANN_BURST_READT,
    ANN_BURST_WRITET,
    ANN_DIRECTCMD,
    ANN_FIFO_WRITE,
    ANN_FIFO_READ,
    ANN_STATUS,
    ANN_WARN,
    NUM_ANN,
};

/* ===== Command type enumeration ===== */
enum st25r39xx_cmd {
    CMD_NONE = 0,
    CMD_WRITE,
    CMD_READ,
    CMD_WRITEB,
    CMD_READB,
    CMD_WRITET,
    CMD_READT,
    CMD_FIFO_WRITE,
    CMD_FIFO_READ,
    CMD_DIRECT,
    CMD_SPACE_B,
    CMD_TEST_ACCESS,
};

/* ===== State structure ===== */
typedef struct {
    int first;
    int cmd_type;
    int cmd_dat;
    int cmd_min;
    int cmd_max;
    uint8_t mb_mosi[1024];
    uint8_t mb_miso[1024];
    int mb_count;
    uint64_t ss_mb;
    uint64_t es_mb;
    int cs_was_released;
    int requirements_met;
    int out_ann;
} st25r39xx_state;

/* ===== Register lookup tables ===== */
static const char *regsSpaceA[] = {
    "IOCFG1", "IOCFG2", "OPCTRL", "MODEDEF", "BITRATE",
    "TYPEA", "TYPEB", "TYPEBF", "NFCIP1", "STREAM",
    "AUX", "RXCFG1", "RXCFG2", "RXCFG3", "RXCFG4",
    "MSKRXTIM", "NRESPTIM1", "NRESPTIM2", "TIMEMV", "GPTIM1",
    "GPTIM2", "PPON2", "MSKMAINIRQ", "MSKTIMNFCIRQ", "MSKERRWAKEIRQ",
    "TARGIRQ", "MAINIRQ", "TIMNFCIRQ", "ERRWAKEIRQ", "TARGIRQ",
    "FIFOSTAT1", "FIFOSTAT2", "COLLDISP", "TARGDISP", "NBTXB1",
    "NBTXB2", "BITRATEDET", "ADCONVOUT", "ANTTUNECTRL1", "ANTTUNECTRL2",
    "TXDRV", "TARGMOD", "EXTFIELDON", "EXTFIELDOFF", "REGVDDCTRL",
    "RSSIDISP", "GAINSTATE", "CAPACTRL", "CAPADISP", "AUXDISP",
    "WAKETIMCTRL", "AMPCFG", "AMPREF", "AMPAAVGDISP", "AMPDISP",
    "PHASECFG", "PHASEREF", "PHASEAAVGDISP", "PHASEDISP", "CAPACFG",
    "CAPAREF", "CAPAAAVGDISP", "CAPADISP", "ICIDENT",
};

/* Special SpaceA addresses (above 0x3F) */
typedef struct { uint8_t addr; const char *name; } st25r39xx_special_reg;
static const st25r39xx_special_reg regsSpaceA_special[] = {
    {0xA0, "PT_memLoadA"},
    {0xA8, "PT_memLoadF"},
    {0xAC, "PT_memLoadTSN"},
    {0xBF, "PT_memRead"},
    {0xFF, NULL},
};

static const char *regsSpaceB[] = {
    NULL, NULL, NULL, NULL, NULL,
    "EMDSUPPRCONF", "SUBCSTARTIM", NULL, NULL, NULL,
    NULL, "P2PRXCONF", "CORRCONF1", "CORRCONF2", NULL,
    "SQUELSHTIM", NULL, NULL, NULL, NULL,
    "NFCGUARDTIM", NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, "AUXMODSET", "TXDRVTIM", "RESAMMODE",
    "TXDRVTIMDISP", "REGDISP", NULL, NULL, NULL,
    "OSHOOTCONF1", "OSHOOTCONF2", "USHOOTCONF1", "USHOOTCONF2",
};

static const char *regsTest[] = {
    NULL, "ANTSTOBS",
};

static const char *dir_cmd[] = {
    /* 0xC0 */ "SET_DEFAULT",
    /* 0xC1 */ "SET_DEFAULT",
    /* 0xC2 */ "STOP",
    /* 0xC3 */ "STOP",
    /* 0xC4 */ "TXCRC",
    /* 0xC5 */ "TXNOCRC",
    /* 0xC6 */ "TXREQA",
    /* 0xC7 */ "TXWUPA",
    /* 0xC8 */ "NFCINITFON",
    /* 0xC9 */ "NFCRESFON",
    /* 0xCA */ NULL,
    /* 0xCB */ NULL,
    /* 0xCC */ NULL,
    /* 0xCD */ "GOIDLE",
    /* 0xCE */ "GOHALT",
    /* 0xCF */ NULL,
    /* 0xD0 */ "STOPRX",
    /* 0xD1 */ "STARRX",
    /* 0xD2 */ "SETAMSTATE",
    /* 0xD3 */ "MAMP",
    /* 0xD4 */ NULL,
    /* 0xD5 */ "RSTRXGAIN",
    /* 0xD6 */ "ADJREG",
    /* 0xD7 */ NULL,
    /* 0xD8 */ "CALDRVTIM",
    /* 0xD9 */ "MPHASE",
    /* 0xDA */ "CLRRSSI",
    /* 0xDB */ "CLRFIFO",
    /* 0xDC */ "TRMODE",
    /* 0xDD */ "CALCAPA",
    /* 0xDE */ "MCAPA",
    /* 0xDF */ "MPOWER",
    /* 0xE0 */ "STARGPTIM",
    /* 0xE1 */ "STARWTIM",
    /* 0xE2 */ "STARMSKTIM",
    /* 0xE3 */ "STARNRESPTIM",
    /* 0xE4 */ "STARPPON2TIM",
};

/* ===== SPI DATA packet helpers ===== */
static inline int spi_proto_get_mosi(const c_field *fields, int n_fields, uint8_t *mosi_val)
{
    if (n_fields < 17 || !(fields[0].u8 & 1)) {
        *mosi_val = 0;
        return 0;
    }
    *mosi_val = (uint8_t)fields[1].u8;
    return 1;
}

static inline int spi_proto_get_miso(const c_field *fields, int n_fields, uint8_t *miso_val)
{
    if (n_fields < 17 || !((fields[0].u8 >> 1) & 1)) {
        *miso_val = 0;
        return 0;
    }
    *miso_val = (uint8_t)fields[9].u8;
    return 1;
}

static inline int spi_proto_cs_change_get_values(const c_field *fields, int n_fields,
                                                  uint8_t *prev, uint8_t *cur)
{
    if (n_fields < 2) {
        *prev = 0xFF; *cur = 0xFF;
        return -1;
    }
    *prev = fields[0].u8;
    *cur = fields[1].u8;
    return 0;
}

/* ===== Static data ===== */
static const char *st25r39xx_spi_inputs[] = {"spi", NULL};
static const char *st25r39xx_spi_tags[] = {"IC", "Wireless/RF", NULL};

static const char *st25r39xx_spi_ann_labels[][3] = {
    {"", "Read", "Burst register read"},
    {"", "Write", "Burst register write"},
    {"", "ReadB", "Burst register SpaceB read"},
    {"", "WriteB", "Burst register SpaceB write"},
    {"", "ReadT", "Burst register Test read"},
    {"", "WriteT", "Burst register Test write"},
    {"", "Cmd", "Direct command"},
    {"", "FIFOW", "FIFO write"},
    {"", "FIFOR", "FIFO read"},
    {"", "status_reg", "Status register"},
    {"", "warning", "Warning"},
};

static const int st25r39xx_row_regs_classes[] = {
    ANN_BURST_READ, ANN_BURST_WRITE, ANN_BURST_READB, ANN_BURST_WRITEB,
    ANN_BURST_READT, ANN_BURST_WRITET, -1
};
static const int st25r39xx_row_cmds_classes[] = {ANN_DIRECTCMD, -1};
static const int st25r39xx_row_data_classes[] = {ANN_FIFO_WRITE, ANN_FIFO_READ, -1};
static const int st25r39xx_row_status_classes[] = {ANN_STATUS, -1};
static const int st25r39xx_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row st25r39xx_spi_ann_rows[] = {
    {"regs", "Regs", st25r39xx_row_regs_classes, 6},
    {"cmds", "Commands", st25r39xx_row_cmds_classes, 1},
    {"data", "Data", st25r39xx_row_data_classes, 2},
    {"status", "Status register", st25r39xx_row_status_classes, 1},
    {"warnings", "Warnings", st25r39xx_row_warnings_classes, 1},
};

/* ===== Helper: find register name ===== */
static const char *st25r39xx_find_reg_spaceA(uint8_t addr)
{
    if (addr <= 0x3F) {
        return regsSpaceA[addr];
    }
    for (int i = 0; regsSpaceA_special[i].name != NULL; i++) {
        if (regsSpaceA_special[i].addr == addr)
            return regsSpaceA_special[i].name;
    }
    return NULL;
}

static const char *st25r39xx_find_reg_spaceB(uint8_t addr)
{
    if (addr <= 0x33) {
        return regsSpaceB[addr];
    }
    return NULL;
}

static const char *st25r39xx_find_reg_test(uint8_t addr)
{
    if (addr <= 0x01) {
        return regsTest[addr];
    }
    return NULL;
}

static const char *st25r39xx_find_direct_cmd(uint8_t cmd_byte)
{
    if (cmd_byte >= 0xC0 && cmd_byte <= 0xE4) {
        return dir_cmd[cmd_byte - 0xC0];
    }
    return NULL;
}

/* ===== Helper: format command label ===== */
static const char *st25r39xx_format_cmd(int cmd_type, int cmd_dat)
{
    switch (cmd_type) {
    case CMD_WRITE: return "Write";
    case CMD_READ: return "Read";
    case CMD_WRITEB: return "WriteB";
    case CMD_READB: return "ReadB";
    case CMD_WRITET: return "WriteT";
    case CMD_READT: return "ReadT";
    case CMD_FIFO_WRITE: return "FIFO Write";
    case CMD_FIFO_READ: return "FIFO Read";
    case CMD_DIRECT: {
        const char *name = st25r39xx_find_direct_cmd((uint8_t)cmd_dat);
        if (name) {
            static char buf[64];
            snprintf(buf, sizeof(buf), "Cmd %s", name);
            return buf;
        }
        return "Cmd Unknown";
    }
    default: return "Unknown";
    }
}

/* ===== Parse command byte ===== */
static int st25r39xx_parse_command(st25r39xx_state *s, struct srd_decoder_inst *di,
                                   uint64_t ss, uint64_t es, uint8_t mosi)
{
    uint8_t addr = mosi & 0x3F;

    if (s->cmd_type == CMD_SPACE_B) {
        if ((mosi & 0xC0) == 0x00) { s->cmd_type = CMD_WRITEB; s->cmd_dat = addr; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
        if ((mosi & 0xC0) == 0x40) { s->cmd_type = CMD_READB; s->cmd_dat = addr; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
        c_put(di, ss, es, s->out_ann, ANN_WARN, "Unknown address/command combination");
        return -1;
    }
    if (s->cmd_type == CMD_TEST_ACCESS) {
        if ((mosi & 0xC0) == 0x00) { s->cmd_type = CMD_WRITET; s->cmd_dat = addr; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
        if ((mosi & 0xC0) == 0x40) { s->cmd_type = CMD_READT; s->cmd_dat = addr; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
        c_put(di, ss, es, s->out_ann, ANN_WARN, "Unknown address/command combination");
        return -1;
    }

    if (mosi <= 0x7F) {
        if ((mosi & 0xC0) == 0x00) { s->cmd_type = CMD_WRITE; s->cmd_dat = addr; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
        if ((mosi & 0xC0) == 0x40) { s->cmd_type = CMD_READ; s->cmd_dat = addr; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
        c_put(di, ss, es, s->out_ann, ANN_WARN, "Unknown address/command combination");
        return -1;
    }

    if (mosi == 0x80) { s->cmd_type = CMD_FIFO_WRITE; s->cmd_dat = mosi; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
    if (mosi == 0xA0 || mosi == 0xA8 || mosi == 0xAC) { s->cmd_type = CMD_WRITE; s->cmd_dat = mosi; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
    if (mosi == 0xBF) { s->cmd_type = CMD_READ; s->cmd_dat = mosi; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
    if (mosi == 0x9F) { s->cmd_type = CMD_FIFO_READ; s->cmd_dat = mosi; s->cmd_min = 1; s->cmd_max = 99999; return 0; }
    if (mosi >= 0xC0 && mosi <= 0xE8) { s->cmd_type = CMD_DIRECT; s->cmd_dat = mosi; s->cmd_min = 0; s->cmd_max = 0; return 0; }
    if (mosi == 0xFB) { s->cmd_type = CMD_SPACE_B; s->cmd_dat = mosi; s->cmd_min = 0; s->cmd_max = 0; return 0; }
    if (mosi == 0xFC) { s->cmd_type = CMD_TEST_ACCESS; s->cmd_dat = mosi; s->cmd_min = 0; s->cmd_max = 0; return 0; }

    c_put(di, ss, es, s->out_ann, ANN_WARN, "Unknown address/command combination");
    return -1;
}

/* ===== Finish command: output annotations ===== */
static void st25r39xx_finish_command(struct srd_decoder_inst *di, st25r39xx_state *s)
{
    int ann;
    const char *cmd_label;
    const char *reg_name;
    char buf[1024];

    switch (s->cmd_type) {
    case CMD_WRITE:   ann = ANN_BURST_WRITE;  break;
    case CMD_READ:    ann = ANN_BURST_READ;   break;
    case CMD_WRITEB:  ann = ANN_BURST_WRITEB; break;
    case CMD_READB:   ann = ANN_BURST_READB;  break;
    case CMD_WRITET:  ann = ANN_BURST_WRITET; break;
    case CMD_READT:   ann = ANN_BURST_READT;  break;
    case CMD_FIFO_WRITE: ann = ANN_FIFO_WRITE; break;
    case CMD_FIFO_READ:  ann = ANN_FIFO_READ;  break;
    case CMD_DIRECT:  ann = ANN_DIRECTCMD;     break;
    default:
        c_put(di, s->ss_mb, s->es_mb, s->out_ann, ANN_WARN, "Unhandled command");
        return;
    }

    cmd_label = st25r39xx_format_cmd(s->cmd_type, s->cmd_dat);

    if (s->cmd_type == CMD_DIRECT) {
        const char *dname = st25r39xx_find_direct_cmd((uint8_t)s->cmd_dat);
        if (dname)
            snprintf(buf, sizeof(buf), "Cmd %s", dname);
        else
            snprintf(buf, sizeof(buf), "Cmd 0x%02X", s->cmd_dat);
        c_put(di, s->ss_mb, s->es_mb, s->out_ann, ann, buf);
        return;
    }

    /* Find register name */
    reg_name = NULL;
    if (ann == ANN_BURST_WRITE || ann == ANN_BURST_READ) {
        reg_name = st25r39xx_find_reg_spaceA((uint8_t)s->cmd_dat);
    } else if (ann == ANN_BURST_WRITEB || ann == ANN_BURST_READB) {
        reg_name = st25r39xx_find_reg_spaceB((uint8_t)s->cmd_dat);
    } else if (ann == ANN_BURST_WRITET || ann == ANN_BURST_READT) {
        reg_name = st25r39xx_find_reg_test((uint8_t)s->cmd_dat);
    }

    /* Format data bytes */
    int is_read = (ann == ANN_BURST_READ || ann == ANN_BURST_READB ||
                   ann == ANN_BURST_READT || ann == ANN_FIFO_READ);
    uint8_t *data_bytes = is_read ? s->mb_miso : s->mb_mosi;
    int data_count = s->mb_count;

    if (reg_name) {
        /* Build data hex string */
        char hex[512];
        int pos = 0;
        for (int i = 0; i < data_count && pos < (int)sizeof(hex) - 4; i++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data_bytes[i]);
        if (pos > 0 && hex[pos-1] == ' ') hex[pos-1] = '\0';

        if (ann == ANN_FIFO_WRITE || ann == ANN_FIFO_READ) {
            snprintf(buf, sizeof(buf), "%s: %s", cmd_label, hex);
        } else {
            snprintf(buf, sizeof(buf), "%s: %s (%02X) = %s", cmd_label, reg_name, s->cmd_dat, hex);
        }
    } else {
        char hex[512];
        int pos = 0;
        for (int i = 0; i < data_count && pos < (int)sizeof(hex) - 4; i++)
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data_bytes[i]);
        if (pos > 0 && hex[pos-1] == ' ') hex[pos-1] = '\0';

        if (ann == ANN_FIFO_WRITE || ann == ANN_FIFO_READ) {
            snprintf(buf, sizeof(buf), "%s: %s", cmd_label, hex);
        } else {
            snprintf(buf, sizeof(buf), "%s: %s", cmd_label, hex);
        }
    }

    c_put(di, s->ss_mb, s->es_mb, s->out_ann, ann, buf);
}

/* ===== Reset state ===== */
static void st25r39xx_next(st25r39xx_state *s)
{
    s->first = 1;
    s->cmd_type = CMD_NONE;
    s->cmd_dat = 0;
    s->cmd_min = 0;
    s->cmd_max = 0;
    s->mb_count = 0;
    s->ss_mb = (uint64_t)-1;
    s->es_mb = 0;
}

/* ===== recv_proto ===== */
static void st25r39xx_spi_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    st25r39xx_state *s = (st25r39xx_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        uint8_t prev_cs, cur_cs;
        spi_proto_cs_change_get_values(fields, n_fields, &prev_cs, &cur_cs);

        if (prev_cs == 0xFF && cur_cs == 0xFF) {
            /* No CS pin */
            return;
        }

        if (prev_cs == 0xFF && cur_cs == 1) {
            /* Initial: CS already released */
            s->cs_was_released = 1;
            return;
        }

        if (prev_cs == 0 && cur_cs == 1) {
            /* CS rising edge: transaction complete */
            if (s->cmd_type != CMD_NONE && s->cmd_type != CMD_SPACE_B && s->cmd_type != CMD_TEST_ACCESS) {
                if (s->mb_count < s->cmd_min) {
                    c_put(di, start_sample, start_sample, s->out_ann, ANN_WARN, "Missing data bytes");
                } else if (s->mb_count > 0) {
                    st25r39xx_finish_command(di, s);
                }
            }
            st25r39xx_next(s);
            s->cs_was_released = 1;
        }
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (!s->cs_was_released)
        return;

    uint8_t mosi = 0, miso = 0;
    int have_mosi = spi_proto_get_mosi(fields, n_fields, &mosi);
    int have_miso = spi_proto_get_miso(fields, n_fields, &miso);
    (void)have_mosi; (void)have_miso;

    if (!s->requirements_met) {
        if (have_mosi && have_miso) {
            s->requirements_met = 1;
        } else {
            return;
        }
    }

    if (s->first) {
        if (mosi == 0xFB) {
            /* Space B: keep first=true, parse next byte */
            st25r39xx_parse_command(s, di, start_sample, end_sample, mosi);
            return;
        }
        if (mosi == 0xFC) {
            /* TestAccess: keep first=true, parse next byte */
            st25r39xx_parse_command(s, di, start_sample, end_sample, mosi);
            return;
        }

        /* Parse command byte */
        if (st25r39xx_parse_command(s, di, start_sample, end_sample, mosi) < 0) {
            st25r39xx_next(s);
            return;
        }

        /* Direct commands have no data bytes */
        if (s->cmd_type == CMD_DIRECT) {
            s->ss_mb = start_sample;
            s->es_mb = end_sample;
            st25r39xx_finish_command(di, s);
            st25r39xx_next(s);
            return;
        }

        s->first = 0;
        s->ss_mb = start_sample;
        s->es_mb = end_sample;
    } else {
        /* Collect data bytes */
        if (s->cmd_type == CMD_NONE || s->mb_count >= s->cmd_max) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, "Excess byte");
            return;
        }

        if (s->ss_mb == (uint64_t)-1)
            s->ss_mb = start_sample;
        s->es_mb = end_sample;

        if (s->mb_count < (int)sizeof(s->mb_mosi)) {
            s->mb_mosi[s->mb_count] = mosi;
            s->mb_miso[s->mb_count] = miso;
            s->mb_count++;
        }
    }
}

/* ===== Lifecycle callbacks ===== */
static void st25r39xx_spi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(st25r39xx_state)));
    }
    st25r39xx_state *s = (st25r39xx_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(st25r39xx_state));
    st25r39xx_next(s);
    s->requirements_met = 1;
    s->cs_was_released = 0;
}

static void st25r39xx_spi_start(struct srd_decoder_inst *di)
{
    st25r39xx_state *s = (st25r39xx_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "st25r39xx_spi");
}

static void st25r39xx_spi_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void st25r39xx_spi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== Decoder struct ===== */
struct srd_c_decoder st25r39xx_spi_c_decoder = {
    .id = "st25r39xx_spi_c",
    .name = "ST25R39xx(C)",
    .longname = "STMicroelectronics ST25R39xx (C)",
    .desc = "ST25R39xx NFC chip SPI protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = st25r39xx_spi_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = st25r39xx_spi_ann_rows,
    .inputs = st25r39xx_spi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = st25r39xx_spi_tags,
    .num_tags = 2,
    .reset = st25r39xx_spi_reset,
    .start = st25r39xx_spi_start,
    .decode = st25r39xx_spi_decode,
    .destroy = st25r39xx_spi_destroy,
    .decode_upper = st25r39xx_spi_recv_proto,
    .state_size = 0,
};

/* ===== Export functions ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &st25r39xx_spi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}