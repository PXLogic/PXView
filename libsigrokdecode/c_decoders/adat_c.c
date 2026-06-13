/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020 Hans Baier <hansfbaier@gmail.com>
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BIT = 0,
    ANN_SYNC,
    ANN_USER,
    ANN_NIBBLE,
    ANN_ERROR,
    ANN_CHANNEL,
    ANN_USER_DATA,
    ANN_CHANNEL_0,
    ANN_CHANNEL_1,
    ANN_CHANNEL_2,
    ANN_CHANNEL_3,
    ANN_CHANNEL_4,
    ANN_CHANNEL_5,
    ANN_CHANNEL_6,
    ANN_CHANNEL_7,
    NUM_ANN,
};

enum {
    STATE_SYNC = 0,
    STATE_USER_BITS,
    STATE_CHANNEL_DATA,
};

typedef struct {
    uint64_t samplerate;
    double bit_time;
    int bit_time_int;
    int sample_display_hex;   /* 0=decimal, 1=hexadecimal */
    int annotations_mode;     /* 0=intra-frame, 1=per-frame, 2=both */

    /* Signal buffer (ring buffer) */
    int signal[512];
    uint64_t times[512];
    int signal_len;

    /* Decode state */
    int state;
    int channel_no;
    int nibble_no;
    uint32_t channel_data;
    uint64_t channel_start_time;
    uint32_t all_channels_data[8];
    uint64_t frame_start_time;
    uint32_t frame_user_data;

    int out_ann;
} adat_priv;

static struct srd_channel adat_channels[] = {
    {"adat", "ADAT", "ADAT data line", 0, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option adat_options[] = {
    {"samplerate",      NULL, "audio sample rate",              NULL, NULL},
    {"sample_display",  NULL, "How to display the channel samples", NULL, NULL},
    {"annotations",     NULL, "Which set of annotations to display", NULL, NULL},
};

static const char *adat_ann_labels[][3] = {
    {"", "bit",             "bit"},
    {"", "sync",            "SYNC pad"},
    {"", "user-bits",       "user bits"},
    {"", "nibble",          "nibbles"},
    {"", "error",           "error"},
    {"", "channel",         "channel data"},
    {"", "frame-user-data", "frame user data"},
    {"", "channel-0",       "channel 0 data"},
    {"", "channel-1",       "channel 1 data"},
    {"", "channel-2",       "channel 2 data"},
    {"", "channel-3",       "channel 3 data"},
    {"", "channel-4",       "channel 4 data"},
    {"", "channel-5",       "channel 5 data"},
    {"", "channel-6",       "channel 6 data"},
    {"", "channel-7",       "channel 7 data"},
};

static const int adat_row_bits_classes[] = {ANN_BIT, -1};
static const int adat_row_nibbles_classes[] = {ANN_NIBBLE, ANN_ERROR, -1};
static const int adat_row_fields_classes[] = {ANN_SYNC, ANN_USER, ANN_CHANNEL, -1};
static const int adat_row_user_data_classes[] = {ANN_USER_DATA, -1};
static const int adat_row_ch0_classes[] = {ANN_CHANNEL_0, -1};
static const int adat_row_ch1_classes[] = {ANN_CHANNEL_1, -1};
static const int adat_row_ch2_classes[] = {ANN_CHANNEL_2, -1};
static const int adat_row_ch3_classes[] = {ANN_CHANNEL_3, -1};
static const int adat_row_ch4_classes[] = {ANN_CHANNEL_4, -1};
static const int adat_row_ch5_classes[] = {ANN_CHANNEL_5, -1};
static const int adat_row_ch6_classes[] = {ANN_CHANNEL_6, -1};
static const int adat_row_ch7_classes[] = {ANN_CHANNEL_7, -1};

static const struct srd_c_ann_row adat_ann_rows[] = {
    {"bits",       "Bits",              adat_row_bits_classes,       1},
    {"nibbles",    "Nibbles",           adat_row_nibbles_classes,    2},
    {"fields",     "Fields",            adat_row_fields_classes,     3},
    {"user-data",  "Frame User Data",   adat_row_user_data_classes,  1},
    {"channel0",   "Channel 0 Data",    adat_row_ch0_classes,        1},
    {"channel1",   "Channel 1 Data",    adat_row_ch1_classes,        1},
    {"channel2",   "Channel 2 Data",    adat_row_ch2_classes,        1},
    {"channel3",   "Channel 3 Data",    adat_row_ch3_classes,        1},
    {"channel4",   "Channel 4 Data",    adat_row_ch4_classes,        1},
    {"channel5",   "Channel 5 Data",    adat_row_ch5_classes,        1},
    {"channel6",   "Channel 6 Data",    adat_row_ch6_classes,        1},
    {"channel7",   "Channel 7 Data",    adat_row_ch7_classes,        1},
};

static const char *adat_inputs[] = {"logic"};
static const char *adat_tags[] = {"Audio"};

static int32_t sign_extend_24bit(uint32_t x)
{
    if (x & 0x800000)
        return -(int32_t)(0x800000 - (x & 0x7fffff));
    return (int32_t)x;
}

static uint32_t bits_to_int(const int *bits, int count)
{
    uint32_t result = 0;
    for (int i = 0; i < count; i++)
        result = (result << 1) | bits[i];
    return result;
}

static void look_for_sync_pad(struct srd_decoder_inst *di, adat_priv *s, uint64_t samplenum)
{
    (void)samplenum;
    (void)samplenum;
    if (s->signal_len < 11)
        return;

    /* Check for sync pattern: 1,0,0,0,0,0,0,0,0,0,0 */
    if (s->signal[0] == 1 && s->signal[1] == 0 && s->signal[2] == 0 &&
        s->signal[3] == 0 && s->signal[4] == 0 && s->signal[5] == 0 &&
        s->signal[6] == 0 && s->signal[7] == 0 && s->signal[8] == 0 &&
        s->signal[9] == 0 && s->signal[10] == 0) {

        if (s->annotations_mode != 1) {  /* not per-frame only */
            uint64_t es = s->times[9] + (uint64_t)(2 * s->bit_time + 0.5);
            c_put(di, s->times[0], es, s->out_ann, ANN_SYNC, "SYNC", "S");
        }
        s->frame_start_time = s->times[0];

        /* Remove parsed bits */
        int remove = 11;
        for (int i = 0; i < s->signal_len - remove; i++) {
            s->signal[i] = s->signal[i + remove];
            s->times[i] = s->times[i + remove];
        }
        s->signal_len -= remove;
        s->state = STATE_USER_BITS;
    } else {
        /* Throw away one bit, keep looking */
        for (int i = 0; i < s->signal_len - 1; i++) {
            s->signal[i] = s->signal[i + 1];
            s->times[i] = s->times[i + 1];
        }
        s->signal_len--;
    }
}

static void decode_user_bits(struct srd_decoder_inst *di, adat_priv *s, uint64_t samplenum)
{
    (void)samplenum;
    (void)samplenum;
    if (s->signal_len < 5)
        return;

    /* First bit must be 1 (4b/5b encoding) */
    if (s->signal[0] != 1) {
        c_put(di, s->times[0], s->times[1], s->out_ann, ANN_ERROR, "ERROR", "ERR", "E");
        /* Remove one bit */
        for (int i = 0; i < s->signal_len - 1; i++) {
            s->signal[i] = s->signal[i + 1];
            s->times[i] = s->times[i + 1];
        }
        s->signal_len--;
        s->state = STATE_SYNC;
        return;
    }

    int user_data_bits[4];
    for (int i = 0; i < 4; i++)
        user_data_bits[i] = s->signal[i + 1];

    s->frame_user_data = bits_to_int(user_data_bits, 4);

    if (s->annotations_mode != 1) {  /* not per-frame only */
        char content[32];
        snprintf(content, sizeof(content), "0b%d%d%d%d",
                 user_data_bits[0], user_data_bits[1], user_data_bits[2], user_data_bits[3]);

        char t1[64], t2[64], t3[48], t4[8];
        snprintf(t1, sizeof(t1), "USER DATA: %s", content);
        snprintf(t2, sizeof(t2), "USER %s", content);
        snprintf(t3, sizeof(t3), "U %s", content);
        snprintf(t4, sizeof(t4), "U");
        const char *txts[] = {t1, t2, t3, t4, NULL};
        struct srd_c_annotation ann;
        ann.ann_class = ANN_USER;
        ann.ann_type = 0;
        ann.ann_text = (char **)txts;
        c_decoder_put(di, s->times[0], s->times[4] + s->bit_time_int, s->out_ann, &ann);

        char hex_str[8];
        snprintf(hex_str, sizeof(hex_str), "0x%x", s->frame_user_data);
        char hex_str2[8];
        snprintf(hex_str2, sizeof(hex_str2), "%x", s->frame_user_data);
        const char *nib_txts[] = {hex_str, hex_str2, NULL};
        struct srd_c_annotation ann2;
        ann2.ann_class = ANN_NIBBLE;
        ann2.ann_type = 0;
        ann2.ann_text = (char **)nib_txts;
        c_decoder_put(di, s->times[1], s->times[4] + s->bit_time_int, s->out_ann, &ann2);
    }

    /* Remove 5 bits */
    for (int i = 0; i < s->signal_len - 5; i++) {
        s->signal[i] = s->signal[i + 5];
        s->times[i] = s->times[i + 5];
    }
    s->signal_len -= 5;
    s->state = STATE_CHANNEL_DATA;
}

static void decode_channel_data(struct srd_decoder_inst *di, adat_priv *s, uint64_t samplenum)
{
    (void)samplenum;
    (void)samplenum;
    if (s->signal_len < 5)
        return;

    /* First bit must be 1 (4b/5b encoding) */
    if (s->signal[0] != 1) {
        c_put(di, s->times[0], s->times[1], s->out_ann, ANN_ERROR, "ERROR", "ERR", "E");
        for (int i = 0; i < s->signal_len - 1; i++) {
            s->signal[i] = s->signal[i + 1];
            s->times[i] = s->times[i + 1];
        }
        s->signal_len--;
        s->channel_no = 0;
        s->channel_data = 0;
        s->nibble_no = 0;
        s->state = STATE_SYNC;
        return;
    }

    int nibble_bits[4];
    for (int i = 0; i < 4; i++)
        nibble_bits[i] = s->signal[i + 1];

    uint32_t nibble = bits_to_int(nibble_bits, 4);

    if (s->nibble_no == 0)
        s->channel_start_time = s->times[0];

    s->channel_data |= nibble << (20 - (s->nibble_no * 4));
    s->nibble_no++;

    if (s->annotations_mode != 1) {  /* not per-frame only */
        char hex_str[8];
        snprintf(hex_str, sizeof(hex_str), "0x%x", nibble);
        char hex_str2[8];
        snprintf(hex_str2, sizeof(hex_str2), "%x", nibble);
        const char *nib_txts[] = {hex_str, hex_str2, NULL};
        struct srd_c_annotation ann;
        ann.ann_class = ANN_NIBBLE;
        ann.ann_type = 0;
        ann.ann_text = (char **)nib_txts;
        c_decoder_put(di, s->times[1], s->times[4] + s->bit_time_int, s->out_ann, &ann);
    }

    /* Remove 5 bits */
    for (int i = 0; i < s->signal_len - 5; i++) {
        s->signal[i] = s->signal[i + 5];
        s->times[i] = s->times[i + 5];
    }
    s->signal_len -= 5;

    /* Data for one channel is complete (6 nibbles = 24bit) */
    if (s->nibble_no == 6) {
        if (s->annotations_mode != 1) {  /* not per-frame only */
            char content[32];
            snprintf(content, sizeof(content), "0x%06x", s->channel_data);

            char t1[64], t2[48], t3[48], t4[16], t5[8];
            snprintf(t1, sizeof(t1), "Channel %d: %s", s->channel_no, content);
            snprintf(t2, sizeof(t2), "Ch%d: %s", s->channel_no, content);
            snprintf(t3, sizeof(t3), "Ch%d: %s", s->channel_no, content + 2);
            snprintf(t4, sizeof(t4), "Ch%d", s->channel_no);
            snprintf(t5, sizeof(t5), "%d", s->channel_no);
            const char *ch_txts[] = {t1, t2, t3, t4, t5, NULL};
            struct srd_c_annotation ann;
            ann.ann_class = ANN_CHANNEL;
            ann.ann_type = 0;
            ann.ann_text = (char **)ch_txts;
            c_decoder_put(di, s->channel_start_time, di_samplenum(di) + s->bit_time_int, s->out_ann, &ann);
        }

        s->all_channels_data[s->channel_no] = s->channel_data;
        s->channel_no++;
        s->channel_data = 0;
        s->nibble_no = 0;
    }

    /* After receiving all 8 channels, output per-frame annotations */
    if (s->channel_no == 8) {
        if (s->annotations_mode != 0) {  /* not intra-frame only */
            for (int ch = 0; ch < 8; ch++) {
                char hex_str[16];
                snprintf(hex_str, sizeof(hex_str), "0x%06x", s->all_channels_data[ch]);
                int32_t signed_val = sign_extend_24bit(s->all_channels_data[ch]);
                char dec_str[16];
                snprintf(dec_str, sizeof(dec_str), "%+d", signed_val);

                const char *val_str = s->sample_display_hex ? hex_str : dec_str;
                char val_str2[16];
                snprintf(val_str2, sizeof(val_str2), "%s", val_str);
                /* Remove 0x prefix for terse */
                const char *terse = s->sample_display_hex ? hex_str + 2 : dec_str;

                const char *ch_txts[] = {val_str, terse, NULL};
                struct srd_c_annotation ann;
                ann.ann_class = ANN_CHANNEL_0 + ch;
                ann.ann_type = 0;
                ann.ann_text = (char **)ch_txts;
                c_decoder_put(di, s->frame_start_time, di_samplenum(di) + s->bit_time_int, s->out_ann, &ann);
            }

            /* Frame user data */
            char bin_str[32];
            snprintf(bin_str, sizeof(bin_str), "0b");
            for (int b = 3; b >= 0; b--)
                snprintf(bin_str + strlen(bin_str), sizeof(bin_str) - strlen(bin_str),
                         "%d", (s->frame_user_data >> b) & 1);
            char bin_str2[32];
            snprintf(bin_str2, sizeof(bin_str2), "%s", bin_str + 2);
            const char *ud_txts[] = {bin_str, bin_str2, NULL};
            struct srd_c_annotation ann;
            ann.ann_class = ANN_USER_DATA;
            ann.ann_type = 0;
            ann.ann_text = (char **)ud_txts;
            c_decoder_put(di, s->frame_start_time, di_samplenum(di) + s->bit_time_int, s->out_ann, &ann);
        }

        s->channel_no = 0;
        s->state = STATE_SYNC;
    }
}

static void adat_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(adat_priv)));
    }
    adat_priv *s = (adat_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(adat_priv));
    s->out_ann = -1;
    s->state = STATE_SYNC;
}

static void adat_start(struct srd_decoder_inst *di)
{
    adat_priv *s = (adat_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "adat");

    int audio_samplerate = (int)c_opt_int(di, "samplerate", 48000);

    const char *sample_display = c_opt_str(di, "sample_display", "decimal");
    s->sample_display_hex = (strcmp(sample_display, "hexadecimal") == 0) ? 1 : 0;

    const char *annotations = c_opt_str(di, "annotations", "both");
    if (strcmp(annotations, "intra-frame") == 0)
        s->annotations_mode = 0;
    else if (strcmp(annotations, "per-frame") == 0)
        s->annotations_mode = 1;
    else
        s->annotations_mode = 2;

    if (s->samplerate > 0) {
        s->bit_time = (double)s->samplerate / (256.0 * audio_samplerate);
        s->bit_time_int = (int)(s->bit_time + 0.5);
    }
}

static void adat_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    adat_priv *s = (adat_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        int audio_samplerate = (int)c_opt_int(di, "samplerate", 48000);
        if (audio_samplerate > 0) {
            s->bit_time = (double)value / (256.0 * audio_samplerate);
            s->bit_time_int = (int)(s->bit_time + 0.5);
        }
    }
}

static void adat_decode(struct srd_decoder_inst *di)
{
    adat_priv *s = (adat_priv *)c_decoder_get_private(di);
    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate == 0)
            return;
        int audio_samplerate = (int)c_opt_int(di, "samplerate", 48000);
        if (audio_samplerate > 0) {
            s->bit_time = (double)s->samplerate / (256.0 * audio_samplerate);
            s->bit_time_int = (int)(s->bit_time + 0.5);
        }
    }

    uint64_t last_time = 0;

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        uint64_t now = di_samplenum(di);
        uint64_t diff = now - last_time;
        int num_bits = (int)((double)diff / s->bit_time + 0.5);

        /* NRZI decode: fill signal buffer */
        for (int i = 0; i < num_bits && s->signal_len < 512; i++) {
            uint64_t t = last_time + (uint64_t)(s->bit_time * i + 0.5);
            int bit = (i == 0) ? 1 : 0;  /* NRZI: first bit is 1, rest are 0 */
            s->signal[s->signal_len] = bit;
            s->times[s->signal_len] = t;
            s->signal_len++;

            if (s->annotations_mode != 1) {  /* not per-frame only */
                char bit_str[16];
                snprintf(bit_str, sizeof(bit_str), "%d", bit);
                c_put(di, t, t + s->bit_time_int, s->out_ann, ANN_BIT, bit_str);
            }
        }

        /* Process signal buffer based on state */
        if (s->state == STATE_SYNC) {
            while (s->state == STATE_SYNC && s->signal_len >= 11)
                look_for_sync_pad(di, s, di_samplenum(di));
        } else if (s->state == STATE_USER_BITS) {
            decode_user_bits(di, s, di_samplenum(di));
        } else if (s->state == STATE_CHANNEL_DATA) {
            decode_channel_data(di, s, di_samplenum(di));
        }

        last_time = now;
    }
}

static void adat_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder adat_c_decoder = {
    .id = "adat_c",
    .name = "ADAT(C)",
    .longname = "ADAT lightpipe decoder(C)",
    .desc = "Decodes the ADAT protocol (C implementation)",
    .license = "gplv2+",
    .channels = adat_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = adat_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = adat_ann_labels,
    .num_annotation_rows = 12,
    .annotation_rows = adat_ann_rows,
    .inputs = adat_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = adat_tags,
    .num_tags = 1,
    .reset = adat_reset,
    .start = adat_start,
    .decode = adat_decode,
    .destroy = adat_destroy,
    .state_size = 0,
    .metadata = adat_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    adat_options[0].def = g_variant_new_int64(48000);
    GSList *sr_vals = NULL;
    sr_vals = g_slist_append(sr_vals, g_variant_new_int64(44100));
    sr_vals = g_slist_append(sr_vals, g_variant_new_int64(48000));
    sr_vals = g_slist_append(sr_vals, g_variant_new_int64(88200));
    sr_vals = g_slist_append(sr_vals, g_variant_new_int64(96000));
    adat_options[0].values = sr_vals;

    adat_options[1].def = g_variant_new_string("decimal");
    GSList *sd_vals = NULL;
    sd_vals = g_slist_append(sd_vals, g_variant_new_string("decimal"));
    sd_vals = g_slist_append(sd_vals, g_variant_new_string("hexadecimal"));
    adat_options[1].values = sd_vals;

    adat_options[2].def = g_variant_new_string("both");
    GSList *ann_vals = NULL;
    ann_vals = g_slist_append(ann_vals, g_variant_new_string("intra-frame"));
    ann_vals = g_slist_append(ann_vals, g_variant_new_string("per-frame"));
    ann_vals = g_slist_append(ann_vals, g_variant_new_string("both"));
    adat_options[2].values = ann_vals;

    return &adat_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}