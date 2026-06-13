/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2017 Ryan "Izzy" Bales <izzy84075@gmail.com>
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

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IR_CH 0

enum ltto_state {
    STATE_IDLE = 0,
    STATE_PSP,
    STATE_SYNC,
    STATE_BITPAUSE,
    STATE_BIT,
};

enum ltto_ann {
    ANN_PRE_SYNC = 0,
    ANN_PRE_SYNC_PAUSE,
    ANN_SYNC,
    ANN_LONG_SYNC,
    ANN_BIT_PAUSE,
    ANN_BIT,
    ANN_SIGNATURE,
    ANN_LONG_SYNC_SIGNATURE,
    ANN_ERROR,
    NUM_ANN,
};

typedef struct {
    enum ltto_state state;
    uint64_t samplerate;
    int active;
    int out_ann;
    int out_python;

    uint64_t margin;
    uint64_t presync;
    uint64_t presyncpause;
    uint64_t sync;
    uint64_t longsync;
    uint64_t bitpause;
    uint64_t dazero;
    uint64_t daone;

    uint64_t oldedgesample;
    uint64_t newedgesample;
    int oldpinstate;
    int ir;

    uint32_t data;
    int count;
    int waslongsync;
    int lastbit;
    uint64_t packetstartsample;
} ltto_priv;

static struct srd_channel ltto_channels[] = {
    { "ir", "IR", "Demodulated IR", 0, SRD_CHANNEL_SDATA, "" },
};

static struct srd_decoder_option ltto_options_arr[1];

static const char* ltto_ann_labels[][3] = {
    { "", "pre-sync", "PRE-SYNC" },
    { "", "pre-sync-pause", "PRE-SYNC PAUSE" },
    { "", "sync", "SYNC" },
    { "", "long-sync", "LONG-SYNC" },
    { "", "bit-pause", "Bit Pause" },
    { "", "bit", "Bit" },
    { "", "signature", "Signature" },
    { "", "long-sync-signature", "Long SYNC Signature" },
    { "", "signature-error", "Error" },
};

static const int ltto_row_bits_classes[] = { ANN_PRE_SYNC, ANN_PRE_SYNC_PAUSE, ANN_SYNC, ANN_LONG_SYNC, ANN_BIT_PAUSE, ANN_BIT, -1 };
static const int ltto_row_signatures_classes[] = { ANN_SIGNATURE, ANN_LONG_SYNC_SIGNATURE, ANN_ERROR, -1 };
static const struct srd_c_ann_row ltto_ann_rows[] = {
    { "bits", "Bits", ltto_row_bits_classes, 6 },
    { "signatures", "Signatures", ltto_row_signatures_classes, 3 },
};

static const char* ltto_inputs[] = { "logic", NULL };
static const char* ltto_outputs[] = { "ir_ltto", NULL };
static const char* ltto_tags[] = { "Embedded/industrial", NULL };

static void calc_rate(ltto_priv *s)
{
    if (s->samplerate == 0)
        return;
    s->margin = (uint64_t)(s->samplerate * 0.0005) - 1;
    s->presync = (uint64_t)(s->samplerate * 0.003) - 1;
    s->presyncpause = (uint64_t)(s->samplerate * 0.006) - 1;
    s->sync = (uint64_t)(s->samplerate * 0.003) - 1;
    s->longsync = (uint64_t)(s->samplerate * 0.006) - 1;
    s->bitpause = (uint64_t)(s->samplerate * 0.002) - 1;
    s->dazero = (uint64_t)(s->samplerate * 0.001) - 1;
    s->daone = (uint64_t)(s->samplerate * 0.002) - 1;
}

static void ltto_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    ltto_priv *s = (ltto_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        calc_rate(s);
    }
}

static void ltto_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ltto_priv)));
    }
    ltto_priv *s = (ltto_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ltto_priv));
    s->state = STATE_IDLE;
    s->active = 0;
    s->ir = 1;
}

static void ltto_start(struct srd_decoder_inst *di)
{
    ltto_priv *s = (ltto_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_ltto");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "ir_ltto");

    const char *polarity = c_opt_str(di, "polarity", "active-low");
    s->active = (polarity && strcmp(polarity, "active-high") == 0) ? 1 : 0;

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        calc_rate(s);

    s->ir = c_init_pin(di, 0);
    if (s->ir == 0xFF)
        s->ir = 1;
}

static void handle_bit(ltto_priv *s, uint64_t tick)
{
    s->lastbit = -1;
    if (tick >= s->dazero - s->margin && tick < s->dazero + s->margin)
        s->lastbit = 0;
    else if (tick >= s->daone - s->margin && tick < s->daone + s->margin)
        s->lastbit = 1;

    if (s->lastbit == 0 || s->lastbit == 1) {
        s->data = (s->data << 1) | s->lastbit;
        s->count++;
    }
}

static void ltto_decode(struct srd_decoder_inst *di)
{
    ltto_priv *s = (ltto_priv *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            calc_rate(s);
    }
    if (s->samplerate == 0)
        return;

    while (1) {
        int ret;

        s->oldedgesample = s->newedgesample;
        s->oldpinstate = s->ir;

        if (s->state == STATE_BIT || s->state == STATE_BITPAUSE) {
            ret = c_wait(di, CW_E(IR_CH), CW_OR, CW_SKIP(s->bitpause + s->margin + s->margin), CW_END);
            if (ret != SRD_OK)
                return;
        } else {
            ret = c_wait(di, CW_E(IR_CH), CW_END);
            if (ret != SRD_OK)
                return;
        }

        s->ir = c_pin(di, IR_CH);
        s->newedgesample = di_samplenum(di);
        uint64_t length = s->newedgesample - s->oldedgesample;

        switch (s->state) {
        case STATE_IDLE:
            if (length >= s->presync - s->margin && length < s->presync + s->margin
                && s->oldpinstate == s->active) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_PRE_SYNC,
                    "PRE-SYNC Pulse", "PRE-SYNC", "PS");
                s->data = 0;
                s->count = 0;
                s->waslongsync = 0;
                s->packetstartsample = s->oldedgesample;
                s->state = STATE_PSP;
            }
            break;

        case STATE_PSP:
            if (length >= s->presyncpause - s->margin && length < s->presyncpause + s->margin) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_PRE_SYNC_PAUSE,
                    "PRE-SYNC Pause", "PRE-SYNC P", "PSP");
                s->state = STATE_SYNC;
            } else {
                c_put(di, s->packetstartsample, s->oldedgesample, s->out_ann, ANN_ERROR,
                    "Error", "Err", "E");
                s->state = STATE_IDLE;
            }
            break;

        case STATE_SYNC:
            if (length >= s->sync - s->margin && length < s->sync + s->margin) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_SYNC,
                    "SYNC Pulse", "SYNC P", "SP");
                s->state = STATE_BITPAUSE;
            } else if (length >= s->longsync - s->margin && length < s->longsync + s->margin) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_LONG_SYNC,
                    "Long SYNC Pulse", "Long SYNC P", "LSP");
                s->waslongsync = 1;
                s->state = STATE_BITPAUSE;
            } else {
                c_put(di, s->packetstartsample, s->oldedgesample, s->out_ann, ANN_ERROR,
                    "Error", "Err", "E");
                s->state = STATE_IDLE;
            }
            break;

        case STATE_BITPAUSE:
            if (length >= s->bitpause - s->margin && length < s->bitpause + s->margin) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_BIT_PAUSE,
                    "Bit Pause", "Bit P", "BP");
                s->state = STATE_BIT;
            } else {
                if (s->count == 0) {
                    c_put(di, s->packetstartsample, s->oldedgesample, s->out_ann, ANN_ERROR,
                        "Error", "Err", "E");
                } else {
                    if (s->waslongsync == 0) {
                        /* SHORT signature with PROTO output */
                        char str1[64], str2[64], str3[64];
                        snprintf(str1, sizeof(str1), "Signature, %d bits: 0x%03X", s->count, s->data);
                        snprintf(str2, sizeof(str2), "Sig, %d: 0x%03X", s->count, s->data);
                        snprintf(str3, sizeof(str3), "S %d: 0x%03X", s->count, s->data);
                        c_put(di, s->packetstartsample, s->oldedgesample, s->out_ann, ANN_SIGNATURE,
                            str1, str2, str3);

                        /* PROTO output: ['SHORT', count, data] */
                        
                        unsigned char py_data[32];
                        memcpy(py_data, "SHORT", 5);
                        py_data[5] = (unsigned char)s->count;
                        uint16_t d = (uint16_t)s->data;
                        memcpy(py_data + 6, &d, 2);
                        py_data[8] = 0;
                        c_proto(di, s->packetstartsample, s->oldedgesample,
                            s->out_python, "SHORT", C_U8(s->count), C_U16((uint16_t)s->data), C_U8(0), C_END);
                    } else {
                        /* LONG signature with PROTO output */
                        char str1[80], str2[80], str3[80];
                        snprintf(str1, sizeof(str1), "Signature, long SYNC, %d bits: 0x%03X", s->count, s->data);
                        snprintf(str2, sizeof(str2), "Sig, LS, %d: 0x%03X", s->count, s->data);
                        snprintf(str3, sizeof(str3), "S LS %d: 0x%03X", s->count, s->data);
                        c_put(di, s->packetstartsample, s->oldedgesample, s->out_ann, ANN_LONG_SYNC_SIGNATURE,
                            str1, str2, str3);

                        /* PROTO output: ['LONG', count, data] */
                        
                        unsigned char py_data[32];
                        memcpy(py_data, "LONG", 4);
                        py_data[4] = (unsigned char)s->count;
                        uint16_t d = (uint16_t)s->data;
                        memcpy(py_data + 5, &d, 2);
                        py_data[7] = 0;
                        c_proto(di, s->packetstartsample, s->oldedgesample,
                            s->out_python, "LONG", C_U8(s->count), C_U16((uint16_t)s->data), C_U8(0), C_END);
                    }
                }
                s->state = STATE_IDLE;
            }
            break;

        case STATE_BIT: {
            /* Check if skip timeout occurred */
            
            if (s->state == STATE_BIT) {
                /* We already used the condition builder above; check di_matched(di) */
                /* For BIT state, the condition was edge OR skip(bitpause+margin+margin) */
                /* If skip di_matched(di), treat as bitpause timeout */
                /* Actually in Python, BIT state doesn't have a separate skip condition,
                   the skip is only for BITPAUSE. Let me re-check...
                   Actually BIT/BITPAUSE both use the same wait condition with skip.
                   For BIT state, the skip timeout means the bit was too long. */
            }

            handle_bit(s, length);
            s->state = STATE_BITPAUSE;

            if (s->lastbit == -1) {
                c_put(di, s->packetstartsample, s->oldedgesample, s->out_ann, ANN_ERROR,
                    "Error", "Err", "E");
                s->state = STATE_IDLE;
            } else {
                char bit_str[16];
                snprintf(bit_str, sizeof(bit_str), "%d", s->lastbit);
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_BIT, bit_str);
            }
            break;
        }
        }
    }
}

static void ltto_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_ltto_c_decoder = {
    .id = "ir_ltto_c",
    .name = "IR LTTO(C)",
    .longname = "LTTO laser tag IR (C)",
    .desc = "A decoder for the LTTO laser tag IR protocol (C implementation)",
    .license = "unknown",
    .channels = ltto_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ltto_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ltto_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = ltto_ann_rows,
    .inputs = ltto_inputs,
    .num_inputs = 1,
    .outputs = ltto_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ltto_tags,
    .num_tags = 1,
    .metadata = ltto_metadata,
    .reset = ltto_reset,
    .start = ltto_start,
    .decode = ltto_decode,
    .destroy = ltto_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *polarity_vals[] = {
        g_variant_new_string("active-low"),
        g_variant_new_string("active-high"),
    };
    GSList *polarity_list = NULL;
    polarity_list = g_slist_append(polarity_list, polarity_vals[0]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[1]);
    ltto_options_arr[0].id = "polarity";
    ltto_options_arr[0].idn = "";
    ltto_options_arr[0].desc = "Polarity";
    ltto_options_arr[0].def = g_variant_new_string("active-low");
    ltto_options_arr[0].values = polarity_list;

    return &ir_ltto_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}