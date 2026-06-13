#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_START = 0,
    ANN_RESET,
    ANN_BIT,
    ANN_OUTBITS,
    ANN_BYTE,
    ANN_WORD,
    ANN_CTRLDATA,
    ANN_CTRLPAD,
    ANN_CTRLTAP,
    ANN_CTRLMOUSE,
    ANN_CTRLUNKN,
    ANN_WARNING,
    NUM_ANN,
};

enum {
    STATE_FIND_START,
    STATE_CHECK_RESET,
    STATE_START_BIT,
    STATE_END_BIT,
};

#define CH_TRG  0
#define CH_CLK  1
#define CH_DATA 2
#define CH_DIR  3

typedef struct {
    uint64_t samplerate;
    int state;
    uint64_t startsamplenum;
    int triggertype;      /* 0=normal, 1=reset */
    uint64_t startbit;    /* current bit start sample */
    int bitvalue;         /* actual electrical value */
    int dispbit;          /* display value */
    int have_direction;   /* DIR channel connected */
    int dir;              /* direction value */
    int bitcount;
    uint32_t bits_value[32]; /* 32 bits internal value (inverted) */
    uint64_t bits_start[32]; /* each bit start sample */
    uint64_t bits_end[32];   /* each bit end sample */
    int bitvals;          /* 0=electrical, 1=internal */
    int out_ann;
} pcfx_state;

static struct srd_channel pcfx_channels[] = {
    {"trigger", "TRG", "Trigger", 0, SRD_CHANNEL_SDATA, NULL},
    {"clk", "CLK", "Clock", 1, SRD_CHANNEL_SCLK, NULL},
    {"data", "DATA", "Data", 2, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_channel pcfx_optional_channels[] = {
    {"dir", "DIR", "Data Direction", 3, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option pcfx_options[] = {
    {"bitvals", NULL, "Show bit values", NULL, NULL},
};

static const char *pcfx_ann_labels[][3] = {
    {"", "start", "Start"},
    {"", "reset", "Reset"},
    {"", "bit", "Bit"},
    {"", "outbits", "Outbound Bits"},
    {"", "byte", "Byte"},
    {"", "word", "Word"},
    {"", "ctrldata", "Controller Data"},
    {"", "ctrlpad", "Joypad Controller"},
    {"", "ctrltap", "Multitap Controller"},
    {"", "ctrlmouse", "Mouse Controller"},
    {"", "ctrlunkn", "Unknown Controller"},
    {"", "warning", "Warnings"},
};

static const int pcfx_row_starts_classes[] = {ANN_START, ANN_RESET, -1};
static const int pcfx_row_bits_classes[] = {ANN_BIT, ANN_OUTBITS, -1};
static const int pcfx_row_bytes_classes[] = {ANN_BYTE, -1};
static const int pcfx_row_words_classes[] = {ANN_WORD, -1};
static const int pcfx_row_controller_classes[] = {ANN_CTRLDATA, ANN_CTRLPAD, ANN_CTRLTAP, ANN_CTRLMOUSE, ANN_CTRLUNKN, -1};
static const int pcfx_row_warnings_classes[] = {ANN_WARNING, -1};
static const struct srd_c_ann_row pcfx_ann_rows[] = {
    {"starts", "Start", pcfx_row_starts_classes, 2},
    {"bits", "Bits", pcfx_row_bits_classes, 2},
    {"bytes", "Bytes", pcfx_row_bytes_classes, 1},
    {"words", "Words", pcfx_row_words_classes, 1},
    {"controller", "Controller", pcfx_row_controller_classes, 5},
    {"warnings", "Warnings", pcfx_row_warnings_classes, 1},
};

static const char *pcfx_inputs[] = {"logic"};
static const char *pcfx_outputs[] = {NULL};
static const char *pcfx_tags[] = {"Retro computing"};

static uint32_t pcfx_get_bitfield(pcfx_state *s, int start_bit, int field_size)
{
    uint32_t value = 0;
    for (int i = 0; i < field_size; i++)
        value |= (s->bits_value[start_bit + i] << i);
    return value;
}

static void pcfx_putbit(struct srd_decoder_inst *di, pcfx_state *s,
                         int annot_type, uint32_t value, int bitnum, const char *dispval)
{
    if (value & (1 << bitnum)) {
        c_put(di, s->bits_start[bitnum], s->bits_end[bitnum],
                  s->out_ann, annot_type, dispval);
    }
}

static void pcfx_handle_complete(struct srd_decoder_inst *di, pcfx_state *s)
{
    /* Output 4 bytes */
    for (int byteseq = 0; byteseq < 4; byteseq++) {
        int startbit = byteseq * 8;
        uint32_t value = pcfx_get_bitfield(s, startbit, 8);
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%2.2X", value);
        c_put(di, s->bits_start[startbit], s->bits_end[startbit + 7],
                  s->out_ann, ANN_BYTE, buf);
    }

    /* Output word */
    uint32_t word_val = pcfx_get_bitfield(s, 0, 32);
    char word_buf[16];
    snprintf(word_buf, sizeof(word_buf), "0x%8.8X", word_val);
    c_put(di, s->bits_start[0], s->bits_end[31],
              s->out_ann, ANN_WORD, word_buf);

    /* Controller type detection */
    uint32_t ctrl_type = pcfx_get_bitfield(s, 28, 4);
    if (ctrl_type == 15) {
        /* Joypad */
        c_put(di, s->bits_start[28], s->bits_end[31],
                  s->out_ann, ANN_CTRLPAD, "Joypad");
        uint32_t btns = pcfx_get_bitfield(s, 0, 16);
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 0, "I");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 1, "II");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 2, "III");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 3, "IV");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 4, "V");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 5, "VI");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 6, "Sel");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 7, "Run");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 8, "Up");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 9, "Right");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 10, "Down");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 11, "Left");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 12, "Mode 1");
        pcfx_putbit(di, s, ANN_CTRLDATA, btns, 14, "Mode 2");
    } else if (ctrl_type == 14) {
        /* Multitap */
        c_put(di, s->bits_start[28], s->bits_end[31],
                  s->out_ann, ANN_CTRLTAP, "Multitap");
    } else if (ctrl_type == 13) {
        /* Mouse */
        c_put(di, s->bits_start[28], s->bits_end[31],
                  s->out_ann, ANN_CTRLMOUSE, "Mouse");

        /* Y coordinate */
        uint32_t yval = pcfx_get_bitfield(s, 0, 8);
        char y_buf[32];
        if (yval & 0x80) {
            snprintf(y_buf, sizeof(y_buf), "Y=%d (Up)", (int)(0 - yval));
        } else {
            snprintf(y_buf, sizeof(y_buf), "Y=%d (Down)", (int)yval);
        }
        c_put(di, s->bits_start[0], s->bits_end[7],
                  s->out_ann, ANN_CTRLDATA, y_buf);

        /* X coordinate */
        uint32_t xval = pcfx_get_bitfield(s, 8, 8);
        char x_buf[32];
        if (xval & 0x80) {
            snprintf(x_buf, sizeof(x_buf), "X=%d (Left)", (int)(0 - xval));
        } else {
            snprintf(x_buf, sizeof(x_buf), "X=%d (Right)", (int)xval);
        }
        c_put(di, s->bits_start[8], s->bits_end[15],
                  s->out_ann, ANN_CTRLDATA, x_buf);

        /* Left button */
        uint32_t lbtn = pcfx_get_bitfield(s, 16, 1);
        if (lbtn == 1) {
            c_put(di, s->bits_start[16], s->bits_end[16],
                      s->out_ann, ANN_CTRLDATA, "Left");
        }

        /* Right button */
        uint32_t rbtn = pcfx_get_bitfield(s, 17, 1);
        if (rbtn == 1) {
            c_put(di, s->bits_start[17], s->bits_end[17],
                      s->out_ann, ANN_CTRLDATA, "Right");
        }
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "(%d)", ctrl_type);
        c_put(di, s->bits_start[28], s->bits_end[31],
                  s->out_ann, ANN_CTRLUNKN, buf);
    }
}

static void pcfx_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(pcfx_state)));
    pcfx_state *s = (pcfx_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(pcfx_state));
    s->state = STATE_FIND_START;
}

static void pcfx_start(struct srd_decoder_inst *di)
{
    pcfx_state *s = (pcfx_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pcfx");
    s->samplerate = c_samplerate(di);
    s->have_direction = c_has_ch(di, CH_DIR);

    const char *bv = c_opt_str(di, "bitvals", "electrical");
    s->bitvals = (strcmp(bv, "internal") == 0) ? 1 : 0;
}

static void pcfx_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    pcfx_state *s = (pcfx_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void pcfx_decode(struct srd_decoder_inst *di)
{
    pcfx_state *s = (pcfx_state *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate == 0) return;
    }

    while (1) {
        switch (s->state) {

        case STATE_FIND_START: {
            ret = c_wait(di, CW_F(CH_TRG), CW_END);
            if (ret != SRD_OK)
                return;
            s->startsamplenum = di_samplenum(di);
            s->state = STATE_CHECK_RESET;
            s->triggertype = 0;
            break;
        }

        case STATE_CHECK_RESET: {
            ret = c_wait(di, CW_L(CH_TRG), CW_F(CH_CLK), CW_OR, CW_R(CH_TRG), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & 0x1) {
                /* TRG low + CLK fall = Reset joypad counter */
                s->triggertype = 1;
            }

            s->bitcount = 0;
            s->startbit = di_samplenum(di);

            if (s->triggertype == 0) {
                c_put(di, s->startsamplenum, di_samplenum(di), s->out_ann, ANN_START,
                          "Trigger", "Trig", "T");
            } else {
                c_put(di, s->startsamplenum, di_samplenum(di), s->out_ann, ANN_RESET,
                          "Reset Joy Count", "Reset", "R");
            }
            s->state = STATE_START_BIT;
            break;
        }

        case STATE_START_BIT: {
            ret = c_wait(di, CW_F(CH_CLK), CW_OR, CW_F(CH_TRG), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & 0x1) {
                /* CLK fall di_matched(di) */
                s->bitvalue = c_pin(di, CH_DATA);
                s->bits_value[s->bitcount] = 1 - s->bitvalue; /* internal value is inverted */
                s->bits_start[s->bitcount] = s->startbit;
                s->state = STATE_END_BIT;
            }
            /* TRG fall = framing error, ignore */
            break;
        }

        case STATE_END_BIT: {
            ret = c_wait(di, CW_R(CH_CLK), CW_OR, CW_F(CH_TRG), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & 0x1) {
                /* CLK rise di_matched(di) */
                s->bits_end[s->bitcount] = di_samplenum(di);

                int disp = (s->bitvals == 0) ? s->bitvalue : (1 - s->bitvalue);
                int ann_class;
                if (s->have_direction) {
                    int dir_val = c_pin(di, CH_DIR);
                    ann_class = (dir_val == 1) ? ANN_OUTBITS : ANN_BIT;
                } else {
                    ann_class = ANN_BIT;
                }
                char bit_str[2] = {disp ? '1' : '0', '\0'};
                c_put(di, s->startbit, di_samplenum(di), s->out_ann, ann_class, bit_str);
            }

            s->startbit = di_samplenum(di);
            s->bitcount++;

            if (s->bitcount == 32) {
                pcfx_handle_complete(di, s);
                s->state = STATE_FIND_START;
            } else {
                s->state = STATE_START_BIT;
            }
            break;
        }

        }
    }
}

static void pcfx_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder pcfx_ctrlr_c_decoder = {
    .id = "pcfx_ctrlr_c",
    .name = "PCFX Cntrlr(C)",
    .longname = "PCFX Controller (C)",
    .desc = "Controller protocol for NEC PC-FX videogame console (C implementation)",
    .license = "gplv2+",
    .channels = pcfx_channels,
    .num_channels = 3,
    .optional_channels = pcfx_optional_channels,
    .num_optional_channels = 1,
    .options = pcfx_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = pcfx_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = pcfx_ann_rows,
    .inputs = pcfx_inputs,
    .num_inputs = 1,
    .outputs = pcfx_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = pcfx_tags,
    .num_tags = 1,
    .reset = pcfx_reset,
    .start = pcfx_start,
    .metadata = pcfx_metadata,
    .decode = pcfx_decode,
    .destroy = pcfx_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GSList *bv_vals = NULL;
    bv_vals = g_slist_append(bv_vals, g_variant_new_string("electrical"));
    bv_vals = g_slist_append(bv_vals, g_variant_new_string("internal"));
    pcfx_options[0].id = "bitvals";
    pcfx_options[0].idn = "dec_pcfx_ctrlr_opt_bitvals";
    pcfx_options[0].desc = "Show bit values";
    pcfx_options[0].def = g_variant_new_string("electrical");
    pcfx_options[0].values = bv_vals;

    return &pcfx_ctrlr_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}