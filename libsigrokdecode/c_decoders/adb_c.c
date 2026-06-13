#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_LO = 0,
    ANN_HI,
    ANN_ATTN,
    ANN_GRESET,
    ANN_BIT,
    ANN_DATA,
    ANN_START,
    ANN_STOP,
    ANN_SRQ,
    ANN_RESET,
    ANN_FLUSH,
    ANN_LISTEN,
    ANN_TALK,
    ANN_UNKNOWN,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;
    int format;        /* 0=hex, 1=dec, 2=oct, 3=bin */
    uint64_t cell_s;
    int byte_val;
    int bit_count;
    int attention;
    uint64_t byte_s;
} adb_state;

static struct srd_channel adb_channels[] = {
    { "data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option adb_options[] = {
    { "format", NULL, "Data format", NULL, NULL },
};

static const char* adb_ann_labels[][3] = {
    { "", "lo", "Low" },
    { "", "hi", "High" },
    { "", "attn", "Attention" },
    { "", "greset", "Global Reset" },
    { "", "bit", "Bit" },
    { "", "data", "Data" },
    { "", "start", "Start" },
    { "", "stop", "Stop" },
    { "", "srq", "Service Request" },
    { "", "reset", "Reset" },
    { "", "flush", "Flush" },
    { "", "listen", "Listen" },
    { "", "talk", "Talk" },
    { "", "unknown", "Unknown" },
};

static const int adb_row_cells_classes[] = {ANN_LO, ANN_HI, ANN_ATTN, ANN_GRESET, ANN_SRQ};
static const int adb_row_bits_classes[] = {ANN_BIT, ANN_START, ANN_STOP};
static const int adb_row_bytes_classes[] = {ANN_DATA, ANN_RESET, ANN_FLUSH, ANN_LISTEN, ANN_TALK, ANN_UNKNOWN};

static const struct srd_c_ann_row adb_ann_rows[] = {
    { "cells", "Cells", adb_row_cells_classes, 5 },
    { "bits", "Bits", adb_row_bits_classes, 3 },
    { "bytes", "Bytes", adb_row_bytes_classes, 6 },
};

static const char* adb_inputs[] = { "logic" };
static const char* adb_tags[] = { "PC" };

static int format_bin(char *buf, int bufsize, unsigned int value, int width)
{
    int pos = 0;
    for (int i = width - 1; i >= 0 && pos < bufsize - 1; i--)
        buf[pos++] = (value & (1 << i)) ? '1' : '0';
    buf[pos] = '\0';
    return pos;
}

static void adb_put_command(struct srd_decoder_inst *di, adb_state *s,
    uint64_t ss, uint64_t es, int C)
{
    int addr = (C >> 4) & 0x0f;
    int cmd = C & 0x0f;
    int reg = C & 0x03;
    char tmp[128];

    if (cmd == 0) {
        snprintf(tmp, sizeof(tmp), "Reset:%02X", C);
        c_put(di, ss, es, s->out_ann, ANN_RESET, tmp, "RST", "R");
    } else if (cmd == 1) {
        snprintf(tmp, sizeof(tmp), "Flush:%02X", C);
        c_put(di, ss, es, s->out_ann, ANN_FLUSH, tmp, "FLS", "F");
    } else if ((cmd & 0x0c) == 0x08) {
        snprintf(tmp, sizeof(tmp), "Listen($%X,r%d) %02X", addr, reg, C);
        char short_tmp[32];
        snprintf(short_tmp, sizeof(short_tmp), "L:%X:%d", addr, reg);
        c_put(di, ss, es, s->out_ann, ANN_LISTEN, tmp, short_tmp, "L");
    } else if ((cmd & 0x0c) == 0x0c) {
        snprintf(tmp, sizeof(tmp), "Talk($%X,r%d) %02X", addr, reg, C);
        char short_tmp[32];
        snprintf(short_tmp, sizeof(short_tmp), "T:%X:%d", addr, reg);
        c_put(di, ss, es, s->out_ann, ANN_TALK, tmp, short_tmp, "T");
    } else {
        snprintf(tmp, sizeof(tmp), "Unknown:%02X", C);
        c_put(di, ss, es, s->out_ann, ANN_UNKNOWN, tmp, "Unk", "U");
    }
}

static void adb_put_data(struct srd_decoder_inst *di, adb_state *s,
    uint64_t ss, uint64_t es, int D)
{
    char tmp[32];
    switch (s->format) {
    case 0: snprintf(tmp, sizeof(tmp), "%02X", D); break;
    case 1: snprintf(tmp, sizeof(tmp), "%d", D); break;
    case 2: snprintf(tmp, sizeof(tmp), "%03o", D); break;
    case 3: format_bin(tmp, sizeof(tmp), D, 8); break;
    default: snprintf(tmp, sizeof(tmp), "%02X", D); break;
    }
    c_put(di, ss, es, s->out_ann, ANN_DATA, tmp);
}

static void adb_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(adb_state)));
    }
    adb_state *s = (adb_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(adb_state));
    s->out_ann = -1;
}

static void adb_start(struct srd_decoder_inst *di)
{
    adb_state *s = (adb_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "adb");

    const char *fmt_str = c_opt_str(di, "format", "hex");
    if (strcmp(fmt_str, "hex") == 0)
        s->format = 0;
    else if (strcmp(fmt_str, "dec") == 0)
        s->format = 1;
    else if (strcmp(fmt_str, "oct") == 0)
        s->format = 2;
    else if (strcmp(fmt_str, "bin") == 0)
        s->format = 3;
    else
        s->format = 0;
}

static void adb_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    adb_state *s = (adb_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void adb_decode(struct srd_decoder_inst *di)
{
    adb_state *s = (adb_state *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    /* Wait for first falling edge */
    int ret = c_wait(di, CW_F(0), CW_END);
    if (ret != SRD_OK)
        return;
    s->cell_s = di_samplenum(di);

    while (1) {
        /* Wait for rising edge (end of low) */
        ret = c_wait(di, CW_R(0), CW_END);
        if (ret != SRD_OK)
            return;
        uint64_t low_e = di_samplenum(di);
        double low_us = (double)(low_e - s->cell_s) * 1000000.0 / (double)s->samplerate;

        if (low_us < 100.0) {
            /* Normal cell low */
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%d", (int)((low_e - s->cell_s) * 1000000 / s->samplerate));
            c_put(di, s->cell_s, low_e, s->out_ann, ANN_LO, tmp);
            if (s->bit_count % 8 == 0)
                s->byte_s = s->cell_s;
        } else if (low_us > 1500.0) {
            /* Global reset */
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "Reset:%d", (int)((low_e - s->cell_s) * 1000000 / s->samplerate));
            c_put(di, s->cell_s, low_e, s->out_ann, ANN_GRESET, tmp, "Rst", "R");
        } else if (low_us > 500.0) {
            /* Attention (560-1040us) */
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "Attn:%d", (int)((low_e - s->cell_s) * 1000000 / s->samplerate));
            c_put(di, s->cell_s, low_e, s->out_ann, ANN_ATTN, tmp, "Attn", "A");
            s->attention = 1;
        } else {
            /* SRQ (100-500us) */
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "SRQ:%d", (int)((low_e - s->cell_s) * 1000000 / s->samplerate));
            c_put(di, s->cell_s, low_e, s->out_ann, ANN_SRQ, tmp, "SRQ", "Q");
        }

        /* Wait for falling edge (end of high / next cell start) */
        ret = c_wait(di, CW_F(0), CW_END);
        if (ret != SRD_OK)
            return;
        uint64_t cell_e = di_samplenum(di);
        double high_us = (double)(cell_e - low_e) * 1000000.0 / (double)s->samplerate;
        double cell_us = (double)(cell_e - s->cell_s) * 1000000.0 / (double)s->samplerate;

        if (high_us < 100.0) {
            /* Normal cell high */
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%d", (int)((cell_e - low_e) * 1000000 / s->samplerate));
            c_put(di, low_e, cell_e, s->out_ann, ANN_HI, tmp);

            if (cell_us <= 130.0) {
                /* Bit cell */
                s->bit_count++;
                if (s->bit_count == 0) {
                    /* Start bit (1) */
                    c_put(di, s->cell_s, cell_e, s->out_ann, ANN_START, "Start(1)", "S1", "S");
                } else {
                    if (low_us > high_us) {
                        /* bit 0 */
                        c_put(di, s->cell_s, cell_e, s->out_ann, ANN_BIT, "0");
                        s->byte_val = ((s->byte_val << 1) & 0xff) | 0;
                    } else {
                        /* bit 1 */
                        c_put(di, s->cell_s, cell_e, s->out_ann, ANN_BIT, "1");
                        s->byte_val = ((s->byte_val << 1) & 0xff) | 1;
                    }
                }

                if (s->bit_count && s->bit_count % 8 == 0) {
                    if (s->attention == 1) {
                        adb_put_command(di, s, s->byte_s, cell_e, s->byte_val);
                        s->attention = 0;
                        s->bit_count = -1;
                    } else {
                        adb_put_data(di, s, s->byte_s, cell_e, s->byte_val);
                    }
                }
            } else {
                /* cell > 130us */
                if (low_us < 100.0) {
                    /* Stop bit (0) */
                    c_put(di, s->cell_s, cell_e, s->out_ann, ANN_STOP, "Stop(0)", "T0", "T");
                } else {
                    /* Start bit (1) after attention */
                    c_put(di, low_e, cell_e, s->out_ann, ANN_START, "Start(1)", "S1", "S");
                    s->bit_count = 0;
                }
            }
        } else {
            /* high >= 100us */
            if (low_us < 100.0) {
                /* Stop bit (0) */
                c_put(di, s->cell_s, low_e, s->out_ann, ANN_STOP, "Stop(0)", "T0", "T");
            }
        }

        s->cell_s = cell_e;
    }
}

static void adb_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder adb_c_decoder = {
    .id = "adb_c",
    .name = "ADB(C)",
    .longname = "Apple Desktop Bus (C)",
    .desc = "Decode command and data of Apple Desktop Bus protocol.",
    .license = "mit",
    .channels = adb_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = adb_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = adb_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = adb_ann_rows,
    .inputs = adb_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = adb_tags,
    .num_tags = 1,
    .reset = adb_reset,
    .start = adb_start,
    .decode = adb_decode,
    .destroy = adb_destroy,
    .state_size = 0,
    .metadata = adb_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &adb_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}