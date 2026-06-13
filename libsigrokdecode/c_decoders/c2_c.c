#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum c2_state {
    STATE_RESET,
    STATE_START,
    STATE_INS,
    STATE_DATA_READ,
    STATE_ADDRESS_READ,
    STATE_READ_WAIT,
    STATE_WRITE_WAIT,
    STATE_ADDRESS_WRITE,
    STATE_DATA_READ_LEN,
    STATE_DATA_WRITE_LEN,
    STATE_DATA_WRITE,
    STATE_END,
};

enum c2_ann {
    ANN_RAW = 0,
    ANN_C2DATA,
    ANN_WARN,
    NUM_ANN,
};

#define C2CK 0
#define C2D  1

struct c2_priv {
    int state;
    int bitcount;
    uint8_t c2data;
    uint32_t data;
    int ins;
    int dataLen;
    int remainData;
    uint64_t tf;
    uint64_t tr;
    uint64_t ss;
    uint64_t ss1;
    int out_ann;
};

static struct srd_channel c2_channels[] = {
    {"c2ck", "c2ck", "Clock", 0, SRD_CHANNEL_SCLK, "dec_c2_chan_c2ck"},
    {"c2d", "c2d", "Data", 1, SRD_CHANNEL_SDATA, "dec_c2_chan_c2d"},
};

static const char *c2_ann_labels[][3] = {
    {"106", "raw-Data", "raw data"},
    {"106", "c2-data", "c2 data"},
    {"", "warnings", "Warnings"},
};

static const int c2_row_raw_classes[] = {ANN_RAW, -1};
static const int c2_row_c2data_classes[] = {ANN_C2DATA, -1};
static const int c2_row_warn_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row c2_ann_rows[] = {
    {"raw-Data", "raw data", c2_row_raw_classes, 1},
    {"c2-data", "c2 data", c2_row_c2data_classes, 1},
    {"warnings", "Warnings", c2_row_warn_classes, 1},
};

static const char *c2_inputs[] = {"logic", NULL};
static const char *c2_outputs[] = {"C2", NULL};
static const char *c2_tags[] = {"Embedded/mcu", NULL};

static void c2_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct c2_priv)));
    }
    struct c2_priv *s = (struct c2_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct c2_priv));
    s->state = STATE_RESET;
}

static void c2_start(struct srd_decoder_inst *di)
{
    struct c2_priv *s = (struct c2_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "C2");
}

static void c2_decode(struct srd_decoder_inst *di)
{
    struct c2_priv *s = (struct c2_priv *)c_decoder_get_private(di);
    int ret;

    if (!c_samplerate(di))
        return;

    while (1) {
        ret = c_wait(di, CW_E(C2CK), CW_END);
        if (ret != SRD_OK)
            return;

        int c2ck = c_pin(di, C2CK);
        int c2d = c_pin(di, C2D);

        if (c2ck == 0) {
            s->tf = di_samplenum(di);

            switch (s->state) {
            case STATE_DATA_READ:
                if (s->bitcount == 0) {
                    s->ss = s->tr;
                    s->c2data = 0;
                }
                s->c2data |= c2d << s->bitcount;
                s->bitcount++;
                if (s->bitcount >= 8) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%02X", s->c2data);
                    c_put(di, s->ss, s->tf, s->out_ann, ANN_RAW, buf);
                    s->bitcount = 0;
                    s->data |= (uint32_t)s->c2data << ((s->dataLen - s->remainData) * 8);
                    s->remainData--;
                    if (s->remainData == 0)
                        s->state = STATE_END;
                }
                break;

            case STATE_ADDRESS_READ:
                if (s->bitcount == 0) {
                    s->ss = s->tr;
                    s->c2data = 0;
                }
                s->c2data |= c2d << s->bitcount;
                s->bitcount++;
                if (s->bitcount >= 8) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "%02X", s->c2data);
                    c_put(di, s->ss, s->tf, s->out_ann, ANN_RAW, buf);
                    s->state = STATE_END;
                }
                break;

            case STATE_READ_WAIT:
                if (s->bitcount == 0)
                    s->ss = s->tf;
                s->bitcount++;
                if (c2d == 1) {
                    c_put(di, s->ss, s->tf, s->out_ann, ANN_RAW, "Wait", "W");
                    s->bitcount = 0;
                    s->state = STATE_DATA_READ;
                }
                break;

            case STATE_WRITE_WAIT:
                if (s->bitcount == 0)
                    s->ss = s->tr;
                s->bitcount++;
                if (c2d == 1) {
                    c_put(di, s->ss, s->tf, s->out_ann, ANN_RAW, "Wait", "W");
                    s->state = STATE_END;
                }
                break;

            default:
                break;
            }
        } else {
            s->tr = di_samplenum(di);

            uint64_t samplerate = c_samplerate(di);
            double interval = (double)(s->tr - s->tf) * 1000000.0 / (double)samplerate;

            if (interval > 20.0) {
                c_put(di, s->tf, s->tr, s->out_ann, ANN_RAW, "Reset", "R");
                s->state = STATE_START;
            } else {
                switch (s->state) {
                case STATE_START:
                    c_put(di, s->tf, s->tr, s->out_ann, ANN_RAW, "Start", "S");
                    s->state = STATE_INS;
                    s->bitcount = 0;
                    s->ins = 0;
                    s->data = 0;
                    s->dataLen = 0;
                    s->ss1 = s->tf;
                    break;

                case STATE_INS:
                    if (s->bitcount == 0) {
                        s->ss = s->tr;
                        s->c2data = 0;
                    }
                    s->ins |= c2d << s->bitcount;
                    s->bitcount++;
                    if (s->bitcount >= 2) {
                        ret = c_wait(di, CW_F(C2CK), CW_END);
                        if (ret != SRD_OK)
                            return;

                        if (s->ins == 0)
                            s->state = STATE_DATA_READ_LEN;
                        else if (s->ins == 2)
                            s->state = STATE_ADDRESS_READ;
                        else if (s->ins == 1)
                            s->state = STATE_DATA_WRITE_LEN;
                        else
                            s->state = STATE_ADDRESS_WRITE;

                        char buf[4];
                        snprintf(buf, sizeof(buf), "%1d", s->ins);
                        c_put(di, s->ss, di_samplenum(di), s->out_ann, ANN_RAW, buf);
                        s->bitcount = 0;
                    }
                    break;

                case STATE_ADDRESS_WRITE:
                    if (s->bitcount == 0) {
                        s->ss = s->tr;
                        s->c2data = 0;
                    }
                    s->c2data |= c2d << s->bitcount;
                    s->bitcount++;
                    if (s->bitcount >= 8) {
                        ret = c_wait(di, CW_F(C2CK), CW_END);
                        if (ret != SRD_OK)
                            return;

                        s->tf = di_samplenum(di);
                        char buf[8];
                        snprintf(buf, sizeof(buf), "%02X", s->c2data);
                        c_put(di, s->ss, s->tf, s->out_ann, ANN_RAW, buf);
                        s->bitcount = 0;
                        s->state = STATE_END;
                    }
                    break;

                case STATE_DATA_READ_LEN:
                    if (s->bitcount == 0) {
                        s->ss = s->tr;
                        s->c2data = 0;
                    }
                    s->c2data |= c2d << s->bitcount;
                    s->bitcount++;
                    if (s->bitcount >= 2) {
                        s->dataLen = s->c2data + 1;
                        s->remainData = s->dataLen;
                        char buf[4];
                        snprintf(buf, sizeof(buf), "%01d", s->c2data);
                        c_put(di, s->ss, di_samplenum(di), s->out_ann, ANN_RAW, buf);
                        s->state = STATE_READ_WAIT;
                        s->bitcount = 0;
                    }
                    break;

                case STATE_DATA_WRITE_LEN:
                    if (s->bitcount == 0) {
                        s->ss = s->tr;
                        s->c2data = 0;
                    }
                    s->c2data |= c2d << s->bitcount;
                    s->bitcount++;
                    if (s->bitcount >= 2) {
                        s->dataLen = s->c2data + 1;
                        s->remainData = s->dataLen;

                        ret = c_wait(di, CW_F(C2CK), CW_END);
                        if (ret != SRD_OK)
                            return;

                        char buf[4];
                        snprintf(buf, sizeof(buf), "%01d", s->c2data);
                        c_put(di, s->ss, di_samplenum(di), s->out_ann, ANN_RAW, buf);
                        s->state = STATE_DATA_WRITE;
                        s->bitcount = 0;
                        s->c2data = 0;
                    }
                    break;

                case STATE_DATA_WRITE:
                    if (s->bitcount == 0) {
                        s->ss = s->tr;
                        s->c2data = 0;
                    }
                    s->c2data |= c2d << s->bitcount;
                    s->bitcount++;
                    if (s->bitcount >= 8) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "%02X", s->c2data);
                        c_put(di, s->ss, s->tr, s->out_ann, ANN_RAW, buf);
                        s->bitcount = 0;
                        s->data |= (uint32_t)s->c2data << ((s->dataLen - s->remainData) * 8);
                        s->remainData--;
                        if (s->remainData == 0)
                            s->state = STATE_WRITE_WAIT;
                    }
                    break;

                case STATE_END:
                    s->state = STATE_START;
                    c_put(di, s->tf, s->tr, s->out_ann, ANN_RAW, "End", "E");
                    if (s->ins == 0) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "ReadData(%01d)=0x%02X", s->dataLen, (unsigned int)s->data);
                        c_put(di, s->ss1, s->tr, s->out_ann, ANN_C2DATA, buf);
                    } else if (s->ins == 1) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "WriteData(0x%02X,%01d)", (unsigned int)s->data, s->dataLen);
                        c_put(di, s->ss1, s->tr, s->out_ann, ANN_C2DATA, buf);
                    } else if (s->ins == 2) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "ReadAddress()=0x%02X", s->c2data);
                        c_put(di, s->ss1, s->tr, s->out_ann, ANN_C2DATA, buf);
                    } else if (s->ins == 3) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "WriteAddress(0x%02X)", s->c2data);
                        c_put(di, s->ss1, s->tr, s->out_ann, ANN_C2DATA, buf);
                    }
                    break;

                default:
                    break;
                }
            }
        }
    }
}

static void c2_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder c2_c_decoder = {
    .id = "c2_c",
    .name = "C2(C)",
    .longname = "Silabs C2 Interface (C)",
    .desc = "Half-duplex, synchronous, serial bus. (C implementation)",
    .license = "gplv2+",
    .channels = c2_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = c2_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = c2_ann_rows,
    .inputs = c2_inputs,
    .num_inputs = 1,
    .outputs = c2_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = c2_tags,
    .num_tags = 1,
    .reset = c2_reset,
    .start = c2_start,
    .decode = c2_decode,
    .destroy = c2_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &c2_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}