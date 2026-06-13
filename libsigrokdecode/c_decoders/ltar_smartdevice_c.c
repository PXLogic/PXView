#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BIT_START = 0,
    ANN_BIT_DATA,
    ANN_BIT_STOP,
    ANN_BIT_SPACER,
    ANN_BIT_BLOCKEND,
    ANN_FRAME,
    ANN_FRAME_ERROR,
    ANN_BLOCK,
    ANN_BLOCK_ERROR,
    NUM_ANN,
};

enum ltar_sd_state {
    LTAR_SD_IDLE,
    LTAR_SD_DATA,
    LTAR_SD_FRAMESTOP,
    LTAR_SD_WAITING_BLOCKEND,
};

#define LTAR_SD_MAX_FRAMES 32
#define LTAR_SD_FRAME_BITS 10

typedef struct {
    int out_ann;
    int out_python;
    enum ltar_sd_state state;
    /* Current frame */
    uint64_t frame_bits_ss[LTAR_SD_FRAME_BITS];
    uint64_t frame_bits_es[LTAR_SD_FRAME_BITS];
    int frame_bits_val[LTAR_SD_FRAME_BITS];
    int frame_bit_count;
    /* Current block */
    int block_frame_count;
    uint64_t block_start_ss;
    uint64_t block_end_es;
    /* Per-frame data for block output */
    uint8_t block_frame_bytes[LTAR_SD_MAX_FRAMES];
    uint64_t block_frame_ss[LTAR_SD_MAX_FRAMES];
    uint64_t block_frame_es[LTAR_SD_MAX_FRAMES];
    /* Spacer count */
    int spacer_count;
} ltar_sd_state;

static const char *ltar_sd_inputs[] = {"afsk_bits", NULL};
static const char *ltar_sd_outputs[] = {"ltar_smartdevice", NULL};
static const char *ltar_sd_tags[] = {"Embedded/industrial", NULL};

static const char *ltar_sd_ann_labels[][3] = {
    {"", "Start Bit", "Start Bit"},
    {"", "Data Bit", "Data Bit"},
    {"", "Stop Bit", "Stop Bit"},
    {"", "Spacer Bit", "Spacer Bit"},
    {"", "Block Stop Bit", "Block Stop Bit"},
    {"", "Data frame", "Data frame"},
    {"", "Framing error", "Framing error"},
    {"", "Data block", "Data block"},
    {"", "Block error", "Block error"},
};

static const int ltar_sd_row_bits_classes[] = {
    ANN_BIT_START, ANN_BIT_DATA, ANN_BIT_STOP, ANN_BIT_SPACER, ANN_BIT_BLOCKEND, -1
};
static const int ltar_sd_row_frames_classes[] = {ANN_FRAME, ANN_FRAME_ERROR, -1};
static const int ltar_sd_row_blocks_classes[] = {ANN_BLOCK, ANN_BLOCK_ERROR, -1};

static const struct srd_c_ann_row ltar_sd_ann_rows[] = {
    {"bits", "Bits", ltar_sd_row_bits_classes, 5},
    {"frames", "Frames", ltar_sd_row_frames_classes, 2},
    {"blocks", "Blocks", ltar_sd_row_blocks_classes, 2},
};

static void ltar_sd_reset_frame(ltar_sd_state *s)
{
    s->frame_bit_count = 0;
}

static void ltar_sd_reset_block(ltar_sd_state *s)
{
    s->block_frame_count = 0;
    s->spacer_count = 0;
}

static void ltar_sd_put_frame(struct srd_decoder_inst *di, ltar_sd_state *s, int data)
{
    char buf[64], buf2[64], buf3[64];
    snprintf(buf, sizeof(buf), "Data frame: 0x%02X", data);
    snprintf(buf2, sizeof(buf2), "Data: 0x%02X", data);
    snprintf(buf3, sizeof(buf3), "D 0x%02X", data);
    c_put(di, s->frame_bits_ss[0], s->frame_bits_es[s->frame_bit_count - 1],
              s->out_ann, ANN_FRAME, buf, buf2, buf3);

    /* Store frame data for block output */
    if (s->block_frame_count < LTAR_SD_MAX_FRAMES) {
        s->block_frame_bytes[s->block_frame_count] = (uint8_t)data;
        s->block_frame_ss[s->block_frame_count] = s->frame_bits_ss[0];
        s->block_frame_es[s->block_frame_count] = s->frame_bits_es[s->frame_bit_count - 1];
    }

    /* Output per-frame protocol data for upper-layer decoders */
    if (s->out_python >= 0) {
        c_proto(di, s->frame_bits_ss[0], s->frame_bits_es[s->frame_bit_count - 1],
                s->out_python, "FRAME",
                C_U8(data), C_U64(s->frame_bits_ss[0]), C_U64(s->frame_bits_es[s->frame_bit_count - 1]),
                C_END);
    }
}

static void ltar_sd_put_frame_error(struct srd_decoder_inst *di, ltar_sd_state *s)
{
    c_put(di, s->frame_bits_ss[0], s->frame_bits_es[s->frame_bit_count - 1],
              s->out_ann, ANN_FRAME_ERROR, "Data framing error", "Framing error", "Frame Error", "FE");
}

static void ltar_sd_put_block(struct srd_decoder_inst *di, ltar_sd_state *s)
{
    char buf[64], buf2[64];
    snprintf(buf, sizeof(buf), "Block, %d frames", s->block_frame_count);
    snprintf(buf2, sizeof(buf2), "B %d", s->block_frame_count);
    c_put(di, s->block_start_ss, s->block_end_es, s->out_ann, ANN_BLOCK, buf, buf2);

    /* Output block-end protocol signal for upper-layer decoders */
    if (s->out_python >= 0) {
        c_proto(di, s->block_start_ss, s->block_end_es, s->out_python, "BLOCK_END",
                C_U8(s->block_frame_count), C_END);
    }
}

static void ltar_sd_put_block_error(struct srd_decoder_inst *di, ltar_sd_state *s)
{
    c_put(di, s->block_start_ss, s->block_end_es, s->out_ann, ANN_BLOCK_ERROR, "Block Error", "Block E", "BE");
}

static void ltar_sd_abort_current(struct srd_decoder_inst *di, ltar_sd_state *s)
{
    if (s->frame_bit_count > 0)
        ltar_sd_put_frame_error(di, s);
    ltar_sd_reset_frame(s);
    if (s->block_frame_count > 0)
        ltar_sd_put_block_error(di, s);
    ltar_sd_reset_block(s);
    s->state = LTAR_SD_IDLE;
}

static void ltar_sd_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ltar_sd_state *s = (ltar_sd_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "BIT") == 0) {
        int bit_val = (n_fields > 0) ? fields[0].u8 : 0;

        switch (s->state) {
        case LTAR_SD_IDLE:
            if (bit_val == 0) {
                /* Start bit */
                c_put(di, start_sample, end_sample, s->out_ann, ANN_BIT_START,
                          "Start Bit", "Start B", "Start");
                if (s->block_frame_count == 0)
                    s->block_start_ss = start_sample;
                s->frame_bits_ss[0] = start_sample;
                s->frame_bits_es[0] = end_sample;
                s->frame_bits_val[0] = bit_val;
                s->frame_bit_count = 1;
                s->state = LTAR_SD_DATA;
            }
            break;

        case LTAR_SD_DATA:
            /* Collect 8 data bits */
            c_put(di, start_sample, end_sample, s->out_ann, ANN_BIT_DATA,
                      bit_val ? "1" : "0");
            if (s->frame_bit_count < LTAR_SD_FRAME_BITS) {
                s->frame_bits_ss[s->frame_bit_count] = start_sample;
                s->frame_bits_es[s->frame_bit_count] = end_sample;
                s->frame_bits_val[s->frame_bit_count] = bit_val;
                s->frame_bit_count++;
            }
            if (s->frame_bit_count == 9)
                s->state = LTAR_SD_FRAMESTOP;
            break;

        case LTAR_SD_FRAMESTOP:
            if (bit_val == 1) {
                /* End of a data frame */
                c_put(di, start_sample, end_sample, s->out_ann, ANN_BIT_STOP,
                          "Stop Bit", "Stop B", "Stop");
                if (s->frame_bit_count < LTAR_SD_FRAME_BITS) {
                    s->frame_bits_ss[s->frame_bit_count] = start_sample;
                    s->frame_bits_es[s->frame_bit_count] = end_sample;
                    s->frame_bits_val[s->frame_bit_count] = bit_val;
                    s->frame_bit_count++;
                }

                /* Bit-swap: LSB first -> MSB first */
                int data = 0;
                for (int i = 8; i >= 1; i--)
                    data = (data << 1) | (s->frame_bits_val[i] & 1);

                ltar_sd_put_frame(di, s, data);
                s->block_frame_count++;
                s->block_end_es = end_sample;
                ltar_sd_reset_frame(s);
                s->state = LTAR_SD_WAITING_BLOCKEND;
            } else {
                /* Framing error */
                if (s->frame_bit_count < LTAR_SD_FRAME_BITS) {
                    s->frame_bits_ss[s->frame_bit_count] = start_sample;
                    s->frame_bits_es[s->frame_bit_count] = end_sample;
                    s->frame_bits_val[s->frame_bit_count] = bit_val;
                    s->frame_bit_count++;
                }
                ltar_sd_put_frame_error(di, s);
                ltar_sd_reset_frame(s);
                if (s->block_frame_count > 0)
                    ltar_sd_put_block_error(di, s);
                ltar_sd_reset_block(s);
                s->state = LTAR_SD_IDLE;
            }
            break;

        case LTAR_SD_WAITING_BLOCKEND:
            if (bit_val == 1) {
                /* Spacer bits between frames/blocks */
                if (s->spacer_count < 14) {
                    c_put(di, start_sample, end_sample, s->out_ann, ANN_BIT_SPACER,
                              "Spacer Bit", "Spacer");
                    s->spacer_count++;
                } else {
                    /* End of a block */
                    c_put(di, start_sample, end_sample, s->out_ann, ANN_BIT_BLOCKEND,
                              "Block Stop", "Block");
                    ltar_sd_put_block(di, s);
                    ltar_sd_reset_block(s);
                    s->state = LTAR_SD_IDLE;
                }
            } else {
                /* Start bit of another frame */
                if (s->spacer_count < 10) {
                    c_put(di, start_sample, end_sample, s->out_ann, ANN_BIT_START,
                              "Start Bit", "Start B", "Start");
                    s->frame_bits_ss[0] = start_sample;
                    s->frame_bits_es[0] = end_sample;
                    s->frame_bits_val[0] = bit_val;
                    s->frame_bit_count = 1;
                    s->spacer_count = 0;
                    s->state = LTAR_SD_DATA;
                } else {
                    /* Too many spacers, error */
                    s->frame_bits_ss[0] = start_sample;
                    s->frame_bits_es[0] = end_sample;
                    s->frame_bits_val[0] = bit_val;
                    s->frame_bit_count = 1;
                    ltar_sd_put_frame_error(di, s);
                    ltar_sd_reset_frame(s);
                    if (s->block_frame_count > 0)
                        ltar_sd_put_block_error(di, s);
                    ltar_sd_reset_block(s);
                    s->state = LTAR_SD_IDLE;
                }
            }
            break;
        }
    } else if (strcmp(cmd, "ERROR") == 0) {
        /* Abort all current decodes */
        ltar_sd_abort_current(di, s);
    }
}

static void ltar_sd_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ltar_sd_state)));
    }
    ltar_sd_state *s = (ltar_sd_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ltar_sd_state));
    s->state = LTAR_SD_IDLE;
}

static void ltar_sd_start(struct srd_decoder_inst *di)
{
    ltar_sd_state *s = (ltar_sd_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ltar_smartdevice");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "ltar_smartdevice");
}

static void ltar_sd_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ltar_sd_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ltar_smartdevice_c_decoder = {
    .id = "ltar_smartdevice_c",
    .name = "LTAR SmartDevice(C)",
    .longname = "LTAR SmartDevice (C)",
    .desc = "A decoder for the LTAR laser tag blaster's Smart Device protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ltar_sd_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ltar_sd_ann_rows,
    .inputs = ltar_sd_inputs,
    .num_inputs = 1,
    .outputs = ltar_sd_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ltar_sd_tags,
    .num_tags = 1,
    .reset = ltar_sd_reset,
    .start = ltar_sd_start,
    .decode = ltar_sd_decode,
    .destroy = ltar_sd_destroy,
    .decode_upper = ltar_sd_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ltar_smartdevice_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}