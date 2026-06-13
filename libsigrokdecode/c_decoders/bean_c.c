/*
 * bean_c.c — BEAN (Toyota Body Electronics Area Network) decoder (C implementation)
 *
 * BEAN is a Toyota vehicle single-wire serial bus protocol
 * using pulse-width encoding with bit stuffing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum bean_ann {
    ANN_BIT0 = 0,
    ANN_BIT1,
    ANN_BITE_ANN,
    ANN_BYTE,
    ANN_FRAME,
    ANN_MESSAGE,
    ANN_PULSE_WIDTH,
    ANN_DEBUG,
    ANN_ALL_BYTE,
    NUM_ANN,
};

typedef struct {
    const char *key;
    const char *value;
} bean_command_entry;

static const bean_command_entry bean_commands[] = {
    { "ABA180", "OK - Power Window Master SW" },
    { "AB4080", "OK - ??? 1 ???" },
    { "ABAE0",  "OK - ??? 2 ???" },
    { "ABE00",  "OK - ??? 3 ???" },
    { "ABA80",  "OK - ??? 4 ???" },
    { "ABAB0",  "OK - ??? 5 ???" },
    { "DB01",   "UnBlock Control" },
    { "DB021",  "Block Control" },
    { "DB4C81", "Door Lock" },
    { "DB4C41", "Door UnLock" },
    { "E000400","Window Rear Left - Down" },
    { "E000600","Window Rear Left - Full Down" },
    { "E000800","Window Rear Left - Up" },
    { "E000A00","Window Rear Left - Full Up" },
    { "E004000","Window Rear Right - Down" },
    { "E006000","Window Rear Right - Full Down" },
    { "E008000","Window Rear Right - Up" },
    { "E00A000","Window Rear Right - Full Up" },
    { "E040000","Window Front Right - Down" },
    { "E060000","Window Front Right - Full Down" },
    { "E080000","Window Front Right - Up" },
    { "E0A0000","Window Front Right - Full Up" },
    { NULL, NULL },
};

typedef struct {
    uint64_t samplerate;
    int out_ann;

    /* Pulse tracking */
    uint64_t ss;
    uint64_t es;
    uint64_t samplenumber_last;
    int pin_last;
    int pin;

    /* State */
    int sof;       /* SOF detected */
    int eom;       /* EOM detected */
    int stuff;     /* Next is stuff bit */
    int draw;      /* Frame complete flag */
    int noresp;    /* No RSP flag */

    /* Collected bits */
    int bits[200];        /* bit values (0/1) */
    uint64_t bits_ss[200]; /* bit start sample */
    uint64_t bits_es[200]; /* bit end sample */
    int bit_count;

    /* bits_ann (SOF/Stuff) */
    char *bits_ann_label[100];
    uint64_t bits_ann_ss[100];
    uint64_t bits_ann_es[100];
    int bits_ann_count;

    /* Options */
    int opt_bit_annotations;
    int opt_pulse_len;
    int opt_command;
    int opt_all_byte;
} bean_state;

static struct srd_channel bean_channels[] = {
    { "data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_bean_chan_data" },
};

static struct srd_decoder_option bean_options[] = {
    { "bit_annotations", NULL, "Bit annotations", NULL, NULL },
    { "pulse_len", NULL, "Pulse length", NULL, NULL },
    { "command", NULL, "Command", NULL, NULL },
    { "all byte", NULL, "All byte", NULL, NULL },
};

static const char *bean_ann_labels[][3] = {
    { "", "bit-0", "Bit 0" },
    { "", "bit-1", "Bit 1" },
    { "", "bite-ann", "Bite_ann" },
    { "", "byte", "Byte" },
    { "", "frame", "Frame" },
    { "", "message", "Message" },
    { "", "pulse-width", "Pulse width" },
    { "", "debug", "Debug" },
    { "", "all-byte", "All byte" },
};

static const int bean_row_bits_classes[] = { ANN_BIT0, ANN_BIT1, -1 };
static const int bean_row_bits_ann_classes[] = { ANN_BITE_ANN, -1 };
static const int bean_row_bytes_classes[] = { ANN_BYTE, -1 };
static const int bean_row_frames_classes[] = { ANN_FRAME, ANN_MESSAGE, -1 };
static const int bean_row_pulse_widths_classes[] = { ANN_PULSE_WIDTH, -1 };
static const int bean_row_command_classes[] = { ANN_DEBUG, -1 };
static const int bean_row_all_byte_classes[] = { ANN_ALL_BYTE, -1 };

static const struct srd_c_ann_row bean_ann_rows[] = {
    { "bits", "Bits", bean_row_bits_classes, 2 },
    { "bits_ann", "Bits_ann", bean_row_bits_ann_classes, 1 },
    { "bytes", "Bytes", bean_row_bytes_classes, 1 },
    { "frames", "Frames", bean_row_frames_classes, 2 },
    { "pulse_widths", "Pulse_widths", bean_row_pulse_widths_classes, 1 },
    { "command", "Command", bean_row_command_classes, 1 },
    { "All_byte", "All byte", bean_row_all_byte_classes, 1 },
};

static const char *bean_inputs[] = { "logic" };
static const char *bean_tags[] = { "Embedded/industrial" };

static const char *bean_lookup_command(const char *key)
{
    for (int i = 0; bean_commands[i].key != NULL; i++) {
        if (strcmp(key, bean_commands[i].key) == 0)
            return bean_commands[i].value;
    }
    return NULL;
}

static void bean_reset_frame(bean_state *s)
{
    s->bit_count = 0;
    s->bits_ann_count = 0;
    s->sof = 0;
    s->eom = 0;
    s->stuff = 0;
    s->draw = 0;
    s->noresp = 0;
}

static void bean_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(bean_state)));
    bean_state *s = (bean_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(bean_state));
    s->out_ann = -1;
    s->samplenumber_last = 0;
}

static void bean_start(struct srd_decoder_inst *di)
{
    bean_state *s = (bean_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "bean");

    const char *ba_str = c_opt_str(di, "bit_annotations", "none");
    s->opt_bit_annotations = (strcmp(ba_str, "yes") == 0) ? 1 : 0;

    const char *pl_str = c_opt_str(di, "pulse_len", "none");
    s->opt_pulse_len = (strcmp(pl_str, "yes") == 0) ? 1 : 0;

    const char *cmd_str = c_opt_str(di, "command", "yes");
    s->opt_command = (strcmp(cmd_str, "yes") == 0) ? 1 : 0;

    const char *ab_str = c_opt_str(di, "all byte", "yes");
    s->opt_all_byte = (strcmp(ab_str, "yes") == 0) ? 1 : 0;
}

static void bean_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    bean_state *s = (bean_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void bean_parse_frame(struct srd_decoder_inst *di, bean_state *s)
{
    if (s->bit_count < 8)
        return;

    /* Parse PRI (bits 0-3) */
    int frame_pri = 0;
    for (int i = 0; i < 4 && i < s->bit_count; i++)
        frame_pri = (frame_pri << 1) | s->bits[i];

    /* Parse ML (bits 4-7) */
    int frame_length = 0;
    for (int i = 4; i < 8 && i < s->bit_count; i++)
        frame_length = (frame_length << 1) | s->bits[i];

    if (frame_length + 3 > s->bit_count / 8)
        return;

    /* Parse bytes */
    char sdata[256] = "";
    char allbyte[512] = "";
    int b = 0;

    /* Byte annotations */
    char byte_val[32][8];
    uint64_t byte_ss[32], byte_es[32];
    int byte_count = 0;

    /* byte_ann: label, ss, es, type (0=message, 1=frame) */
    char byte_ann_label[64][64];
    uint64_t byte_ann_ss[64], byte_ann_es[64];
    int byte_ann_type[64]; /* 0=message, 1=frame */
    int byte_ann_count = 0;

    /* byte_all */
    char byte_all_str[512];
    uint64_t byte_all_ss = 0, byte_all_es = 0;
    byte_all_str[0] = '\0';

    for (int i = 0; i < frame_length + 3 && i * 8 + 7 < s->bit_count; i++) {
        b = 0;
        for (int j = 0; j < 8; j++)
            b = (b << 1) | s->bits[i * 8 + j];

        snprintf(byte_val[byte_count], sizeof(byte_val[0]), "%X", b);
        byte_ss[byte_count] = s->bits_ss[i * 8];
        byte_es[byte_count] = s->bits_es[i * 8 + 7];

        char *ab_pos = allbyte + strlen(allbyte);
        snprintf(ab_pos, sizeof(allbyte) - strlen(allbyte), " %s", byte_val[byte_count]);

        if (i == 0) {
            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "PRI: 0x%x", frame_pri);
            byte_ann_ss[byte_ann_count] = s->bits_ss[0];
            byte_ann_es[byte_ann_count] = s->bits_es[3];
            byte_ann_type[byte_ann_count] = 0;
            byte_ann_count++;

            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "ML: 0x%x", frame_length);
            byte_ann_ss[byte_ann_count] = s->bits_ss[4];
            byte_ann_es[byte_ann_count] = s->bits_es[7];
            byte_ann_type[byte_ann_count] = 0;
            byte_ann_count++;
        } else if (i == 1) {
            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "DST-ID");
            byte_ann_ss[byte_ann_count] = s->bits_ss[i * 8];
            byte_ann_es[byte_ann_count] = s->bits_es[i * 8 + 7];
            byte_ann_type[byte_ann_count] = 1;
            byte_ann_count++;
        } else if (i == 2) {
            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "MES-ID");
            byte_ann_ss[byte_ann_count] = s->bits_ss[i * 8];
            byte_ann_es[byte_ann_count] = s->bits_es[i * 8 + 7];
            byte_ann_type[byte_ann_count] = 1;
            byte_ann_count++;
            strcat(sdata, byte_val[byte_count]);
        } else if (i == frame_length + 1) {
            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "CRC");
            byte_ann_ss[byte_ann_count] = s->bits_ss[i * 8];
            byte_ann_es[byte_ann_count] = s->bits_es[i * 8 + 7];
            byte_ann_type[byte_ann_count] = 0;
            byte_ann_count++;

            /* byte_all: from start to CRC+EOM */
            snprintf(byte_all_str, sizeof(byte_all_str), "%s", allbyte);
            byte_all_ss = s->bits_ss[0];
            if ((i + 1) * 8 + 7 < s->bit_count)
                byte_all_es = s->bits_es[(i + 1) * 8 + 7];
            else
                byte_all_es = s->bits_es[i * 8 + 7];
        } else if (i == frame_length + 2) {
            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "EOM");
            byte_ann_ss[byte_ann_count] = s->bits_ss[i * 8];
            byte_ann_es[byte_ann_count] = s->bits_es[i * 8 + 7];
            byte_ann_type[byte_ann_count] = 0;
            byte_ann_count++;
        } else {
            snprintf(byte_ann_label[byte_ann_count], sizeof(byte_ann_label[0]),
                     "Data%i", i - 2);
            byte_ann_ss[byte_ann_count] = s->bits_ss[i * 8];
            byte_ann_es[byte_ann_count] = s->bits_es[i * 8 + 7];
            byte_ann_type[byte_ann_count] = 1;
            byte_ann_count++;
            strcat(sdata, byte_val[byte_count]);
        }
        byte_count++;
    }

    /* Command lookup */
    if (s->opt_command) {
        const char *cmd = bean_lookup_command(sdata);
        if (cmd && byte_count > 0) {
            /* Find MES-ID start and CRC/EOM end for command annotation range */
            uint64_t cmd_ss = 0, cmd_es = 0;
            for (int i = 0; i < byte_ann_count; i++) {
                if (strcmp(byte_ann_label[i], "MES-ID") == 0)
                    cmd_ss = byte_ann_ss[i];
                if (strcmp(byte_ann_label[i], "CRC") == 0)
                    cmd_es = byte_ann_es[i];
            }
            if (cmd_ss && cmd_es)
                c_put(di, cmd_ss, cmd_es, s->out_ann, ANN_DEBUG, cmd);
        }
    }

    /* Output bits */
    for (int i = 0; i < s->bit_count; i++) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), " %d", s->bits[i]);
        int ann_class = s->bits[i] ? ANN_BIT1 : ANN_BIT0;
        c_put(di, s->bits_ss[i], s->bits_es[i], s->out_ann, ann_class, bit_str);
    }

    /* Output bit annotations (SOF/Stuff) */
    if (s->opt_bit_annotations) {
        for (int i = 0; i < s->bits_ann_count; i++) {
            c_put(di, s->bits_ann_ss[i], s->bits_ann_es[i],
                      s->out_ann, ANN_BITE_ANN, s->bits_ann_label[i]);
        }
    }

    /* Output all byte */
    if (s->opt_all_byte && byte_all_str[0]) {
        c_put(di, byte_all_ss, byte_all_es, s->out_ann, ANN_ALL_BYTE, byte_all_str);
    }

    /* Output bytes */
    for (int i = 0; i < byte_count; i++) {
        c_put(di, byte_ss[i], byte_es[i], s->out_ann, ANN_BYTE, byte_val[i]);
    }

    /* Output byte annotations */
    for (int i = 0; i < byte_ann_count; i++) {
        int ann_class = byte_ann_type[i] ? ANN_FRAME : ANN_MESSAGE;
        c_put(di, byte_ann_ss[i], byte_ann_es[i], s->out_ann, ann_class, byte_ann_label[i]);
    }
}

static void bean_decode(struct srd_decoder_inst *di)
{
    bean_state *s = (bean_state *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
    }

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int pin = c_pin(di, 0);

        if (!s->samplenumber_last) {
            s->samplenumber_last = di_samplenum(di);
            s->pin_last = pin;
            s->ss = di_samplenum(di);
            continue;
        }

        s->es = di_samplenum(di);
        uint64_t puls = s->es - s->ss;

        /* Pulse length option */
        if (s->opt_pulse_len) {
            char puls_str[32];
            snprintf(puls_str, sizeof(puls_str), " %llu", (unsigned long long)puls);
            c_put(di, s->ss, s->es, s->out_ann, ANN_PULSE_WIDTH, puls_str);
        }

        uint64_t count = puls / 100;

        if (puls > 150 && puls < 650) {
            /* Medium pulse: split into multiple bits */
            uint64_t temp_ss = s->ss;
            uint64_t i = 0;
            while (i < count - 1) {
                if (!s->stuff) {
                    if (!s->sof) {
                        s->sof = 1;
                        uint64_t temp_es = temp_ss + puls / count;
                        /* Save SOF annotation */
                        if (s->bits_ann_count < 100) {
                            s->bits_ann_label[s->bits_ann_count] = "SOF";
                            s->bits_ann_ss[s->bits_ann_count] = s->ss;
                            s->bits_ann_es[s->bits_ann_count] = temp_es;
                            s->bits_ann_count++;
                        }
                        temp_ss = temp_es;
                    } else {
                        uint64_t temp_es = temp_ss + puls / count;
                        if (s->bit_count < 200) {
                            s->bits[s->bit_count] = s->pin_last;
                            s->bits_ss[s->bit_count] = temp_ss;
                            s->bits_es[s->bit_count] = temp_es;
                            s->bit_count++;
                        }
                        temp_ss = temp_es;
                    }
                } else {
                    s->stuff = 0;
                    uint64_t temp_es = temp_ss + puls / count;
                    if (s->bits_ann_count < 100) {
                        s->bits_ann_label[s->bits_ann_count] = "Stuff";
                        s->bits_ann_ss[s->bits_ann_count] = temp_ss;
                        s->bits_ann_es[s->bits_ann_count] = temp_es;
                        s->bits_ann_count++;
                    }
                    temp_ss = temp_es;
                }
                i++;
                if (i == 4 && count == 5) {
                    s->stuff = 1;
                }
            }
            /* Last bit */
            if (s->bit_count < 200) {
                s->bits[s->bit_count] = s->pin_last;
                s->bits_ss[s->bit_count] = temp_ss;
                s->bits_es[s->bit_count] = s->es;
                s->bit_count++;
            }

            if (count == 6) {
                s->eom = 1;
            }
        } else if (puls <= 150) {
            /* Short pulse */
            if (!s->sof) {
                s->sof = 1;
                if (s->bits_ann_count < 100) {
                    s->bits_ann_label[s->bits_ann_count] = "SOF";
                    s->bits_ann_ss[s->bits_ann_count] = s->ss;
                    s->bits_ann_es[s->bits_ann_count] = s->es;
                    s->bits_ann_count++;
                }
            } else if (s->stuff) {
                if (s->bits_ann_count < 100) {
                    s->bits_ann_label[s->bits_ann_count] = "Stuff";
                    s->bits_ann_ss[s->bits_ann_count] = s->ss;
                    s->bits_ann_es[s->bits_ann_count] = s->es;
                    s->bits_ann_count++;
                }
                s->stuff = 0;
            } else {
                if (s->bit_count < 200) {
                    s->bits[s->bit_count] = s->pin_last;
                    s->bits_ss[s->bit_count] = s->ss;
                    s->bits_es[s->bit_count] = s->es;
                    s->bit_count++;
                }
            }
            if (s->eom) {
                /* RSP annotation - short pulse after EOM */
                if (!s->noresp && s->bit_count >= 2) {
                    c_put(di, s->bits_ss[s->bit_count - 2], s->bits_es[s->bit_count - 1],
                          s->out_ann, ANN_MESSAGE, "RSP");
                }
                s->draw = 1;
            }
        } else {
            /* puls >= 650: long pulse = frame gap / EOF */
            if (s->eom) {
                /* noresp: add a bit for the long gap */
                if (s->bit_count >= 2) {
                    uint64_t last_es = s->bits_es[s->bit_count - 1];
                    uint64_t last_width = s->bits_es[s->bit_count - 1] - s->bits_ss[s->bit_count - 1];
                    if (s->bit_count < 200) {
                        s->bits[s->bit_count] = s->pin_last;
                        s->bits_ss[s->bit_count] = last_es;
                        s->bits_es[s->bit_count] = last_es + last_width;
                        s->bit_count++;
                    }
                }
                s->draw = 1;
                s->noresp = 1;
            }
            s->sof = 0;
            s->stuff = 0;
        }

        s->ss = di_samplenum(di);
        s->pin_last = pin;

        if (s->draw) {
            bean_parse_frame(di, s);
            if (s->noresp)
                bean_reset_frame(s);
            else
                bean_reset(di);
        }
    }
}

static void bean_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder bean_c_decoder = {
    .id = "bean_c",
    .name = "BEAN(C)",
    .longname = "BEAN is a Toyota Body Electronics Area Network (C)",
    .desc = "BEAN is a Toyota Body Electronics Area Network (C implementation)",
    .license = "gplv2+",
    .channels = bean_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = bean_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = bean_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = bean_ann_rows,
    .inputs = bean_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = bean_tags,
    .num_tags = 1,
    .reset = bean_reset,
    .start = bean_start,
    .decode = bean_decode,
    .destroy = bean_destroy,
    .state_size = 0,
    .metadata = bean_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *vals_yesno[] = {
        g_variant_new_string("none"),
        g_variant_new_string("yes"),
    };
    GSList *list_yesno = NULL;
    list_yesno = g_slist_append(list_yesno, vals_yesno[0]);
    list_yesno = g_slist_append(list_yesno, vals_yesno[1]);

    bean_options[0].id = "bit_annotations";
    bean_options[0].idn = "dec_bean_opt_bit_annotations";
    bean_options[0].desc = "Bit annotations";
    bean_options[0].def = g_variant_new_string("none");
    bean_options[0].values = list_yesno;

    bean_options[1].id = "pulse_len";
    bean_options[1].idn = "dec_bean_opt_pulse_len";
    bean_options[1].desc = "Pulse length";
    bean_options[1].def = g_variant_new_string("none");
    bean_options[1].values = list_yesno;

    GSList *list_yesno2 = NULL;
    GVariant *vals_yesno2[] = {
        g_variant_new_string("none"),
        g_variant_new_string("yes"),
    };
    list_yesno2 = g_slist_append(list_yesno2, vals_yesno2[0]);
    list_yesno2 = g_slist_append(list_yesno2, vals_yesno2[1]);

    bean_options[2].id = "command";
    bean_options[2].idn = "dec_bean_opt_command";
    bean_options[2].desc = "Command";
    bean_options[2].def = g_variant_new_string("yes");
    bean_options[2].values = list_yesno2;

    GSList *list_yesno3 = NULL;
    GVariant *vals_yesno3[] = {
        g_variant_new_string("none"),
        g_variant_new_string("yes"),
    };
    list_yesno3 = g_slist_append(list_yesno3, vals_yesno3[0]);
    list_yesno3 = g_slist_append(list_yesno3, vals_yesno3[1]);

    bean_options[3].id = "all byte";
    bean_options[3].idn = "dec_bean_opt_all_byte";
    bean_options[3].desc = "All byte";
    bean_options[3].def = g_variant_new_string("yes");
    bean_options[3].values = list_yesno3;

    return &bean_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}