#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum maple_ann {
    ANN_START = 0,
    ANN_END,
    ANN_START_CRC,
    ANN_OCCUPANCY,
    ANN_RESET,
    ANN_BIT,
    ANN_SIZE,
    ANN_SOURCE,
    ANN_DEST,
    ANN_COMMAND,
    ANN_DATA,
    ANN_CHECKSUM,
    ANN_FRAME_ERROR,
    ANN_CHECKSUM_ERROR,
    ANN_SIZE_ERROR,
    NUM_ANN,
};

struct maple_priv {
    uint64_t ss;
    uint64_t es;
    uint64_t last_samplenum;
    int data;
    int length;
    int expected_length;
    int checksum;
    int pending_bit;
    uint64_t pending_bit_pos;
    int out_ann;
    int out_binary;
};

static struct srd_channel maple_channels[] = {
    {"sdcka", "SDCKA", "Data/clock line A", 0, SRD_CHANNEL_SCLK, "dec_maple_bus_chan_sdcka"},
    {"sdckb", "SDCKB", "Data/clock line B", 1, SRD_CHANNEL_SDATA, "dec_maple_bus_chan_sdckb"},
};

static const char *maple_ann_labels[][3] = {
    {"", "start", "Start pattern"},
    {"", "end", "End pattern"},
    {"", "start-with-crc", "Start pattern with CRC"},
    {"", "occupancy", "SDCKB occupancy pattern"},
    {"", "reset", "RESET pattern"},
    {"", "bit", "Bit"},
    {"", "size", "Data size"},
    {"", "source", "Source AP"},
    {"", "dest", "Destination AP"},
    {"", "command", "Command"},
    {"", "data", "Data"},
    {"", "checksum", "Checksum"},
    {"", "frame-error", "Frame error"},
    {"", "checksum-error", "Checksum error"},
    {"", "size-error", "Size error"},
};

static const int maple_row_bits_classes[] = {ANN_START, ANN_END, ANN_START_CRC, ANN_OCCUPANCY, ANN_RESET, ANN_BIT, -1};
static const int maple_row_fields_classes[] = {ANN_SIZE, ANN_SOURCE, ANN_DEST, ANN_COMMAND, ANN_DATA, ANN_CHECKSUM, -1};
static const int maple_row_warnings_classes[] = {ANN_FRAME_ERROR, ANN_CHECKSUM_ERROR, ANN_SIZE_ERROR, -1};

static const struct srd_c_ann_row maple_ann_rows[] = {
    {"bits", "Bits", maple_row_bits_classes, 6},
    {"fields", "Fields", maple_row_fields_classes, 6},
    {"warnings", "Warnings", maple_row_warnings_classes, 3},
};

static const struct srd_decoder_binary maple_binary[] = {
    {0, "size", "Data size"},
    {1, "source", "Source AP"},
    {2, "dest", "Destination AP"},
    {3, "command", "Command code"},
    {4, "data", "Data"},
    {5, "checksum", "Checksum"},
};

static const char *maple_inputs[] = {"logic"};
static const char *maple_tags[] = {"Retro computing"};

static void output_pending_bit(struct srd_decoder_inst *di, struct maple_priv *s)
{
    if (s->pending_bit_pos) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "Bit: %d", s->pending_bit);
        char short_str[4];
        snprintf(short_str, sizeof(short_str), "%d", s->pending_bit);
        c_put(di, s->pending_bit_pos, s->pending_bit_pos, s->out_ann, ANN_BIT, bit_str, short_str);
    }
}

static void got_bit(struct srd_decoder_inst *di, struct maple_priv *s, int n, uint64_t pos)
{
    output_pending_bit(di, s);
    s->data = s->data * 2 + n;
    s->pending_bit = n;
    s->pending_bit_pos = pos;
}

static void byte_annotation(struct srd_decoder_inst *di, struct maple_priv *s,
                            int bintype, uint8_t d)
{
    static const char *ann_long[] = {"Size", "SrcAP", "DstAP", "Cmd", "Data", "Cksum"};
    static const char *ann_mid[] = {"L", "S", "D", "C", "D", "K"};

    char long_str[64], mid_str[32], short_str[8];
    snprintf(long_str, sizeof(long_str), "%s: %02X", ann_long[bintype], d);
    snprintf(short_str, sizeof(short_str), "%02X", d);

    if (bintype == 4) {
        /* Data: only 2 texts like Python ('Data: XX', 'XX') */
        c_put(di, s->ss, s->es, s->out_ann, bintype + 6, long_str, short_str);
    } else {
        snprintf(mid_str, sizeof(mid_str), "%s: %02X", ann_mid[bintype], d);
        c_put(di, s->ss, s->es, s->out_ann, bintype + 6, long_str, mid_str, short_str);
    }

    /* Binary output */
    c_put_bin(di, s->ss, s->es, s->out_binary, bintype, 1, &d);
}

static void got_byte(struct srd_decoder_inst *di, struct maple_priv *s)
{
    output_pending_bit(di, s);
    int bintype = 4;
    if (s->length < 4) {
        if (s->length == 0)
            s->expected_length = 4 * (s->data + 1);
        bintype = s->length;
    } else if (s->length == s->expected_length) {
        bintype = 5;
        if (s->data != s->checksum) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_CHECKSUM_ERROR,
                      "Cksum error", "K error", "KE");
        }
    }
    s->length++;
    s->checksum ^= s->data;
    byte_annotation(di, s, bintype, (uint8_t)s->data);
    s->pending_bit_pos = 0;
}

static void frame_error(struct srd_decoder_inst *di, struct maple_priv *s)
{
    c_put(di, s->ss, s->es, s->out_ann, 7, "Frame error", "F error", "FE");
}

/* handle_start: wait for start pattern
   Returns: 1=Start detected, 2=Start with CRC, 0=failed/reset/occupancy, -1=end of data */
static int handle_start(struct srd_decoder_inst *di, struct maple_priv *s)
{
    /* Wait for SDCKA=low, SDCKB=high */
    {
        int ret = c_wait(di, CW_L(0), CW_H(1), CW_END);
        if (ret != SRD_OK)
            return -1;
    }

    s->ss = di_samplenum(di);
    int count = 0;

    while (1) {
        int ret = c_wait(di, CW_F(1), CW_OR, CW_R(0), CW_END);
        if (ret != SRD_OK)
            return -1;

        int sdcka = c_pin(di, 0);
        int sdckb = c_pin(di, 1);
        (void)sdcka;

        if (di_matched(di) & (1ULL << 0)) {
            /* SDCKB fell */
            count++;
        }
        if (di_matched(di) & (1ULL << 1)) {
            /* SDCKA rose */
            s->es = di_samplenum(di);
            s->last_samplenum = di_samplenum(di);
            if (sdckb == 1) {
                if (count == 4) {
                    c_put(di, s->ss, s->es, s->out_ann, ANN_START, "Start pattern", "Start", "S");
                    return 1;
                } else if (count == 6) {
                    c_put(di, s->ss, s->es, s->out_ann, ANN_START_CRC, "Start pattern with CRC", "Start CRC", "SC");
                    return 2;
                } else if (count == 8) {
                    c_put(di, s->ss, s->es, s->out_ann, ANN_OCCUPANCY, "SDCKB occupancy pattern", "Occupancy", "O");
                    return 0;
                } else if (count >= 14) {
                    c_put(di, s->ss, s->es, s->out_ann, ANN_RESET, "RESET pattern", "RESET", "R");
                    return 0;
                }
            }
            frame_error(di, s);
            return 0;
        }
    }
}

/* handle_byte_or_stop: decode bit-pairs into a byte or detect end pattern
   Returns: 1=byte decoded, 0=end pattern or error, -1=end of data */
static int handle_byte_or_stop(struct srd_decoder_inst *di, struct maple_priv *s)
{
    int counta = 0;
    int countb = 0;
    int initial = 1;

    s->ss = s->last_samplenum;
    s->pending_bit_pos = 0;
    s->data = 0;

    while (countb < 4) {
        int ret = c_wait(di, CW_F(0), CW_OR, CW_F(1), CW_END);
        if (ret != SRD_OK)
            return -1;

        int sdcka = c_pin(di, 0);
        int sdckb = c_pin(di, 1);

        s->es = di_samplenum(di);

        if (di_matched(di) & (1ULL << 0)) {
            /* SDCKA fell */
            if (counta == countb) {
                got_bit(di, s, sdckb, di_samplenum(di));
                counta++;
            } else if (counta == 1 && countb == 0 && s->data == 0 && sdckb == 0) {
                /* End pattern detection */
                ret = c_wait(di, CW_H(1), CW_OR, CW_F(0), CW_OR, CW_F(1), CW_END);
                if (ret != SRD_OK)
                    return -1;

                s->es = di_samplenum(di);
                if (di_matched(di) & (1ULL << 0)) {
                    /* SDCKA=high AND SDCKB=high di_matched(di) */
                    c_put(di, s->ss, s->es, s->out_ann, ANN_END, "End pattern", "End", "E");
                } else {
                    frame_error(di, s);
                }
                return 0;
            } else {
                frame_error(di, s);
                return 0;
            }
        } else if (di_matched(di) & (1ULL << 1)) {
            /* SDCKB fell */
            if (counta == countb + 1) {
                got_bit(di, s, sdcka, di_samplenum(di));
                countb++;
            } else if (counta == 0 && countb == 0 && sdcka == 1 && initial) {
                s->ss = di_samplenum(di);
                initial = 0;
            } else {
                frame_error(di, s);
                return 0;
            }
        }
    }

    /* Wait for SDCKA=high */
    {
        int ret = c_wait(di, CW_H(0), CW_END);
        if (ret != SRD_OK)
            return -1;
    }

    s->es = di_samplenum(di);
    s->last_samplenum = di_samplenum(di);
    got_byte(di, s);
    return 1;
}

static void maple_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct maple_priv)));
    struct maple_priv *s = (struct maple_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct maple_priv));
    s->out_ann = -1;
    s->out_binary = -1;
}

static void maple_start(struct srd_decoder_inst *di)
{
    struct maple_priv *s = (struct maple_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "maple_bus");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "maple_bus");
}

static void maple_decode(struct srd_decoder_inst *di)
{
    struct maple_priv *s = (struct maple_priv *)c_decoder_get_private(di);

    while (1) {
        /* Wait for start pattern */
        int start_type = handle_start(di, s);
        if (start_type < 0)
            return;  /* End of data */
        if (start_type == 0)
            continue;

        /* Decode bytes */
        s->length = 0;
        s->expected_length = 4;
        s->checksum = 0;

        while (1) {
            int ret = handle_byte_or_stop(di, s);
            if (ret < 0)
                return;  /* End of data */
            if (ret == 0)
                break;
        }
    }
}

static void maple_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder maple_bus_c_decoder = {
    .id = "maple_bus_c",
    .name = "Maple bus(C)",
    .longname = "SEGA Maple bus (C)",
    .desc = "Maple bus peripheral protocol for SEGA Dreamcast (C implementation)",
    .license = "gplv2+",
    .channels = maple_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = maple_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = maple_ann_rows,
    .inputs = maple_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = maple_binary,
    .num_binary = 6,
    .tags = maple_tags,
    .num_tags = 1,
    .reset = maple_reset,
    .start = maple_start,
    .decode = maple_decode,
    .destroy = maple_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &maple_bus_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}