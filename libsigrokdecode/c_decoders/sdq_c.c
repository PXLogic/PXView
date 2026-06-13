#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum sdq_ann {
    ANN_BIT = 0,
    ANN_BYTE,
    ANN_BREAK,
    NUM_ANN,
};

struct sdq_priv {
    uint64_t samplerate;
    int out_ann;

    double bit_width;
    double half_bit_width;
    double break_threshold;

    int bits[8];
    int bits_len;
    uint64_t startsample;
    uint64_t bytepos;
};

static struct srd_channel sdq_channels[] = {
    {"sdq", "SDQ", "Single wire SDQ data line.", 0, SRD_CHANNEL_SDATA, "dec_sdq_chan_sdq"},
};

static struct srd_decoder_option sdq_options[] = {
    {"bitrate", NULL, "Bit rate", NULL, NULL},
};

static const char *sdq_ann_labels[][3] = {
    {"", "bit", "Bit"},
    {"", "byte", "Byte"},
    {"", "break", "Break"},
};

static const int sdq_row_bits_classes[] = {ANN_BIT, -1};
static const int sdq_row_bytes_classes[] = {ANN_BYTE, -1};
static const int sdq_row_breaks_classes[] = {ANN_BREAK, -1};

static const struct srd_c_ann_row sdq_ann_rows[] = {
    {"bits", "Bits", sdq_row_bits_classes, 1},
    {"bytes", "Bytes", sdq_row_bytes_classes, 1},
    {"breaks", "Breaks", sdq_row_breaks_classes, 1},
};

static const char *sdq_inputs[] = {"logic"};
static const char *sdq_outputs[] = {};
static const char *sdq_tags[] = {"Embedded/industrial"};

static void sdq_handle_bit(struct srd_decoder_inst *di, int bit)
{
    struct sdq_priv *s = (struct sdq_priv *)c_decoder_get_private(di);

    s->bits[s->bits_len] = bit;
    s->bits_len++;

    uint64_t bit_end = s->startsample + (uint64_t)s->bit_width;

    char bit_long[16], bit_short[4];
    snprintf(bit_long, sizeof(bit_long), "Bit: %d", bit);
    snprintf(bit_short, sizeof(bit_short), "%d", bit);
    c_put(di, s->startsample, bit_end, s->out_ann, ANN_BIT, bit_long, bit_short);

    if (s->bits_len == 8) {
        /* bitpack: LSB first */
        uint8_t byte_val = 0;
        for (int i = 0; i < 8; i++)
            byte_val |= (s->bits[i] << i);

        char byte_long[16], byte_short[8];
        snprintf(byte_long, sizeof(byte_long), "Byte: 0x%02x", byte_val);
        snprintf(byte_short, sizeof(byte_short), "0x%02x", byte_val);
        c_put(di, s->bytepos, bit_end, s->out_ann, ANN_BYTE, byte_long, byte_short);

        s->bits_len = 0;
        s->bytepos = 0;
    }
}

static void sdq_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct sdq_priv)));
    struct sdq_priv *s = (struct sdq_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct sdq_priv));
    s->out_ann = -1;
}

static void sdq_start(struct srd_decoder_inst *di)
{
    struct sdq_priv *s = (struct sdq_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sdq");
}

static void sdq_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct sdq_priv *s = (struct sdq_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void sdq_decode(struct srd_decoder_inst *di)
{
    struct sdq_priv *s = (struct sdq_priv *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0)
        return;

    int64_t bitrate = c_opt_int(di, "bitrate", 98425);
    s->bit_width = (double)s->samplerate / (double)bitrate;
    s->half_bit_width = s->bit_width / 2.0;
    s->break_threshold = s->bit_width * 1.2;

    /* Wait for line to go high */
    {
        ret = c_wait(di, CW_H(0), CW_END);
        if (ret != SRD_OK)
            return;
    }

    while (1) {
        /* Wait for falling edge */
        {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
        }
        s->startsample = di_samplenum(di);
        if (s->bytepos == 0)
            s->bytepos = di_samplenum(di);

        /* Wait for rising edge */
        {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
        }

        uint64_t delta = di_samplenum(di) - s->startsample;

        if ((double)delta > s->break_threshold) {
            /* BREAK */
            c_put(di, s->startsample, di_samplenum(di), s->out_ann, ANN_BREAK, "Break", "BR");
            s->bits_len = 0;
            s->startsample = di_samplenum(di);
            s->bytepos = 0;
        } else if ((double)delta > s->half_bit_width) {
            /* Bit 0 */
            sdq_handle_bit(di, 0);
        } else {
            /* Bit 1 */
            sdq_handle_bit(di, 1);
        }
    }
}

static void sdq_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder sdq_c_decoder = {
    .id = "sdq_c",
    .name = "SDQ(C)",
    .longname = "Texas Instruments SDQ (C)",
    .desc = "Texas Instruments SDQ. The SDQ protocol is also used by Apple. (C implementation)",
    .license = "gplv2+",
    .channels = sdq_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = sdq_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = sdq_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = sdq_ann_rows,
    .inputs = sdq_inputs,
    .num_inputs = 1,
    .outputs = sdq_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sdq_tags,
    .num_tags = 1,
    .reset = sdq_reset,
    .start = sdq_start,
    .decode = sdq_decode,
    .destroy = sdq_destroy,
    .state_size = 0,
    .metadata = sdq_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    sdq_options[0].idn = "dec_sdq_opt_bitrate";
    sdq_options[0].def = g_variant_new_int64(98425);
    return &sdq_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}