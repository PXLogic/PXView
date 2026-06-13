#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_RAW = 0,
    ANN_SOF,
    ANN_IFS,
    ANN_DATA,
    ANN_PACKET,
    NUM_ANN,
};

enum {
    STATE_IDLE,
    STATE_DATA,
};

/* VPW timing parameters (microseconds) */
#define VPW_SOF_US         200
#define VPW_SOF_L_US       164
#define VPW_SOF_H_US       245
#define VPW_LONG_US        128
#define VPW_LONG_L_US      97
#define VPW_LONG_H_US      170
#define VPW_SHORT_US       64
#define VPW_SHORT_L_US     24
#define VPW_SHORT_H_US     97
#define VPW_IFS_US         240

typedef struct {
    uint64_t samplerate;
    int state;
    int active;           /* active logic level */
    int spd;              /* 1 or 4 */
    uint8_t byte_val;
    int bit_count;
    uint64_t datastart;
    int byte_count;       /* byte offset in packet */
    int mode;             /* mode byte value */
    uint64_t csa;         /* checksum byte start sample */
    uint64_t csb;         /* checksum byte end sample */
    int out_ann;
} vpw_state;

static struct srd_channel vpw_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, NULL},
};

static const char *vpw_ann_labels[][3] = {
    {"", "raw", "Raw"},
    {"", "sof", "SOF"},
    {"", "ifs", "EOF/IFS"},
    {"", "data", "Data"},
    {"", "packet", "Packet"},
};

static const int vpw_row_raws_classes[] = {ANN_RAW, ANN_SOF, ANN_IFS, -1};
static const int vpw_row_bytes_classes[] = {ANN_DATA, -1};
static const int vpw_row_packets_classes[] = {ANN_PACKET, -1};
static const struct srd_c_ann_row vpw_ann_rows[] = {
    {"raws", "Raws", vpw_row_raws_classes, 3},
    {"bytes", "Bytes", vpw_row_bytes_classes, 1},
    {"packets", "Packets", vpw_row_packets_classes, 1},
};

static const char *vpw_inputs[] = {"logic"};
static const char *vpw_outputs[] = {NULL};
static const char *vpw_tags[] = {"Automotive"};

static int vpw_samples_to_us(vpw_state *s, uint64_t samples)
{
    return (int)((samples * 1000000ULL) / s->samplerate);
}

static void vpw_handle_bit(struct srd_decoder_inst *di, vpw_state *s,
                            uint64_t ss, uint64_t es, int bit)
{
    s->byte_val |= (bit << (7 - s->bit_count));  /* MSB-first */

    char bit_str[2] = {bit ? '1' : '0', '\0'};
    c_put(di, ss, es, s->out_ann, ANN_RAW, bit_str);

    if (s->bit_count == 0)
        s->datastart = ss;

    if (s->bit_count == 7) {
        s->csa = s->datastart;
        s->csb = es;
        char byte_str[8];
        snprintf(byte_str, sizeof(byte_str), "%02X", s->byte_val);
        c_put(di, s->datastart, es, s->out_ann, ANN_DATA, byte_str);

        /* Packet field annotation */
        if (s->byte_count == 0) {
            c_put(di, s->datastart, es, s->out_ann, ANN_PACKET,
                      "Priority", "Prio", "P");
        } else if (s->byte_count == 1) {
            c_put(di, s->datastart, es, s->out_ann, ANN_PACKET,
                      "Destination", "Dest", "D");
        } else if (s->byte_count == 2) {
            c_put(di, s->datastart, es, s->out_ann, ANN_PACKET,
                      "Source", "Src", "S");
        } else if (s->byte_count == 3) {
            c_put(di, s->datastart, es, s->out_ann, ANN_PACKET,
                      "Mode", "M");
            s->mode = s->byte_val;
        } else if (s->mode == 1 && s->byte_count == 4) {
            c_put(di, s->datastart, es, s->out_ann, ANN_PACKET,
                      "Pid", "P");
        }

        s->bit_count = -1;
        s->byte_val = 0;
        s->byte_count++;
    }
    s->bit_count++;
}

static void vpw_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(vpw_state)));
    vpw_state *s = (vpw_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(vpw_state));
    s->state = STATE_IDLE;
    s->active = 0;
    s->spd = 1;
}

static void vpw_start(struct srd_decoder_inst *di)
{
    vpw_state *s = (vpw_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sae_j1850_vpw");
    s->samplerate = c_samplerate(di);
}

static void vpw_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    vpw_state *s = (vpw_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void vpw_decode(struct srd_decoder_inst *di)
{
    vpw_state *s = (vpw_state *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate == 0) return;
    }

    /* Wait for first edge */
    ret = c_wait(di, CW_E(0), CW_END);
    if (ret != SRD_OK)
        return;

    uint64_t es = di_samplenum(di);

    while (1) {
        uint64_t ss = es;
        ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;
        es = di_samplenum(di);

        uint64_t samples = es - ss;
        int t = vpw_samples_to_us(s, samples);
        int pin = c_pin(di, 0);  /* pin level at pulse start */

        if (s->state == STATE_IDLE) {
            /* Detect SOF and set speed */
            if (pin == s->active && t >= VPW_SOF_L_US && t < VPW_SOF_H_US) {
                c_put(di, ss, es, s->out_ann, ANN_RAW, "1X SOF", "S1", "S");
                s->spd = 1;
                s->byte_val = 0;
                s->bit_count = 0;
                s->byte_count = 0;
                s->mode = 0;
                s->state = STATE_DATA;
            } else if (pin == s->active &&
                       t >= VPW_SOF_L_US / 4 && t < VPW_SOF_H_US / 4) {
                c_put(di, ss, es, s->out_ann, ANN_RAW, "4X SOF", "S4", "4");
                s->spd = 4;
                s->byte_val = 0;
                s->bit_count = 0;
                s->byte_count = 0;
                s->mode = 0;
                s->state = STATE_DATA;
            }
        } else if (s->state == STATE_DATA) {
            /* Scale timing by speed factor */
            int shortl = VPW_SHORT_L_US / s->spd;
            int shorth = VPW_SHORT_H_US / s->spd;
            int longl = VPW_LONG_L_US / s->spd;
            int longh = VPW_LONG_H_US / s->spd;
            int ifs = VPW_IFS_US / s->spd;

            if (t >= ifs) {
                /* EOF/IFS */
                s->state = STATE_IDLE;
                c_put(di, ss, es, s->out_ann, ANN_RAW, "EOF/IFS", "E");
                /* Retrospective checksum annotation */
                c_put(di, s->csa, s->csb, s->out_ann, ANN_PACKET,
                          "Checksum", "CS", "C");
                s->byte_count = 0;
            } else if (t >= shortl && t < shorth) {
                /* Short pulse */
                int bit = (pin == s->active) ? 1 : 0;
                vpw_handle_bit(di, s, ss, es, bit);
            } else if (t >= longl && t < longh) {
                /* Long pulse */
                int bit = (pin == s->active) ? 0 : 1;
                vpw_handle_bit(di, s, ss, es, bit);
            }
        }
    }
}

static void vpw_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sae_j1850_vpw_c_decoder = {
    .id = "sae_j1850_vpw_c",
    .name = "SAE J1850 VPW(C)",
    .longname = "SAE J1850 VPW. (C)",
    .desc = "SAE J1850 Variable Pulse Width 1x and 4x. (C implementation)",
    .license = "gplv2+",
    .channels = vpw_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = vpw_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = vpw_ann_rows,
    .inputs = vpw_inputs,
    .num_inputs = 1,
    .outputs = vpw_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = vpw_tags,
    .num_tags = 1,
    .reset = vpw_reset,
    .start = vpw_start,
    .metadata = vpw_metadata,
    .decode = vpw_decode,
    .destroy = vpw_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &sae_j1850_vpw_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}