/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2021 Ryan "Izzy" Bales <izzy84075@gmail.com>
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_SIGNALS = 0,
    ANN_BIT_ZERO,
    ANN_BIT_ONE,
    ANN_BIT_ERROR,
    ANN_STATE_ERROR,
    ANN_BYTE,
    ANN_BIT_COUNT,
    ANN_BIT_COUNT_ERROR,
    NUM_ANN,
};

enum sony_md_state {
    STATE_IDLE,
    STATE_PRESYNC,
    STATE_SYNC,
    STATE_DATA_BIT_HIGH,
    STATE_DATA_BIT_LOW,
};

#define CH_DATA 0

#define MAX_BIT_DATA 128

typedef struct {
    enum sony_md_state state;

    uint64_t lastedgesample;
    int lastedgestate;
    uint64_t newedgesample;
    int newedgestate;

    int playerHasData;
    int remoteHasData;
    int playerCedesBus;

    uint64_t pulselength;

    int dataBitCount;
    int expectedBitCount;

    uint64_t databitstart;
    uint64_t databitend;

    uint64_t bytestartsample;
    uint64_t byteendsample;
    int bytevalue;

    uint64_t packetstartsample;
    uint64_t packetendsample;

    /* Timing parameters */
    uint64_t resetCycles, resetMin, resetMax;
    uint64_t presyncCycles, presyncMin, presyncMax;
    uint64_t presyncDelayMin, presyncDelayMax;
    uint64_t syncMin, syncMax;
    uint64_t bitDelayHighMin;
    uint64_t dataLongMin, dataLongMax;
    uint64_t dataShortMin, dataShortMax;

    /* Sync data for proto output: 3 pulses, each [ss, es] */
    uint64_t syncData[3][2];
    int syncDataCount;

    /* Bit data for proto output */
    uint64_t bitDataStart;
    uint64_t bitDataEnd;
    int bitDataCount;
    /* Per-bit: [start, middle, end, value] */
    uint64_t bitData[MAX_BIT_DATA][4];

    int marginpct;
    int out_ann;
    int out_proto;
    uint64_t samplerate;
} sony_md_state;

static struct srd_channel sony_md_channels[] = {
    {"data", "data", "Data stream", 0, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_decoder_option sony_md_options[] = {
    {"marginpct", "dec_sony_md_opt_marginpct", "Error margin %", NULL, NULL},
};

static const char *sony_md_ann_labels[][3] = {
    {"", "signals", "Signals"},
    {"", "bit-zero", "0"},
    {"", "bit-one", "1"},
    {"", "bit-error", "Unknown half-cycle"},
    {"", "state-error", "State error"},
    {"", "byte", "Byte value"},
    {"", "bit-count", "Message bit count"},
    {"", "bit-count-error", "Expected multiple of 8 bits"},
};

static const int sony_md_row_signalling_classes[] = {ANN_SIGNALS, -1};
static const int sony_md_row_raw_bits_classes[] = {ANN_BIT_ZERO, ANN_BIT_ONE, -1};
static const int sony_md_row_byte_values_classes[] = {ANN_BYTE, -1};
static const int sony_md_row_messages_classes[] = {ANN_BIT_COUNT, -1};
static const int sony_md_row_errors_classes[] = {ANN_BIT_ERROR, ANN_STATE_ERROR, ANN_BIT_COUNT_ERROR, -1};

static const struct srd_c_ann_row sony_md_ann_rows[] = {
    {"signalling", "Signalling", sony_md_row_signalling_classes, 1},
    {"raw-bits", "Raw Bits", sony_md_row_raw_bits_classes, 2},
    {"byte-values", "Byte Values", sony_md_row_byte_values_classes, 1},
    {"Messages", "Messages", sony_md_row_messages_classes, 1},
    {"errors", "Errors", sony_md_row_errors_classes, 3},
};

static const char *sony_md_inputs[] = {"logic"};
static const char *sony_md_outputs[] = {"sony_md"};
static const char *sony_md_tags[] = {""};

static int in_range(uint64_t val, uint64_t min, uint64_t max)
{
    return val >= min && val < max;
}

static void sony_md_calc_timing(sony_md_state *s)
{
    double margin = s->marginpct * 0.01;
    s->resetCycles = (uint64_t)(s->samplerate * 40.0 / 1000.0);
    s->resetMin = (uint64_t)(s->resetCycles * (1.0 - margin));
    s->resetMax = (uint64_t)(s->resetCycles * (1.0 + margin));
    s->presyncCycles = (uint64_t)(s->samplerate * 1100.0 / 1000000.0);
    s->presyncMin = (uint64_t)(s->presyncCycles * (1.0 - margin));
    s->presyncMax = (uint64_t)(s->presyncCycles * (1.0 + margin));
    s->presyncDelayMin = (uint64_t)(s->samplerate * 800.0 / 1000000.0);
    s->presyncDelayMax = (uint64_t)(s->samplerate * 1500.0 / 1000000.0);
    s->syncMin = (uint64_t)(s->samplerate * 20.0 / 1000000.0);
    s->syncMax = (uint64_t)(s->presyncCycles * (1.0 + margin));
    uint64_t bitDelayHighIdeal = (uint64_t)(s->samplerate * 32.5 / 1000000.0);
    s->bitDelayHighMin = (uint64_t)(bitDelayHighIdeal * (1.0 - margin));
    s->dataLongMin = (uint64_t)(s->samplerate * 101.0 / 1000000.0);
    s->dataLongMax = (uint64_t)(s->samplerate * 280.0 / 1000000.0);
    s->dataShortMin = (uint64_t)(s->samplerate * 10.0 / 1000000.0);
    s->dataShortMax = (uint64_t)(s->samplerate * 100.0 / 1000000.0);
}

static void sony_md_return_to_idle(sony_md_state *s)
{
    s->state = STATE_IDLE;
    s->playerHasData = 0;
    s->remoteHasData = 0;
    s->playerCedesBus = 0;
    s->dataBitCount = 0;
    s->expectedBitCount = 16;
    s->syncDataCount = 0;
    s->bitDataCount = 0;
}

static void sony_md_put_packet_proto(struct srd_decoder_inst *di, sony_md_state *s, int clean_end)
{
    if (s->out_proto < 0)
        return;

    /* Build proto output: syncData + bitData + cleanEnd */
    /* Format: sync_data_count(1B) + 3 pulses * [ss(8B), es(8B)] +
     *         bit_start(8B) + bit_end(8B) + bit_count(1B) +
     *         N * [start(8B), middle(8B), end(8B), value(1B)] +
     *         clean_end(1B) */
    int num_sync = s->syncDataCount;
    int num_bits = s->bitDataCount;
    int n_fields = 1 + num_sync * 16 + 8 + 8 + 1 + num_bits * 25 + 1;
    unsigned char *proto_data = (unsigned char *)g_malloc(n_fields);
    int pos = 0;

    proto_data[pos++] = (unsigned char)num_sync;
    for (int i = 0; i < num_sync; i++) {
        for (int b = 0; b < 8; b++)
            proto_data[pos++] = (unsigned char)(s->syncData[i][0] >> (8 * b));
        for (int b = 0; b < 8; b++)
            proto_data[pos++] = (unsigned char)(s->syncData[i][1] >> (8 * b));
    }

    for (int b = 0; b < 8; b++)
        proto_data[pos++] = (unsigned char)(s->bitDataStart >> (8 * b));
    for (int b = 0; b < 8; b++)
        proto_data[pos++] = (unsigned char)(s->bitDataEnd >> (8 * b));
    proto_data[pos++] = (unsigned char)num_bits;

    for (int i = 0; i < num_bits; i++) {
        for (int b = 0; b < 8; b++)
            proto_data[pos++] = (unsigned char)(s->bitData[i][0] >> (8 * b));
        for (int b = 0; b < 8; b++)
            proto_data[pos++] = (unsigned char)(s->bitData[i][1] >> (8 * b));
        for (int b = 0; b < 8; b++)
            proto_data[pos++] = (unsigned char)(s->bitData[i][2] >> (8 * b));
        proto_data[pos++] = (unsigned char)s->bitData[i][3];
    }

    proto_data[pos++] = (unsigned char)(clean_end ? 1 : 0);

    c_proto(di, s->packetstartsample, s->packetendsample,
                        s->out_proto, "PACKET", C_BYTES(proto_data, pos), C_END);
    g_free(proto_data);
}

static void sony_md_put_packet_bit_count(struct srd_decoder_inst *di, sony_md_state *s)
{
    sony_md_put_packet_proto(di, s, 1);

    char buf[64];
    snprintf(buf, sizeof(buf), "Message, %d bits", s->dataBitCount);
    c_put(di, s->packetstartsample, s->packetendsample, s->out_ann, ANN_BIT_COUNT, buf);
}

static void sony_md_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(sony_md_state)));
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(sony_md_state));
    s->out_ann = -1;
    s->out_proto = -1;
    s->expectedBitCount = 16;
}

static void sony_md_start(struct srd_decoder_inst *di)
{
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sony_md");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "sony_md");

    s->marginpct = (int)c_opt_int(di, "marginpct", 20);
    s->samplerate = c_samplerate(di);
    if (s->samplerate > 0)
        sony_md_calc_timing(s);
}

static void sony_md_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (s->samplerate > 0)
            sony_md_calc_timing(s);
    }
}

static void sony_md_decode(struct srd_decoder_inst *di)
{
    sony_md_state *s = (sony_md_state *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;

    sony_md_calc_timing(s);

    while (1) {
        s->lastedgesample = s->newedgesample;
        s->lastedgestate = s->newedgestate;

        int ret = c_wait(di, CW_E(CH_DATA), CW_END);
        if (ret != SRD_OK)
            return;

        s->newedgestate = c_pin(di, CH_DATA);
        s->newedgesample = di_samplenum(di);
        s->pulselength = s->newedgesample - s->lastedgesample;

        if (s->state == STATE_IDLE) {
            /* Low or high */
            if (s->lastedgestate == 0 && s->newedgestate == 1) {
                /* Now high, was low */
                if (in_range(s->pulselength, s->resetMin, s->resetMax)) {
                    s->packetstartsample = s->lastedgesample;
                    c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_SIGNALS,
                              "Reset+Presync pulse", "Reset", "R");
                    s->syncDataCount = 0;
                    s->state = STATE_PRESYNC;
                } else if (in_range(s->pulselength, s->presyncMin, s->presyncMax)) {
                    s->packetstartsample = s->lastedgesample;
                    if (s->syncDataCount < 3) {
                        s->syncData[s->syncDataCount][0] = s->lastedgesample;
                        s->syncData[s->syncDataCount][1] = s->newedgesample;
                        s->syncDataCount++;
                    }
                    c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_SIGNALS,
                              "Presync pulse", "Presync", "PS");
                    s->state = STATE_PRESYNC;
                } else if (in_range(s->pulselength, s->dataShortMin, s->dataShortMax)) {
                    c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_BIT_ERROR,
                              "Unexpected data bit");
                } else if (in_range(s->pulselength, s->dataLongMin, s->dataLongMax)) {
                    c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_BIT_ERROR,
                              "Unexpected data bit");
                }
            }
        } else if (s->state == STATE_PRESYNC) {
            /* Now low, was high */
            if (in_range(s->pulselength, s->presyncDelayMin, s->presyncDelayMax)) {
                if (s->syncDataCount < 3) {
                    s->syncData[s->syncDataCount][0] = s->lastedgesample;
                    s->syncData[s->syncDataCount][1] = s->newedgesample;
                    s->syncDataCount++;
                }
                c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_SIGNALS,
                          "Presync delay", "PSD");
                s->state = STATE_SYNC;
            } else {
                c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_BIT_ERROR,
                          "Error");
                sony_md_return_to_idle(s);
            }
        } else if (s->state == STATE_SYNC) {
            /* Now high, was low */
            if (in_range(s->pulselength, s->syncMin, s->syncMax)) {
                if (s->syncDataCount < 3) {
                    s->syncData[s->syncDataCount][0] = s->lastedgesample;
                    s->syncData[s->syncDataCount][1] = s->newedgesample;
                    s->syncDataCount++;
                }
                c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_SIGNALS,
                          "Sync pulse", "S");
                s->bytevalue = 0;
                s->bytestartsample = s->newedgesample;
                s->bitDataStart = s->newedgesample;
                s->bitDataCount = 0;
                s->dataBitCount = 0;
                s->state = STATE_DATA_BIT_HIGH;
            } else {
                c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_BIT_ERROR,
                          "Error");
                sony_md_return_to_idle(s);
            }
        } else if (s->state == STATE_DATA_BIT_HIGH) {
            /* Now low, was high */
            s->databitstart = s->lastedgesample;
            s->state = STATE_DATA_BIT_LOW;
        } else if (s->state == STATE_DATA_BIT_LOW) {
            /* Now high, was low */
            if (in_range(s->pulselength, s->dataShortMin, s->dataShortMax)) {
                /* Bit = 1 */
                s->databitend = s->newedgesample;
                s->dataBitCount++;

                if (s->bitDataCount < MAX_BIT_DATA) {
                    s->bitData[s->bitDataCount][0] = s->databitstart;
                    s->bitData[s->bitDataCount][1] = s->lastedgesample;
                    s->bitData[s->bitDataCount][2] = s->databitend;
                    s->bitData[s->bitDataCount][3] = 1;
                    s->bitDataCount++;
                }

                c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_BIT_ONE, "1");

                if (s->dataBitCount == 5) {
                    c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_SIGNALS,
                              "Remote HAS data to send", "RY");
                    s->remoteHasData = 1;
                }
                if (s->dataBitCount == 9) {
                    c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_SIGNALS,
                              "Player has NO data to send", "PN");
                }
                if (s->dataBitCount == 13) {
                    c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_SIGNALS,
                              "Player CEDES bus to Remote", "RDB");
                    s->playerCedesBus = 1;
                    if (s->playerCedesBus) {
                        if (!s->remoteHasData) {
                            c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_BIT_COUNT_ERROR,
                                      "Player ceded bus to Remote without Remote asking!");
                        }
                        s->expectedBitCount = 115;
                    } else if (s->playerHasData && !s->playerCedesBus) {
                        s->expectedBitCount = 104;
                    }
                }

                if (s->dataBitCount == s->expectedBitCount) {
                    s->packetendsample = s->newedgesample;
                    s->bitDataEnd = s->newedgesample;
                    sony_md_put_packet_bit_count(di, s);
                    c_put(di, s->newedgesample, s->newedgesample, s->out_ann, ANN_SIGNALS,
                              "Message End", "St");
                    sony_md_return_to_idle(s);
                } else {
                    s->state = STATE_DATA_BIT_HIGH;
                }
            } else if (in_range(s->pulselength, s->dataLongMin, s->dataLongMax)) {
                /* Bit = 0 */
                s->databitend = s->newedgesample;
                s->dataBitCount++;

                if (s->bitDataCount < MAX_BIT_DATA) {
                    s->bitData[s->bitDataCount][0] = s->databitstart;
                    s->bitData[s->bitDataCount][1] = s->lastedgesample;
                    s->bitData[s->bitDataCount][2] = s->databitend;
                    s->bitData[s->bitDataCount][3] = 0;
                    s->bitDataCount++;
                }

                c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_BIT_ZERO, "0");

                if (s->dataBitCount == 5) {
                    c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_SIGNALS,
                              "Remote has NO data to send", "RN");
                }
                if (s->dataBitCount == 9) {
                    c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_SIGNALS,
                              "Player HAS data to send", "PY");
                    s->playerHasData = 1;
                }
                if (s->dataBitCount == 13) {
                    c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_SIGNALS,
                              "Player does NOT cede bus to Remote", "PDB");
                    if (s->playerCedesBus) {
                        if (!s->remoteHasData) {
                            c_put(di, s->databitstart, s->databitend, s->out_ann, ANN_BIT_COUNT_ERROR,
                                      "Player ceded bus to Remote without Remote asking!");
                        }
                        s->expectedBitCount = 115;
                    } else if (s->playerHasData && !s->playerCedesBus) {
                        s->expectedBitCount = 104;
                    }
                }

                if (s->dataBitCount == s->expectedBitCount) {
                    s->packetendsample = s->newedgesample;
                    s->bitDataEnd = s->newedgesample;
                    sony_md_put_packet_bit_count(di, s);
                    c_put(di, s->newedgesample, s->newedgesample, s->out_ann, ANN_SIGNALS,
                              "Message End", "St");
                    sony_md_return_to_idle(s);
                } else {
                    s->state = STATE_DATA_BIT_HIGH;
                }
            } else {
                c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_BIT_ERROR,
                          "Error");
                sony_md_return_to_idle(s);
            }
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "State error: %d", s->state);
            c_put(di, s->lastedgesample, s->newedgesample, s->out_ann, ANN_STATE_ERROR, buf);
            sony_md_return_to_idle(s);
        }
    }
}

static void sony_md_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder sony_md_c_decoder = {
    .id = "sony_md_c",
    .name = "Sony MD Remote(C)",
    .longname = "Sony MD LCD Remote (C)",
    .desc = "Sony MD LCD Remote protocol decoder (C implementation)",
    .license = "unknown",
    .channels = sony_md_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = sony_md_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = sony_md_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = sony_md_ann_rows,
    .inputs = sony_md_inputs,
    .num_inputs = 1,
    .outputs = sony_md_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = sony_md_tags,
    .num_tags = 1,
    .reset = sony_md_reset,
    .start = sony_md_start,
    .decode = sony_md_decode,
    .destroy = sony_md_destroy,
    .state_size = 0,
    .metadata = sony_md_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    sony_md_options[0].def = g_variant_new_int64(20);
    return &sony_md_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}