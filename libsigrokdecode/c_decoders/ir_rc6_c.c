/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2019 Benedikt Otto <benedikt_o@web.de>
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
#define MAX_BITS 64
#define MAX_EDGES 128
#define MAX_DELTAS 128

enum rc6_state {
    STATE_IDLE = 0,
    STATE_SYNC,
    STATE_DATA,
};

enum rc6_ann {
    ANN_BIT = 0,
    ANN_SYNC,
    ANN_STARTBIT,
    ANN_FIELD,
    ANN_TOGGLEBIT,
    ANN_ADDRESS,
    ANN_COMMAND,
    NUM_ANN,
};

typedef struct {
    uint64_t ss;
    uint64_t es;
    int width;
    int value;
} rc6_bit;

typedef struct {
    enum rc6_state state;
    uint64_t samplerate;
    uint64_t halfbit;
    int out_ann;

    int invert;
    int polarity_auto;

    uint64_t edges[MAX_EDGES];
    int num_edges;
    uint64_t deltas[MAX_DELTAS];
    int num_deltas;

    rc6_bit bits[MAX_BITS];
    int num_bits;

    int mode;
    int num_edges_counted;
} rc6_priv;

static struct srd_channel rc6_channels[] = {
    { "ir", "IR", "IR data line", 0, SRD_CHANNEL_SDATA, "dec_ir_rc6_chan_ir" },
};

static struct srd_decoder_option rc6_options_arr[1];

static const char* rc6_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "sync", "Sync" },
    { "", "startbit", "Startbit" },
    { "", "field", "Field" },
    { "", "togglebit", "Togglebit" },
    { "", "address", "Address" },
    { "", "command", "Command" },
};

static const int rc6_row_bits_classes[] = { ANN_BIT, -1 };
static const int rc6_row_fields_classes[] = { ANN_SYNC, ANN_STARTBIT, ANN_FIELD, ANN_TOGGLEBIT, ANN_ADDRESS, ANN_COMMAND, -1 };
static const struct srd_c_ann_row rc6_ann_rows[] = {
    { "bits", "Bits", rc6_row_bits_classes, 1 },
    { "fields", "Fields", rc6_row_fields_classes, 6 },
};

static const char* rc6_inputs[] = { "logic", NULL };
static const char* rc6_outputs[] = { NULL };
static const char* rc6_tags[] = { "IR", NULL };

static void rc6_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    rc6_priv *s = (rc6_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->samplerate > 0)
            s->halfbit = (uint64_t)((double)s->samplerate * 0.000889 / 2.0);
    }
}

static void rc6_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(rc6_priv)));
    }
    rc6_priv *s = (rc6_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(rc6_priv));
    s->state = STATE_IDLE;
    s->mode = 0;
}

static void rc6_start(struct srd_decoder_inst *di)
{
    rc6_priv *s = (rc6_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_rc6");

    const char *polarity = c_opt_str(di, "polarity", "auto");
    s->polarity_auto = (polarity && strcmp(polarity, "auto") == 0);

    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        s->halfbit = (uint64_t)((double)s->samplerate * 0.000889 / 2.0);
}

static void handle_bit(struct srd_decoder_inst *di, rc6_priv *s)
{
    if (s->num_bits != 6)
        return;

    /* bits[0]: sync bit (width=8, value must be 1) */
    if (s->bits[0].width == 8 && s->bits[0].value == 1) {
        c_put(di, s->bits[0].ss, s->bits[0].es, s->out_ann, ANN_SYNC,
            "Synchronisation", "Sync");
    } else {
        return;
    }

    /* bits[1]: start bit (value must be 1) */
    if (s->bits[1].value == 1) {
        c_put(di, s->bits[1].ss, s->bits[1].es, s->out_ann, ANN_STARTBIT,
            "Startbit", "Start");
    } else {
        return;
    }

    /* bits[2-4]: mode field (3 bits) */
    s->mode = 0;
    int i;
    for (i = 0; i < 3; i++)
        s->mode |= (s->bits[2 + i].value << (2 - i));

    char field_str[32];
    snprintf(field_str, sizeof(field_str), "Field: %d", s->mode);
    c_put(di, s->bits[2].ss, s->bits[4].es, s->out_ann, ANN_FIELD, field_str);

    /* bits[5]: toggle bit */
    char toggle_str[32];
    snprintf(toggle_str, sizeof(toggle_str), "Toggle: %d", s->bits[5].value);
    c_put(di, s->bits[5].ss, s->bits[5].es, s->out_ann, ANN_TOGGLEBIT, toggle_str);
}

static void handle_package(struct srd_decoder_inst *di, rc6_priv *s)
{
    /* Sync and start bits have to be 1 */
    if (s->bits[0].value == 0 || s->bits[1].value == 0)
        return;
    if (s->num_bits <= 6)
        return;

    if (s->mode == 0 && s->num_bits == 22) {
        /* Mode 0 standard: 8-bit address + 8-bit command */
        int value = 0;
        int i;
        for (i = 0; i < 8; i++)
            value |= (s->bits[6 + i].value << (7 - i));

        char addr_str[32];
        snprintf(addr_str, sizeof(addr_str), "Address: %.2X", (uint8_t)value);
        c_put(di, s->bits[6].ss, s->bits[13].es, s->out_ann, ANN_ADDRESS, addr_str);

        value = 0;
        for (i = 0; i < 8; i++)
            value |= (s->bits[14 + i].value << (7 - i));

        char cmd_str[32];
        snprintf(cmd_str, sizeof(cmd_str), "Data: %.2X", (uint8_t)value);
        c_put(di, s->bits[14].ss, s->bits[21].es, s->out_ann, ANN_COMMAND, cmd_str);

        s->num_bits = 0;
    }

    if (s->mode == 6 && s->num_bits >= 15) {
        if (s->bits[6].value == 0) {
            /* Mode 6A: short address */
            int value = 0;
            int i;
            for (i = 0; i < 8; i++)
                value |= (s->bits[6 + i].value << (7 - i));

            char addr_str[32];
            snprintf(addr_str, sizeof(addr_str), "Address: %.2X", (uint8_t)value);
            c_put(di, s->bits[6].ss, s->bits[13].es, s->out_ann, ANN_ADDRESS, addr_str);

            int num_data_bits = s->num_bits - 14;
            value = 0;
            for (i = 0; i < num_data_bits; i++)
                value |= (s->bits[14 + i].value << (num_data_bits - 1 - i));

            char cmd_str[32];
            snprintf(cmd_str, sizeof(cmd_str), "Data: %X", value);
            c_put(di, s->bits[14].ss, s->bits[s->num_bits - 1].es, s->out_ann, ANN_COMMAND, cmd_str);

            s->num_bits = 0;
        } else if (s->num_bits >= 23) {
            /* Mode 6B: long address */
            int value = 0;
            int i;
            for (i = 0; i < 16; i++)
                value |= (s->bits[6 + i].value << (15 - i));

            char addr_str[32];
            snprintf(addr_str, sizeof(addr_str), "Address: %.4X", (uint16_t)value);
            c_put(di, s->bits[6].ss, s->bits[21].es, s->out_ann, ANN_ADDRESS, addr_str);

            int num_data_bits = s->num_bits - 22;
            value = 0;
            for (i = 0; i < num_data_bits; i++)
                value |= (s->bits[22 + i].value << (num_data_bits - 1 - i));

            char cmd_str[32];
            snprintf(cmd_str, sizeof(cmd_str), "Data: %X", value);
            c_put(di, s->bits[22].ss, s->bits[s->num_bits - 1].es, s->out_ann, ANN_COMMAND, cmd_str);

            s->num_bits = 0;
        }
    }
}

static void rc6_decode(struct srd_decoder_inst *di)
{
    rc6_priv *s = (rc6_priv *)c_decoder_get_private(di);
    int value = 0;

    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            s->halfbit = (uint64_t)((double)s->samplerate * 0.000889 / 2.0);
    }
    if (s->samplerate == 0 || s->halfbit == 0)
        return;

    s->num_edges_counted = -1;
    s->invert = 0;

    while (1) {
        int ret;
        int ir;

            if (s->state == STATE_DATA)
            ret = c_wait(di, CW_E(IR_CH), CW_OR, CW_SKIP(s->halfbit * 6), CW_END);
        else
            ret = c_wait(di, CW_E(IR_CH), CW_END);
        if (ret != SRD_OK)
            return;

        ir = c_pin(di, IR_CH);

        /* Check skip timeout in DATA state */
        if (s->state == STATE_DATA) {
            if (di_matched(di) & (1ULL << 1)) {
                s->state = STATE_IDLE;
                /* Don't process this edge further, just continue */
                /* But we still need to update edges for potential sync detection */
            }
        }

        /* Add edge */
        if (s->num_edges < MAX_EDGES) {
            s->edges[s->num_edges++] = di_samplenum(di);
        } else {
            /* Shift edges array */
            int j;
            for (j = 1; j < MAX_EDGES; j++)
                s->edges[j - 1] = s->edges[j];
            s->edges[MAX_EDGES - 1] = di_samplenum(di);
        }

        if (s->num_edges < 2)
            continue;

        /* Calculate delta */
        uint64_t delta_val = s->edges[s->num_edges - 1] - s->edges[s->num_edges - 2];
        double delta_double = (double)delta_val / (double)s->halfbit;
        uint64_t delta = (uint64_t)(delta_double + 0.5);

        if (s->num_deltas < MAX_DELTAS) {
            s->deltas[s->num_deltas++] = delta;
        } else {
            int j;
            for (j = 1; j < MAX_DELTAS; j++)
                s->deltas[j - 1] = s->deltas[j];
            s->deltas[MAX_DELTAS - 1] = delta;
        }

        if (s->num_deltas < 2)
            continue;

        /* Check for sync pattern: deltas[-2:] == [6, 2] */
        if (s->deltas[s->num_deltas - 2] == 6 && s->deltas[s->num_deltas - 1] == 2) {
            s->state = STATE_SYNC;
            s->num_edges_counted = 0;
            s->num_bits = 0;

            if (s->polarity_auto) {
                value = 1;
            } else {
                const char *pol = c_opt_str(di, "polarity", "auto");
                value = (pol && strcmp(pol, "active-high") == 0) ? ir : 1 - ir;
            }

            /* Add sync bit: (edges[-3], edges[-1], 8, value) */
            if (s->num_bits < MAX_BITS && s->num_edges >= 3) {
                s->bits[s->num_bits].ss = s->edges[s->num_edges - 3];
                s->bits[s->num_bits].es = s->edges[s->num_edges - 1];
                s->bits[s->num_bits].width = 8;
                s->bits[s->num_bits].value = value;
                s->num_bits++;
            }

            s->invert = (ir == 0);

            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", value);
            c_put(di, s->bits[s->num_bits - 1].ss, s->bits[s->num_bits - 1].es,
                s->out_ann, ANN_BIT, bit_str);
        }

        /* Process data bits */
        if ((s->num_edges_counted % 2) == 0) {
            if (s->num_deltas >= 2) {
                uint64_t d2 = s->deltas[s->num_deltas - 2];
                uint64_t d1 = s->deltas[s->num_deltas - 1];

                if ((d2 == 1 || d2 == 2 || d2 == 3) &&
                    (d1 == 1 || d1 == 2 || d1 == 3 || d1 == 6)) {
                    s->state = STATE_DATA;

                    if (d2 != d1) {
                        /* Insert border between 2 bits */
                        /* Insert edge at edges[-2] + deltas[-2] * halfbit */
                        uint64_t insert_edge = s->edges[s->num_edges - 2] + d2 * s->halfbit;

                        /* Shift edges to insert */
                        if (s->num_edges < MAX_EDGES) {
                            s->edges[s->num_edges] = s->edges[s->num_edges - 1];
                            s->edges[s->num_edges - 1] = insert_edge;
                            s->num_edges++;
                        }

                        uint64_t total = d1;
                        s->deltas[s->num_deltas - 1] = d2;
                        if (s->num_deltas < MAX_DELTAS) {
                            s->deltas[s->num_deltas] = total - d2;
                            s->num_deltas++;
                        }

                        /* First bit: (edges[-4], edges[-2], deltas[-2]*2, value) */
                        if (s->num_bits < MAX_BITS && s->num_edges >= 4) {
                            s->bits[s->num_bits].ss = s->edges[s->num_edges - 4];
                            s->bits[s->num_bits].es = s->edges[s->num_edges - 2];
                            s->bits[s->num_bits].width = d2 * 2;
                            s->bits[s->num_bits].value = value;
                            s->num_bits++;
                        }

                        s->num_edges_counted++;
                    } else {
                        /* One bit spanning two half-bit periods */
                        if (s->num_bits < MAX_BITS && s->num_edges >= 3) {
                            s->bits[s->num_bits].ss = s->edges[s->num_edges - 3];
                            s->bits[s->num_bits].es = s->edges[s->num_edges - 1];
                            s->bits[s->num_bits].width = d1 * 2;
                            s->bits[s->num_bits].value = value;
                            s->num_bits++;
                        }
                    }

                    /* Output bit annotation */
                    if (s->num_bits > 0) {
                        char bit_str[16];
                        snprintf(bit_str, sizeof(bit_str), "%d", s->bits[s->num_bits - 1].value);
                        c_put(di, s->bits[s->num_bits - 1].ss, s->bits[s->num_bits - 1].es,
                            s->out_ann, ANN_BIT, bit_str);
                    }
                }
            }
        }

        /* Handle bit and package processing */
        if (s->num_bits > 0) {
            handle_bit(di, s);
            if (s->state == STATE_IDLE) {
                handle_package(di, s);
            }
        }

        /* Update value based on polarity */
        if (s->polarity_auto) {
            value = s->invert ? ir : 1 - ir;
        } else {
            const char *pol = c_opt_str(di, "polarity", "auto");
            value = (pol && strcmp(pol, "active-low") == 0) ? ir : 1 - ir;
        }

        s->num_edges_counted++;
    }
}

static void rc6_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_rc6_c_decoder = {
    .id = "ir_rc6_c",
    .name = "IR RC-6(C)",
    .longname = "RC-6 infrared remote control protocol (C)",
    .desc = "RC-6 infrared remote control protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = rc6_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = rc6_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = rc6_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = rc6_ann_rows,
    .inputs = rc6_inputs,
    .num_inputs = 1,
    .outputs = rc6_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = rc6_tags,
    .num_tags = 1,
    .metadata = rc6_metadata,
    .reset = rc6_reset,
    .start = rc6_start,
    .decode = rc6_decode,
    .destroy = rc6_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *polarity_vals[] = {
        g_variant_new_string("auto"),
        g_variant_new_string("active-low"),
        g_variant_new_string("active-high"),
    };
    GSList *polarity_list = NULL;
    polarity_list = g_slist_append(polarity_list, polarity_vals[0]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[1]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[2]);
    rc6_options_arr[0].id = "polarity";
    rc6_options_arr[0].idn = "dec_ir_rc6_opt_polarity";
    rc6_options_arr[0].desc = "Polarity";
    rc6_options_arr[0].def = g_variant_new_string("auto");
    rc6_options_arr[0].values = polarity_list;

    return &ir_rc6_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}