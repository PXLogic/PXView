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
#define MAX_DATA_BITS 256

enum recoil_state {
    STATE_IDLE = 0,
    STATE_SYNCING,
    STATE_DATA,
};

enum recoil_ann {
    ANN_SYNC = 0,
    ANN_SYNC_PAUSE,
    ANN_BIT,
    ANN_PACKET,
    NUM_ANN,
};

typedef struct {
    enum recoil_state state;
    uint64_t samplerate;
    int active;
    int out_ann;

    uint64_t margin;
    uint64_t sync;
    uint64_t syncpause;
    uint64_t dazero;
    uint64_t daone;
    uint64_t dathreshold;
    uint64_t daminimum;
    uint64_t damaximum;

    uint64_t oldedgesample;
    uint64_t newedgesample;
    int oldpinstate;
    int ir;

    char data[MAX_DATA_BITS + 1];
    int count;
    int lastbit;
    uint64_t packetstartsample;
} recoil_priv;

static struct srd_channel recoil_channels[] = {
    { "ir", "IR", "Demodulated IR", 0, SRD_CHANNEL_SDATA, "" },
};

static struct srd_decoder_option recoil_options_arr[1];

static const char* recoil_ann_labels[][3] = {
    { "", "sync", "SYNC" },
    { "", "sync-pause", "SYNC PAUSE" },
    { "", "bit", "Bit" },
    { "", "packet", "Packet" },
};

static const int recoil_row_bits_classes[] = { ANN_SYNC, ANN_SYNC_PAUSE, ANN_BIT, -1 };
static const int recoil_row_packets_classes[] = { ANN_PACKET, -1 };
static const struct srd_c_ann_row recoil_ann_rows[] = {
    { "bits", "Bits", recoil_row_bits_classes, 3 },
    { "packets", "Packet", recoil_row_packets_classes, 1 },
};

static const char* recoil_inputs[] = { "logic", NULL };
static const char* recoil_outputs[] = { "ir_recoil", NULL };
static const char* recoil_tags[] = { "Embedded/industrial", NULL };

static void calc_rate(recoil_priv *s)
{
    if (s->samplerate == 0)
        return;
    s->margin = (uint64_t)(s->samplerate * 0.0002) - 1;
    s->sync = (uint64_t)(s->samplerate * 0.0033) - 1;
    s->syncpause = (uint64_t)(s->samplerate * 0.0015) - 1;
    s->dazero = (uint64_t)(s->samplerate * 0.0004) - 1;
    s->daone = (uint64_t)(s->samplerate * 0.0008) - 1;
    s->dathreshold = (uint64_t)(s->samplerate * 0.00059) - 1;
    s->daminimum = (uint64_t)(s->samplerate * 0.0002) - 1;
    s->damaximum = (uint64_t)(s->samplerate * 0.0012) - 1;
}

static void recoil_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    recoil_priv *s = (recoil_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        calc_rate(s);
    }
}

static void recoil_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(recoil_priv)));
    }
    recoil_priv *s = (recoil_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(recoil_priv));
    s->state = STATE_IDLE;
    s->active = 0;
    s->ir = 1;
}

static void recoil_start(struct srd_decoder_inst *di)
{
    recoil_priv *s = (recoil_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_recoil");

    const char *polarity = c_opt_str(di, "polarity", "active-low");
    s->active = (polarity && strcmp(polarity, "active-high") == 0) ? 1 : 0;

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        calc_rate(s);

    s->ir = c_init_pin(di, 0);
    if (s->ir == 0xFF)
        s->ir = 1;
}

static void handle_bit(recoil_priv *s, uint64_t tick)
{
    s->lastbit = -1;
    if (tick >= s->daminimum && tick < s->dathreshold)
        s->lastbit = 0;
    else if (tick >= s->dathreshold && tick < s->damaximum)
        s->lastbit = 1;

    if (s->lastbit == 0 || s->lastbit == 1) {
        if (s->count < MAX_DATA_BITS) {
            s->data[s->count] = '0' + s->lastbit;
            s->count++;
            s->data[s->count] = '\0';
        }
    }
}

static void recoil_decode(struct srd_decoder_inst *di)
{
    recoil_priv *s = (recoil_priv *)c_decoder_get_private(di);
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

        if (s->state == STATE_DATA) {
            ret = c_wait(di, CW_E(IR_CH), CW_OR, CW_SKIP(s->damaximum + s->margin), CW_END);
            if (ret != SRD_OK)
                return;

            if (di_matched(di) & (1ULL << 1)) {
                /* skip timeout */
                s->ir = c_pin(di, IR_CH);
            } else {
                s->ir = c_pin(di, IR_CH);
            }
        } else {
            ret = c_wait(di, CW_E(IR_CH), CW_END);
            if (ret != SRD_OK)
                return;

            s->ir = c_pin(di, IR_CH);
        }

        s->newedgesample = di_samplenum(di);
        uint64_t length = s->newedgesample - s->oldedgesample;

        switch (s->state) {
        case STATE_IDLE:
            if (length >= s->sync - s->margin && length < s->sync + s->margin
                && s->oldpinstate == s->active) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_SYNC,
                    "SYNC Pulse", "SYNC", "S");
                s->data[0] = '\0';
                s->count = 0;
                s->packetstartsample = s->oldedgesample;
                s->state = STATE_SYNCING;
            }
            break;

        case STATE_SYNCING:
            if (length >= s->syncpause - s->margin && length < s->syncpause + s->margin) {
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_SYNC_PAUSE,
                    "SYNC Pause", "SYNC P", "SP");
                s->state = STATE_DATA;
            } else {
                /* sync pause timeout, output packet */
                if (s->count > 0) {
                    char str1[300], str2[300], str3[300];
                    snprintf(str1, sizeof(str1), "Packet, %d bits: 0b%s", s->count, s->data);
                    snprintf(str2, sizeof(str2), "Pack, %d: 0b%s", s->count, s->data);
                    snprintf(str3, sizeof(str3), "P %d: 0b%s", s->count, s->data);
                    c_put(di, s->packetstartsample, s->oldedgesample + 1, s->out_ann, ANN_PACKET,
                        str1, str2, str3);
                }
                s->state = STATE_IDLE;
            }
            break;

        case STATE_DATA: {
            /* Check if skip timeout occurred */
            if (di_matched(di) & (1ULL << 1)) {
                /* timeout - output packet */
                if (s->count > 0) {
                    char str1[300], str2[300], str3[300];
                    snprintf(str1, sizeof(str1), "Packet, %d bits: 0b%s", s->count, s->data);
                    snprintf(str2, sizeof(str2), "Pack, %d: 0b%s", s->count, s->data);
                    snprintf(str3, sizeof(str3), "P %d: 0b%s", s->count, s->data);
                    c_put(di, s->packetstartsample, s->oldedgesample + 1, s->out_ann, ANN_PACKET,
                        str1, str2, str3);
                }
                s->state = STATE_IDLE;
                break;
            }

            handle_bit(s, length);
            if (s->lastbit == 0 || s->lastbit == 1) {
                char bit_str[16];
                snprintf(bit_str, sizeof(bit_str), "%d", s->lastbit);
                c_put(di, s->oldedgesample, s->newedgesample, s->out_ann, ANN_BIT, bit_str);
            } else {
                /* bit error, output packet */
                if (s->count > 0) {
                    char str1[300], str2[300], str3[300];
                    snprintf(str1, sizeof(str1), "Packet, %d bits: 0b%s", s->count, s->data);
                    snprintf(str2, sizeof(str2), "Pack, %d: 0b%s", s->count, s->data);
                    snprintf(str3, sizeof(str3), "P %d: 0b%s", s->count, s->data);
                    c_put(di, s->packetstartsample, s->oldedgesample + 1, s->out_ann, ANN_PACKET,
                        str1, str2, str3);
                }
                s->state = STATE_IDLE;
            }
            break;
        }
        }
    }
}

static void recoil_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_recoil_c_decoder = {
    .id = "ir_recoil_c",
    .name = "IR Recoil(C)",
    .longname = "Recoil laser tag IR (C)",
    .desc = "A decoder for the Recoil laser tag IR protocol (C implementation)",
    .license = "unknown",
    .channels = recoil_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = recoil_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = recoil_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = recoil_ann_rows,
    .inputs = recoil_inputs,
    .num_inputs = 1,
    .outputs = recoil_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = recoil_tags,
    .num_tags = 1,
    .metadata = recoil_metadata,
    .reset = recoil_reset,
    .start = recoil_start,
    .decode = recoil_decode,
    .destroy = recoil_destroy,
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
    recoil_options_arr[0].id = "polarity";
    recoil_options_arr[0].idn = "";
    recoil_options_arr[0].desc = "Polarity";
    recoil_options_arr[0].def = g_variant_new_string("active-low");
    recoil_options_arr[0].values = polarity_list;

    return &ir_recoil_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}