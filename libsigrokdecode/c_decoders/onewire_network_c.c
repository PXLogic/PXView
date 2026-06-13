/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Iztok Jeras <iztok.jeras@gmail.com>
 * Copyright (C) 2025 C port (v4 API)
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

/* 1-Wire network layer decoder */

enum {
    ANN_TEXT = 0,
    NUM_ANN,
};

enum ow_net_state {
    STATE_COMMAND,
    STATE_GET_ROM,
    STATE_SEARCH_ROM,
    STATE_TRANSPORT,
    STATE_COMMAND_ERROR,
};

/* ROM command table */
struct rom_cmd {
    uint8_t code;
    const char *name;
    enum ow_net_state next_state;
};

static const struct rom_cmd rom_commands[] = {
    {0x33, "Read ROM",               STATE_GET_ROM},
    {0x0f, "Conditional read ROM",   STATE_GET_ROM},
    {0xcc, "Skip ROM",               STATE_TRANSPORT},
    {0x55, "Match ROM",              STATE_GET_ROM},
    {0xf0, "Search ROM",             STATE_SEARCH_ROM},
    {0xec, "Conditional search ROM", STATE_SEARCH_ROM},
    {0x3c, "Overdrive skip ROM",     STATE_TRANSPORT},
    {0x69, "Overdrive match ROM",    STATE_GET_ROM},
    {0xa5, "Resume",                 STATE_TRANSPORT},
    {0x96, "DS2408: Disable Test Mode", STATE_GET_ROM},
};

#define NUM_ROM_COMMANDS (sizeof(rom_commands) / sizeof(rom_commands[0]))

/* Decoder private state — C_DECODER_STATE auto-generates ownet_s typedef,
 * ownet_reset (calloc), and ownet_destroy (free). */
C_DECODER_STATE(ownet, {
    enum ow_net_state state;
    int bit_cnt;
    uint8_t data;
    uint64_t ss_block;
    uint64_t es_block;
    int out_ann;
    int out_proto;

    /* Search ROM state */
    int search_phase; /* 0='P', 1='N', 2='D' */
    uint64_t data_p;
    uint64_t data_n;
})

static const char *ownet_inputs[] = {"onewire_link", NULL};
static const char *ownet_outputs[] = {"onewire_network", NULL};
static const char *ownet_tags[] = {"Embedded/industrial", NULL};

static const char *ownet_ann_labels[][3] = {
    {"text", "text", "Human-readable text"},
};

static const int ownet_row_text_classes[] = {ANN_TEXT, -1};

static const struct srd_c_ann_row ownet_ann_rows[] = {
    {"text", "Human-readable text", ownet_row_text_classes, 1},
};

static const struct rom_cmd *find_rom_command(uint8_t code)
{
    for (size_t i = 0; i < NUM_ROM_COMMANDS; i++) {
        if (rom_commands[i].code == code)
            return &rom_commands[i];
    }
    return NULL;
}

/* Data collector — faithful to Python onewire_collect() */
static int onewire_collect(ownet_s *s, int length, uint8_t val,
                           uint64_t ss, uint64_t es)
{
    if (s->bit_cnt == 0)
        s->ss_block = ss;
    s->data = (s->data & ~(1 << s->bit_cnt)) | (val << s->bit_cnt);
    s->bit_cnt++;
    if (s->bit_cnt == length) {
        s->es_block = es;
        s->data = s->data & ((1 << length) - 1);
        s->bit_cnt = 0;
        return 1;
    }
    return 0;
}

/* Search collector — faithful to Python onewire_search() */
static int onewire_search(ownet_s *s, int length, uint8_t val,
                          uint64_t ss, uint64_t es)
{
    if ((s->bit_cnt == 0) && (s->search_phase == 0))
        s->ss_block = ss;

    if (s->search_phase == 0) {
        /* Master receives an original address bit */
        s->data_p = (s->data_p & ~((uint64_t)1 << s->bit_cnt)) |
                    ((uint64_t)val << s->bit_cnt);
        s->search_phase = 1;
    } else if (s->search_phase == 1) {
        /* Master receives a complemented address bit */
        s->data_n = (s->data_n & ~((uint64_t)1 << s->bit_cnt)) |
                    ((uint64_t)val << s->bit_cnt);
        s->search_phase = 2;
    } else {
        /* Master transmits an address bit */
        s->data = (s->data & ~(1 << s->bit_cnt)) | (val << s->bit_cnt);
        s->search_phase = 0;
        s->bit_cnt++;
    }

    if (s->bit_cnt == length) {
        s->es_block = es;
        s->data_p = s->data_p & ((1ULL << length) - 1);
        s->data_n = s->data_n & ((1ULL << length) - 1);
        s->data = s->data & ((1 << length) - 1);
        s->search_phase = 0;
        s->bit_cnt = 0;
        return 1;
    }
    return 0;
}

/* --- Core decode_upper --- */

static void ownet_decode_upper(struct srd_decoder_inst *di,
    uint64_t start_sample, uint64_t end_sample,
    const char *cmd, const c_field *fields, int n_fields)
{
    ownet_s *s = (ownet_s *)c_decoder_get_private(di);
    if (!s) return;

    uint8_t val = (n_fields > 0 && fields[0].type == C_FIELD_U8) ? fields[0].u8 : 0;

    if (strcmp(cmd, "RESET/PRESENCE") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Reset/presence: %s", val ? "true" : "false");
        c_put(di, start_sample, end_sample, s->out_ann, ANN_TEXT, buf);
        c_proto(di, start_sample, end_sample, s->out_proto, "RESET/PRESENCE", C_U8(val), C_END);
        s->state = STATE_COMMAND;
        s->bit_cnt = 0;
        s->data = 0;
        s->search_phase = 0;
        return;
    }

    if (strcmp(cmd, "BIT") != 0)
        return;

    if (s->state == STATE_COMMAND) {
        if (onewire_collect(s, 8, val, start_sample, end_sample) == 0)
            return;
        const struct rom_cmd *c = find_rom_command(s->data);
        if (c) {
            char buf[256];
            snprintf(buf, sizeof(buf), "ROM command: 0x%02x '%s'", s->data, c->name);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_TEXT, buf);
            s->state = c->next_state;
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "ROM command: 0x%02x '%s'", s->data, "unrecognized");
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_TEXT, buf);
            s->state = STATE_COMMAND_ERROR;
        }
        s->bit_cnt = 0;
        s->data = 0;
    } else if (s->state == STATE_GET_ROM) {
        if (onewire_collect(s, 64, val, start_sample, end_sample) == 0)
            return;
        uint64_t rom = s->data & 0xffffffffffffffffULL;
        char buf[64];
        snprintf(buf, sizeof(buf), "ROM: 0x%016llx", (unsigned long long)rom);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_TEXT, buf);
        uint8_t rom_data[8];
        for (int i = 0; i < 8; i++)
            rom_data[i] = (rom >> (i * 8)) & 0xff;
        c_proto(di, s->ss_block, s->es_block, s->out_proto, "ROM",
                C_BYTES(rom_data, 8), C_END);
        s->state = STATE_TRANSPORT;
        s->bit_cnt = 0;
        s->data = 0;
    } else if (s->state == STATE_SEARCH_ROM) {
        if (onewire_search(s, 64, val, start_sample, end_sample) == 0)
            return;
        uint64_t rom = s->data & 0xffffffffffffffffULL;
        char buf[64];
        snprintf(buf, sizeof(buf), "ROM: 0x%016llx", (unsigned long long)rom);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_TEXT, buf);
        uint8_t rom_data[8];
        for (int i = 0; i < 8; i++)
            rom_data[i] = (rom >> (i * 8)) & 0xff;
        c_proto(di, s->ss_block, s->es_block, s->out_proto, "ROM",
                C_BYTES(rom_data, 8), C_END);
        s->state = STATE_TRANSPORT;
        s->bit_cnt = 0;
        s->data = 0;
    } else if (s->state == STATE_TRANSPORT) {
        if (onewire_collect(s, 8, val, start_sample, end_sample) == 0)
            return;
        char buf[32];
        snprintf(buf, sizeof(buf), "Data: 0x%02x", s->data);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_TEXT, buf);
        c_proto(di, s->ss_block, s->es_block, s->out_proto, "DATA", C_U8(s->data), C_END);
        s->bit_cnt = 0;
        s->data = 0;
    } else if (s->state == STATE_COMMAND_ERROR) {
        if (onewire_collect(s, 8, val, start_sample, end_sample) == 0)
            return;
        char buf[64];
        snprintf(buf, sizeof(buf), "ROM error data: 0x%02x", s->data);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_TEXT, buf);
        s->bit_cnt = 0;
        s->data = 0;
    }
}

/* --- Decoder lifecycle --- */

static void ownet_start(struct srd_decoder_inst *di)
{
    ownet_s *s = (ownet_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "onewire_network");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "onewire_network");
    s->state = STATE_COMMAND;
}

/* ---- Decoder definition (v4 API) ---- */
static struct srd_c_decoder onewire_network_c_def = {
    .id = "onewire_network_c",
    .name = "OneWire network(C)",
    .longname = "1-Wire serial communication bus (network layer)(C)",
    .desc = "1-Wire network layer: ROM commands, device addressing and enumeration. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ownet_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = ownet_ann_rows,
    .inputs = ownet_inputs,
    .num_inputs = 1,
    .outputs = ownet_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ownet_tags,
    .num_tags = 1,
    .state_size = sizeof(ownet_s),
    .reset = ownet_reset,
    .start = ownet_start,
    .decode = NULL,
    .end = NULL,
    .metadata = NULL,
    .destroy = ownet_destroy,
    .decode_upper = ownet_decode_upper,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &onewire_network_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}