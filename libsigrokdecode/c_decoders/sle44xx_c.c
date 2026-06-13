#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SLE44xx memory card decoder */

#define CH_RST 0
#define CH_CLK 1
#define CH_IO  2

enum sle44xx_ann {
    ANN_RESET_SYM = 0,
    ANN_INTR_SYM,
    ANN_START_SYM,
    ANN_STOP_SYM,
    ANN_BIT_SYM,
    ANN_ATR_BYTE,
    ANN_CMD_BYTE,
    ANN_OUT_BYTE,
    ANN_PROC_BYTE,
    ANN_ATR_DATA,
    ANN_CMD_DATA,
    ANN_OUT_DATA,
    ANN_PROC_DATA,
    NUM_ANN,
};

enum sle44xx_state {
    STATE_NONE = -1,
    STATE_ATR = 0,
    STATE_CMD,
    STATE_DATA,
    STATE_OUT,
    STATE_PROC,
};

#define SLE44XX_MAX_BYTES 260

struct sle44xx_priv {
    int state;
    uint64_t samplerate;
    int max_addr;

    /* Bit collection */
    struct { int val; uint64_t ss; uint64_t es; } bits[8];
    int bit_count;
    int cur_bit_started;

    /* ATR byte collection */
    struct { uint8_t data; uint64_t ss; uint64_t es; } atr_bytes[4];
    int atr_count;

    /* CMD byte collection */
    struct { uint8_t data; uint64_t ss; uint64_t es; } cmd_bytes[3];
    int cmd_count;

    /* Command parse result */
    int cmd_proc;
    int out_len;

    /* OUT byte collection */
    struct { uint8_t data; uint64_t ss; uint64_t es; } out_bytes[SLE44XX_MAX_BYTES];
    int out_count;

    /* PROC state */
    struct {
        uint64_t ss;
        uint64_t es;
        int clk_count;
        int io_high;
    } proc_state;

    int out_ann;
    int out_binary;
};

static struct srd_channel sle44xx_channels[] = {
    { "rst", "RST", "Reset line", 0, SRD_CHANNEL_COMMON, "dec_sle44xx_chan_rst" },
    { "clk", "CLK", "Clock line", 1, SRD_CHANNEL_SCLK, "dec_sle44xx_chan_clk" },
    { "io",  "I/O", "I/O data line", 2, SRD_CHANNEL_SDATA, "dec_sle44xx_chan_io" },
};

static const char *sle44xx_ann_labels[][3] = {
    { "", "Reset Symbol", "R" },
    { "", "Interrupt Symbol", "Intr" },
    { "", "Start Symbol", "ST" },
    { "", "Stop Symbol", "SP" },
    { "", "Bit Symbol", "B" },
    { "", "ATR Byte", "ATR" },
    { "", "Command Byte", "Cmd" },
    { "", "Outgoing Byte", "Out" },
    { "", "Processing Byte", "Proc" },
    { "", "ATR data", "ATR data" },
    { "", "Command data", "Cmd data" },
    { "", "Outgoing data", "Out data" },
    { "", "Processing data", "Proc data" },
};

static const int sle44xx_row_symbols_classes[] = {
    ANN_RESET_SYM, ANN_INTR_SYM, ANN_START_SYM, ANN_STOP_SYM, ANN_BIT_SYM, -1
};
static const int sle44xx_row_fields_classes[] = {
    ANN_ATR_BYTE, ANN_CMD_BYTE, ANN_OUT_BYTE, ANN_PROC_BYTE, -1
};
static const int sle44xx_row_operations_classes[] = {
    ANN_ATR_DATA, ANN_CMD_DATA, ANN_OUT_DATA, ANN_PROC_DATA, -1
};

static const struct srd_c_ann_row sle44xx_ann_rows[] = {
    { "symbols", "Symbols", sle44xx_row_symbols_classes, 5 },
    { "fields", "Fields", sle44xx_row_fields_classes, 4 },
    { "operations", "Operations", sle44xx_row_operations_classes, 4 },
};

static const struct srd_decoder_binary sle44xx_binary[] = {
    { 0, "bytes", "Bytes" },
};

static const char *sle44xx_inputs[] = { "logic" };
static const char *sle44xx_outputs[] = { NULL };
static const char *sle44xx_tags[] = { "Memory" };

static uint8_t bitpack_lsb(int bits[], int count)
{
    uint8_t val = 0;
    for (int i = 0; i < count && i < 8; i++)
        val |= (bits[i] & 1) << i;
    return val;
}

static void sle44xx_flush_queued(struct srd_decoder_inst *di, struct sle44xx_priv *s)
{
    /* ATR data */
    if (s->atr_count > 0) {
        uint64_t ss = s->atr_bytes[0].ss;
        uint64_t es = s->atr_bytes[s->atr_count - 1].es;
        char text[256] = {0};
        int pos = 0;
        for (int i = 0; i < s->atr_count; i++) {
            if (i > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
            pos += snprintf(text + pos, sizeof(text) - pos, "%02x", s->atr_bytes[i].data);
        }
        char ann_txt[256];
        snprintf(ann_txt, sizeof(ann_txt), "Answer To Reset: %s", text);
        c_put(di, ss, es, s->out_ann, ANN_ATR_DATA, ann_txt, text);
    }

    /* CMD data */
    if (s->cmd_count > 0) {
        uint64_t ss = s->cmd_bytes[0].ss;
        uint64_t es = s->cmd_bytes[s->cmd_count - 1].es;
        char text[256] = {0};
        int pos = 0;
        for (int i = 0; i < s->cmd_count; i++) {
            if (i > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
            pos += snprintf(text + pos, sizeof(text) - pos, "%02x", s->cmd_bytes[i].data);
        }
        char ann_txt[256];
        snprintf(ann_txt, sizeof(ann_txt), "Command: %s", text);
        c_put(di, ss, es, s->out_ann, ANN_CMD_DATA, ann_txt, text);
    }

    /* OUT data */
    if (s->out_count > 0) {
        uint64_t ss = s->out_bytes[0].ss;
        uint64_t es = s->out_bytes[s->out_count - 1].es;
        char text[1024] = {0};
        int pos = 0;
        for (int i = 0; i < s->out_count; i++) {
            if (i > 0) pos += snprintf(text + pos, sizeof(text) - pos, " ");
            pos += snprintf(text + pos, sizeof(text) - pos, "%02x", s->out_bytes[i].data);
        }
        char ann_txt[1024];
        snprintf(ann_txt, sizeof(ann_txt), "Outgoing: %s", text);
        c_put(di, ss, es, s->out_ann, ANN_OUT_DATA, ann_txt, text);
    }

    /* PROC data */
    if (s->proc_state.es > s->proc_state.ss) {
        char text[256];
        int clk = s->proc_state.clk_count;
        int high = s->proc_state.io_high;
        if (s->samplerate) {
            double usecs = (double)(s->proc_state.es - s->proc_state.ss) / ((double)s->samplerate / 1e6);
            double msecs = usecs / 1000.0;
            snprintf(text, sizeof(text), "%.2f ms, %d clocks, I/O %d", msecs, clk, high);
        } else {
            snprintf(text, sizeof(text), "%d clocks, I/O %d", clk, high);
        }
        c_put(di, s->proc_state.ss, s->proc_state.es, s->out_ann, ANN_PROC_DATA, text);
    }

    /* Reset accumulators */
    s->atr_count = 0;
    s->cmd_count = 0;
    s->cmd_proc = 0;
    s->out_len = 0;
    s->out_count = 0;
    memset(&s->proc_state, 0, sizeof(s->proc_state));
    s->state = STATE_NONE;
}

static void sle44xx_command_check(struct sle44xx_priv *s, int ctrl, int addr, int data,
    char *texts, int texts_size, int *out_len, int *is_proc)
{
    const char *fmt_long = NULL;

    switch (ctrl) {
    case 0x30:
        fmt_long = "read main memory, addr %02x";
        *out_len = s->max_addr - addr;
        *is_proc = 0;
        break;
    case 0x31:
        fmt_long = "read security memory";
        *out_len = 4;
        *is_proc = 0;
        break;
    case 0x33:
        fmt_long = "compare verification data, addr %02x, data %02x";
        *out_len = 0;
        *is_proc = 1;
        break;
    case 0x34:
        fmt_long = "read protection memory, addr %02x";
        *out_len = 4;
        *is_proc = 0;
        break;
    case 0x38:
        fmt_long = "update main memory, addr %02x, data %02x";
        *out_len = 0;
        *is_proc = 1;
        break;
    case 0x39:
        fmt_long = "update security memory, addr %02x, data %02x";
        *out_len = 0;
        *is_proc = 1;
        break;
    case 0x3c:
        fmt_long = "write protection memory, addr %02x, data %02x";
        *out_len = 0;
        *is_proc = 1;
        break;
    default:
        fmt_long = "unknown, ctrl %02x, addr %02x, data %02x";
        *out_len = 0;
        *is_proc = 0;
        break;
    }

    snprintf(texts, texts_size, fmt_long, ctrl, addr, data);
}

static void sle44xx_handle_data_byte(struct srd_decoder_inst *di, struct sle44xx_priv *s,
    uint64_t ss, uint64_t es, uint8_t data)
{
    if (s->state == STATE_ATR) {
        if (s->atr_count < 4) {
            s->atr_bytes[s->atr_count].data = data;
            s->atr_bytes[s->atr_count].ss = ss;
            s->atr_bytes[s->atr_count].es = es;
            s->atr_count++;
        }
        if (s->atr_count == 4)
            sle44xx_flush_queued(di, s);
        return;
    }

    if (s->state == STATE_CMD) {
        if (s->cmd_count < 3) {
            s->cmd_bytes[s->cmd_count].data = data;
            s->cmd_bytes[s->cmd_count].ss = ss;
            s->cmd_bytes[s->cmd_count].es = es;
            s->cmd_count++;
        }
        if (s->cmd_count == 3) {
            int ctrl = s->cmd_bytes[0].data;
            int addr = s->cmd_bytes[1].data;
            int d = s->cmd_bytes[2].data;
            char text_long[256], text_short[128];
            int out_len = 0, is_proc = 0;
            /* Generate long form */
            sle44xx_command_check(s, ctrl, addr, d, text_long, sizeof(text_long), &out_len, &is_proc);
            /* Generate short form */
            {
                const char *fmt_short;
                switch (ctrl) {
                case 0x30: fmt_short = "RD-M @%02x"; break;
                case 0x31: fmt_short = "RD-S"; break;
                case 0x33: fmt_short = "CMP-V @%02x =%02x"; break;
                case 0x34: fmt_short = "RD-P @%02x"; break;
                case 0x38: fmt_short = "WR-M @%02x =%02x"; break;
                case 0x39: fmt_short = "WR-S @%02x =%02x"; break;
                case 0x3c: fmt_short = "WR-P @%02x =%02x"; break;
                default:   fmt_short = "UNK-%02x @%02x, =%02x"; break;
                }
                snprintf(text_short, sizeof(text_short), fmt_short, ctrl, addr, d);
            }

            uint64_t css = s->cmd_bytes[0].ss;
            uint64_t ces = s->cmd_bytes[2].es;
            c_put(di, css, ces, s->out_ann, ANN_CMD_DATA, text_long, text_short);

            s->cmd_count = 0;
            s->out_len = out_len;
            s->cmd_proc = is_proc;
            s->state = STATE_NONE;
        }
        return;
    }

    if (s->state == STATE_OUT) {
        if (s->out_count < SLE44XX_MAX_BYTES) {
            s->out_bytes[s->out_count].data = data;
            s->out_bytes[s->out_count].ss = ss;
            s->out_bytes[s->out_count].es = es;
            s->out_count++;
        }
        if (s->out_len > 0 && s->out_count == s->out_len)
            sle44xx_flush_queued(di, s);
        return;
    }
}

static void sle44xx_handle_data_bit(struct srd_decoder_inst *di, struct sle44xx_priv *s,
    uint64_t ss, uint64_t es, int bit_val, int is_start_edge)
{
    /* Switch from DATA to OUT or PROC */
    if (s->state == STATE_DATA) {
        if (s->out_len > 0)
            s->state = STATE_OUT;
        else if (s->cmd_proc) {
            s->state = STATE_PROC;
            /* Match Python's processing_start(ss or es, es or ss, bit == 1) */
            s->proc_state.ss = ss ? ss : es;
            s->proc_state.es = es ? es : ss;
            s->proc_state.clk_count = 0;
            s->proc_state.io_high = (bit_val == 1);
        }
        else
            s->state = STATE_OUT;
    }

    if (s->state == STATE_PROC) {
        int high = (bit_val == 1);
        if (is_start_edge) {
            if (s->proc_state.ss == 0)
                s->proc_state.ss = ss;
        } else {
            if (es > s->proc_state.es)
                s->proc_state.es = es;
            s->proc_state.clk_count++;
            if (high)
                s->proc_state.io_high = 1;
            if (high)
                sle44xx_flush_queued(di, s);
        }
        return;
    }

    /* Start edge: record bit ss */
    if (is_start_edge) {
        if (s->bit_count < 8) {
            s->bits[s->bit_count].val = bit_val;
            s->bits[s->bit_count].ss = ss;
            s->bits[s->bit_count].es = ss;
            s->cur_bit_started = 1;
        }
        return;
    }

    /* End edge: update bit es */
    if (s->bit_count >= 8)
        return;
    if (!s->cur_bit_started)
        return;
    if (s->bit_count < 8) {
        s->bits[s->bit_count].es = es;
        /* Emit bit annotation */
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->bits[s->bit_count].val);
        c_put(di, s->bits[s->bit_count].ss, es, s->out_ann, ANN_BIT_SYM, bit_str);
        s->bit_count++;
        s->cur_bit_started = 0;
    }

    if (s->bit_count < 8)
        return;

    /* Got 8 bits - compose byte */
    int bit_vals[8];
    for (int i = 0; i < 8; i++)
        bit_vals[i] = s->bits[i].val;
    uint8_t data = bitpack_lsb(bit_vals, 8);
    uint64_t bss = s->bits[0].ss;
    uint64_t bes = s->bits[7].es;

    /* Byte annotation */
    int ann_cls;
    switch (s->state) {
    case STATE_ATR: ann_cls = ANN_ATR_BYTE; break;
    case STATE_CMD: ann_cls = ANN_CMD_BYTE; break;
    case STATE_OUT: ann_cls = ANN_OUT_BYTE; break;
    case STATE_PROC: ann_cls = ANN_PROC_BYTE; break;
    default: ann_cls = ANN_OUT_BYTE; break;
    }

    char byte_hex[8], byte_long[64], byte_mid[32];
    snprintf(byte_hex, sizeof(byte_hex), "%02x", data);
    switch (ann_cls) {
    case ANN_ATR_BYTE:
        snprintf(byte_long, sizeof(byte_long), "Answer To Reset: %s", byte_hex);
        snprintf(byte_mid, sizeof(byte_mid), "ATR: %s", byte_hex);
        c_put_v(di, bss, bes, s->out_ann, ann_cls, data, byte_long, byte_mid, byte_hex);
        break;
    case ANN_CMD_BYTE:
        snprintf(byte_long, sizeof(byte_long), "Command: %s", byte_hex);
        snprintf(byte_mid, sizeof(byte_mid), "Cmd: %s", byte_hex);
        c_put_v(di, bss, bes, s->out_ann, ann_cls, data, byte_long, byte_mid, byte_hex);
        break;
    case ANN_OUT_BYTE:
        snprintf(byte_long, sizeof(byte_long), "Outgoing data: %s", byte_hex);
        snprintf(byte_mid, sizeof(byte_mid), "Data: %s", byte_hex);
        c_put_v(di, bss, bes, s->out_ann, ann_cls, data, byte_long, byte_mid, byte_hex);
        break;
    case ANN_PROC_BYTE:
        snprintf(byte_long, sizeof(byte_long), "Internal processing: %s", byte_hex);
        snprintf(byte_mid, sizeof(byte_mid), "Proc: %s", byte_hex);
        c_put_v(di, bss, bes, s->out_ann, ann_cls, data, byte_long, byte_mid, byte_hex);
        break;
    default:
        c_put_v(di, bss, bes, s->out_ann, ann_cls, data, byte_hex);
        break;
    }

    /* Binary output */
    c_put_bin(di, bss, bes, s->out_binary, 0, 1, &data);

    /* Pass to handler */
    sle44xx_handle_data_byte(di, s, bss, bes, data);

    s->bit_count = 0;
    s->cur_bit_started = 0;
}

static void sle44xx_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct sle44xx_priv)));
    struct sle44xx_priv *s = (struct sle44xx_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct sle44xx_priv));
    s->state = STATE_NONE;
    s->max_addr = 256;
    s->out_ann = -1;
    s->out_binary = -1;
}

static void sle44xx_start(struct srd_decoder_inst *di)
{
    struct sle44xx_priv *s = (struct sle44xx_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sle44xx");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "sle44xx");
}

static void sle44xx_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct sle44xx_priv *s = (struct sle44xx_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void sle44xx_decode(struct srd_decoder_inst *di)
{
    struct sle44xx_priv *s = (struct sle44xx_priv *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);

    uint64_t ss_reset = 0, es_reset = 0;
    uint64_t ss_clk = 0, es_clk = 0;
    int has_reset_start = 0, has_rstclk = 0;

    while (1) {
        int is_outgoing = (s->state == STATE_OUT);
        int is_processing = (s->state == STATE_PROC);

        int ret = c_wait(di, CW_R(CH_RST), CW_OR, CW_F(CH_RST), CW_OR, CW_H(CH_RST), CW_R(CH_CLK), CW_OR, CW_H(CH_RST), CW_F(CH_CLK), CW_OR, CW_L(CH_RST), CW_R(CH_CLK), CW_OR, CW_L(CH_RST), CW_F(CH_CLK), CW_OR, CW_H(CH_CLK), CW_F(CH_IO), CW_OR, CW_H(CH_CLK), CW_R(CH_IO), CW_OR, CW_L(CH_RST), CW_R(CH_IO), CW_END);
        if (ret != SRD_OK)
            return;

        int io = c_pin(di, CH_IO);

        /* COND_RESET_START */
        if (di_matched(di) & (1ULL << 0)) {
            sle44xx_flush_queued(di, s);
            ss_reset = di_samplenum(di);
            es_reset = 0;
            ss_clk = 0;
            (void)ss_clk;
            es_clk = 0;
            has_reset_start = 1;
            has_rstclk = 0;
            continue;
        }
        /* COND_RESET_STOP */
        if (di_matched(di) & (1ULL << 1)) {
            if (has_reset_start) {
                es_reset = di_samplenum(di);
                sle44xx_flush_queued(di, s);
                if (has_rstclk && es_clk > 0) {
                    /* RESET with CLK pulse */
                    c_put(di, ss_reset, es_reset, s->out_ann, ANN_RESET_SYM, "Reset", "R");
                    s->bit_count = 0;
                    s->cur_bit_started = 0;
                    s->state = STATE_ATR;
                } else {
                    /* INTERRUPT (no CLK pulse) */
                    c_put(di, ss_reset, es_reset, s->out_ann, ANN_INTR_SYM, "Interrupt", "Intr", "I");
                    s->bit_count = 0;
                    s->cur_bit_started = 0;
                    s->state = STATE_NONE;
                }
                has_reset_start = 0;
            }
            continue;
        }
        /* COND_RSTCLK_START */
        if (di_matched(di) & (1ULL << 2)) {
            ss_clk = di_samplenum(di);
            (void)ss_clk;
            has_rstclk = 1;
            continue;
        }
        /* COND_RSTCLK_STOP */
        if (di_matched(di) & (1ULL << 3)) {
            es_clk = di_samplenum(di);
            continue;
        }
        /* COND_DATA_START */
        if (di_matched(di) & (1ULL << 4)) {
            sle44xx_handle_data_bit(di, s, di_samplenum(di), 0, io, 1);
            continue;
        }
        /* COND_DATA_STOP */
        if (di_matched(di) & (1ULL << 5)) {
            sle44xx_handle_data_bit(di, s, 0, di_samplenum(di), 0, 0);
            continue;
        }
        /* PROC IO high check */
        if (is_processing && (di_matched(di) & (1ULL << 8))) {
            sle44xx_handle_data_bit(di, s, di_samplenum(di), di_samplenum(di), io, 1);
            continue;
        }
        /* CMD START/STOP only outside OUT/PROC */
        if (!is_outgoing && !is_processing) {
            if (di_matched(di) & (1ULL << 6)) {
                /* START condition */
                sle44xx_flush_queued(di, s);
                c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_START_SYM, "Start", "ST", "S");
                s->bit_count = 0;
                s->cur_bit_started = 0;
                s->state = STATE_CMD;
                continue;
            }
            if (di_matched(di) & (1ULL << 7)) {
                /* STOP condition */
                c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_STOP_SYM, "Stop", "SP", "P");
                s->bit_count = 0;
                s->cur_bit_started = 0;
                s->state = STATE_DATA;
                continue;
            }
        }
    }
}

static void sle44xx_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sle44xx_c_decoder = {
    .id = "sle44xx_c",
    .name = "SLE 44xx(C)",
    .longname = "SLE44xx memory card(C)",
    .desc = "SLE 4418/28/32/42 memory card serial protocol.(C implementation)",
    .license = "gplv2+",
    .channels = sle44xx_channels,
    .num_channels = 3,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = sle44xx_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = sle44xx_ann_rows,
    .inputs = sle44xx_inputs,
    .num_inputs = 1,
    .outputs = sle44xx_outputs,
    .num_outputs = 0,
    .binary = sle44xx_binary,
    .num_binary = 1,
    .tags = sle44xx_tags,
    .num_tags = 1,
    .reset = sle44xx_reset,
    .start = sle44xx_start,
    .decode = sle44xx_decode,
    .destroy = sle44xx_destroy,
    .state_size = 0,
    .metadata = sle44xx_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &sle44xx_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}