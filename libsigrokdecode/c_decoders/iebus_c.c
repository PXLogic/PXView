/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2023 Maciej Grela <enki@fsck.pl>
 * Copyright (C) 2023 ALIENTEK(正点原子) <39035605@qq.com>
 * Copyright (C) 2025 C port (v4 API)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CH_BUS 0

enum {
    ANN_START_BIT = 0,
    ANN_BIT,
    ANN_PARITY,
    ANN_ACK,
    ANN_BROADCAST,
    ANN_MADDR,
    ANN_SADDR,
    ANN_CONTROL,
    ANN_DATALEN,
    ANN_BYTE,
    ANN_WARNING,
    NUM_ANN,
};

enum {
    CMD_READ_STATUS = 0x00,
    CMD_READ_DATA_LOCK = 0x03,
    CMD_READ_LOCK_ADDR_LO = 0x04,
    CMD_READ_LOCK_ADDR_HI = 0x05,
    CMD_READ_STATUS_UNLOCK = 0x06,
    CMD_READ_DATA = 0x07,
    CMD_WRITE_CMD_LOCK = 0x0a,
    CMD_WRITE_DATA_LOCK = 0x0b,
    CMD_WRITE_CMD = 0x0e,
    CMD_WRITE_DATA = 0x0f,
};

C_DECODER_STATE(iebus, {
    uint64_t samplerate;
    int bus_polarity;    /* 0=idle-low, 1=idle-high */
    int ignore_nak;      /* 0=Disabled, 1=Enabled */
    int broadcast_bit;

    uint64_t bits_begin;
    uint64_t bits_end;

    int out_ann;
    int out_python;
});

static const char *cmd_names[] = {
    "READ_STATUS",         /* 0x00 */
    NULL, NULL,
    "READ_DATA_LOCK",      /* 0x03 */
    "READ_LOCK_ADDR_LO",   /* 0x04 */
    "READ_LOCK_ADDR_HI",   /* 0x05 */
    "READ_STATUS_UNLOCK",  /* 0x06 */
    "READ_DATA",           /* 0x07 */
    NULL, NULL,
    "WRITE_CMD_LOCK",      /* 0x0a */
    "WRITE_DATA_LOCK",     /* 0x0b */
    NULL, NULL,
    "WRITE_CMD",           /* 0x0e */
    "WRITE_DATA",          /* 0x0f */
};

static int popcount32(uint32_t v)
{
    int count = 0;
    while (v) {
        count++;
        v &= v - 1;
    }
    return count;
}

/* Read a single bit from the bus */
static int read_bit(struct srd_decoder_inst *di, iebus_s *s)
{
    /* Wait for sync edge */
    int ret;
    if (s->bus_polarity == 0)
        ret = c_wait(di, CW_R(CH_BUS), CW_END);
    else
        ret = c_wait(di, CW_F(CH_BUS), CW_END);
    if (ret != SRD_OK)
        return -1;

    uint64_t bit_start = di_samplenum(di);

    /* Sample 27us after sync edge */
    uint64_t skip_count = (uint64_t)(27e-6 * (double)s->samplerate);
    ret = c_wait(di, CW_SKIP(skip_count), CW_END);
    if (ret != SRD_OK)
        return -1;

    uint8_t pin = c_pin(di, CH_BUS);
    int bit = (pin + 1) % 2;

    /* Invert for idle-high */
    if (s->bus_polarity == 1)
        bit = (bit + 1) % 2;

    /* Assume 33us bit length */
    uint64_t bit_end = bit_start + (uint64_t)(33e-6 * (double)s->samplerate);

    char bit_str[16];
    snprintf(bit_str, sizeof(bit_str), "%d", bit);
    c_put(di, bit_start, bit_end, s->out_ann, ANN_BIT, bit_str);

    s->bits_begin = bit_start;
    s->bits_end = bit_end;

    return bit;
}

/* Read n bits from the bus, return value MSB first */
static int read_bits(struct srd_decoder_inst *di, iebus_s *s, int n, uint16_t *value)
{
    uint16_t v = 0;
    uint64_t first_begin = 0;
    s->bits_begin = 0;
    s->bits_end = 0;

    for (int i = 0; i < n; i++) {
        int bit = read_bit(di, s);
        if (bit < 0)
            return -1;
        v = (v << 1) | bit;
        if (i == 0)
            first_begin = s->bits_begin;
    }

    if (value)
        *value = v;
    s->bits_begin = first_begin;
    return 0;
}

/* Read value and return with ss/es */
static int read_value(struct srd_decoder_inst *di, iebus_s *s,
                      int num_bits, uint16_t *value, uint64_t *ss, uint64_t *es)
{
    if (read_bits(di, s, num_bits, value) < 0)
        return -1;
    if (ss) *ss = s->bits_begin;
    if (es) *es = s->bits_end;
    return 0;
}

/* Read broadcast bit */
static int read_broadcast_bit(struct srd_decoder_inst *di, iebus_s *s)
{
    int broadcast_bit = read_bit(di, s);
    if (broadcast_bit < 0)
        return -1;

    if (broadcast_bit == 1)
        c_put(di, s->bits_begin, s->bits_end, s->out_ann, ANN_BROADCAST, "Unicast", "Uni", "U");
    else
        c_put(di, s->bits_begin, s->bits_end, s->out_ann, ANN_BROADCAST, "Broadcast", "Bro", "B");

    return broadcast_bit;
}

/* Read ACK/NAK bit */
static int read_ack_bit(struct srd_decoder_inst *di, iebus_s *s)
{
    int ack_bit = read_bit(di, s);
    if (ack_bit < 0)
        return -1;

    if (s->broadcast_bit == 1) {
        if (s->ignore_nak == 1)
            ack_bit = 0;

        if (ack_bit == 0) {
            c_put(di, s->bits_begin, s->bits_end, s->out_ann, ANN_ACK, "ACK", "A");
        } else if (ack_bit == 1) {
            c_put(di, s->bits_begin, s->bits_end, s->out_ann, ANN_ACK, "NAK", "N");
        }
    }

    return ack_bit;
}

/* Read parity bit and check */
static int read_parity_bit(struct srd_decoder_inst *di, iebus_s *s, int value)
{
    int parity_bit = read_bit(di, s);
    if (parity_bit < 0)
        return -1;

    c_put(di, s->bits_begin, s->bits_end, s->out_ann, ANN_PARITY, "Parity", "Par", "P");

    int expected_parity = popcount32((uint32_t)value) % 2;
    if (expected_parity != parity_bit) {
        c_put(di, s->bits_begin, s->bits_end, s->out_ann, ANN_WARNING, "Parity error");
    }

    return parity_bit;
}

/* Read header (start bit + broadcast bit) */
static int read_header(struct srd_decoder_inst *di, iebus_s *s,
                       int *start_bit, int *broadcast_bit_out,
                       uint64_t *ss, uint64_t *es)
{
    /* Wait for start edge */
    int ret;
    if (s->bus_polarity == 0)
        ret = c_wait(di, CW_R(CH_BUS), CW_END);
    else
        ret = c_wait(di, CW_F(CH_BUS), CW_END);
    if (ret != SRD_OK)
        return -1;

    uint64_t start_ss = di_samplenum(di);

    /* Wait for opposite edge */
    if (s->bus_polarity == 0)
        ret = c_wait(di, CW_F(CH_BUS), CW_END);
    else
        ret = c_wait(di, CW_R(CH_BUS), CW_END);
    if (ret != SRD_OK)
        return -1;

    uint64_t start_es = di_samplenum(di);

    /* Check start bit width >= 100us */
    double duration = (double)(start_es - start_ss) / (double)s->samplerate;
    if (duration < 100e-6) {
        c_put(di, start_ss, start_es, s->out_ann, ANN_WARNING,
            "Startbit too short", "Too short");
        if (start_bit) *start_bit = 0;
        if (ss) *ss = start_ss;
        if (es) *es = start_es;
        return 0;
    }

    c_put(di, start_ss, start_es, s->out_ann, ANN_START_BIT, "Start bit", "Start", "S");

    /* Read broadcast bit */
    int bb = read_broadcast_bit(di, s);
    if (bb < 0)
        return -1;

    if (start_bit) *start_bit = 1;
    if (broadcast_bit_out) *broadcast_bit_out = bb;
    if (ss) *ss = start_ss;
    if (es) *es = s->bits_end;
    return 0;
}

/* Handle data bytes */
static int handle_data_bytes(struct srd_decoder_inst *di, iebus_s *s, int n_fields)
{
    while (n_fields > 0) {
        uint16_t b;
        uint64_t ss = 0, es = 0;
        
        if (read_value(di, s, 8, &b, &ss, &es) < 0)
            return -1;

        char db_str[32], db_short[16];
        snprintf(db_str, sizeof(db_str), "Data: 0x%02x", b);
        snprintf(db_short, sizeof(db_short), "0x%02x", b);
        c_put(di, ss, es, s->out_ann, ANN_BYTE, db_str, db_short);

        int parity_bit = read_parity_bit(di, s, b);
        if (parity_bit < 0)
            return -1;

        int ack_bit = read_ack_bit(di, s);
        if (ack_bit < 0)
            return -1;

        /* Protocol output */
        c_proto(di, ss, es, s->out_python, "DATA_BYTE",
                C_U8(b), C_U8(parity_bit), C_U8(ack_bit),
                C_U64(ss), C_U64(es), C_END);

        n_fields--;

        /* NAK condition */
        if (s->broadcast_bit == 1 && ack_bit == 1)
            break;
    }
    return 0;
}

static struct srd_channel iebus_channels[] = {
    {"bus", "BUS", "Bus input", 0, SRD_CHANNEL_SDATA, "dec_iebus_chan_bus"},
};

static struct srd_decoder_option iebus_options[] = {
    {"mode", NULL, "Mode", NULL, NULL},
    {"bus_polarity", NULL, "Bus polarity", NULL, NULL},
    {"ignore_nak", NULL, "Ignore NAK condition", NULL, NULL},
};

static const char *iebus_ann_labels[][3] = {
    {"", "start-bit", "Start bit"},
    {"", "bit", "Bit"},
    {"", "parity", "Parity"},
    {"", "ack", "Acknowledge"},
    {"", "broadcast", "Broadcast flag"},
    {"", "maddr", "Master address"},
    {"", "saddr", "Slave address"},
    {"", "control", "Control"},
    {"", "datalen", "Data Length"},
    {"", "byte", "Data Byte"},
    {"", "warning", "Warning"},
};

static const int iebus_row_bits_classes[] = {ANN_START_BIT, ANN_BIT, ANN_PARITY, ANN_ACK};
static const int iebus_row_fields_classes[] = {ANN_BROADCAST, ANN_MADDR, ANN_SADDR, ANN_CONTROL, ANN_DATALEN, ANN_BYTE};
static const int iebus_row_warnings_classes[] = {ANN_WARNING};
static const struct srd_c_ann_row iebus_ann_rows[] = {
    {"bits", "Bits", iebus_row_bits_classes, 4},
    {"fields", "Raw Fields", iebus_row_fields_classes, 6},
    {"warnings", "Warnings", iebus_row_warnings_classes, 1},
};

static const char *iebus_inputs[] = {"logic"};
static const char *iebus_outputs[] = {"iebus"};
static const char *iebus_tags[] = {"Automotive"};

static void iebus_start(struct srd_decoder_inst *di)
{
    iebus_s *s = (iebus_s *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "iebus");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "iebus");

    const char *pol = c_opt_str(di, "bus_polarity", "idle-low");
    s->bus_polarity = (strcmp(pol, "idle-high") == 0) ? 1 : 0;

    const char *nak = c_opt_str(di, "ignore_nak", "Disabled");
    s->ignore_nak = (strcmp(nak, "Enabled") == 0) ? 1 : 0;

    s->samplerate = c_samplerate(di);
}

static void iebus_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    iebus_s *s = (iebus_s *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void iebus_decode(struct srd_decoder_inst *di)
{
    iebus_s *s = (iebus_s *)c_decoder_get_private(di);

    /* Fallback samplerate */
    if (s->samplerate == 0)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0)
        return;

    while (1) {
        int start_bit, broadcast_bit;
        uint64_t ss = 0, es = 0;

        if (read_header(di, s, &start_bit, &broadcast_bit, &ss, &es) < 0)
            return;

        if (!start_bit)
            continue;

        s->broadcast_bit = broadcast_bit;

        /* Output HEADER */
        c_proto(di, ss, es, s->out_python, "HEADER", C_U8(broadcast_bit), C_END);

        /* Master address (12 bits) */
        uint16_t master_addr;
        if (read_value(di, s, 12, &master_addr, &ss, &es) < 0)
            return;

        char ma_str[48], ma_short[16];
        snprintf(ma_str, sizeof(ma_str), "Master: 0x%03x", master_addr);
        snprintf(ma_short, sizeof(ma_short), "0x%03x", master_addr);
        c_put(di, ss, es, s->out_ann, ANN_MADDR, ma_str, ma_short);

        int parity_bit = read_parity_bit(di, s, master_addr);
        if (parity_bit < 0)
            return;

        c_proto(di, ss, es, s->out_python, "MASTER ADDRESS",
                C_U16(master_addr), C_U8(parity_bit), C_END);

        /* Slave address (12 bits) */
        uint16_t slave_addr;
        if (read_value(di, s, 12, &slave_addr, &ss, &es) < 0)
            return;

        char sa_str[48], sa_short[16];
        snprintf(sa_str, sizeof(sa_str), "Slave: 0x%03x", slave_addr);
        snprintf(sa_short, sizeof(sa_short), "0x%03x", slave_addr);
        c_put(di, ss, es, s->out_ann, ANN_SADDR, sa_str, sa_short);

        parity_bit = read_parity_bit(di, s, slave_addr);
        if (parity_bit < 0)
            return;

        int ack_bit = read_ack_bit(di, s);
        if (ack_bit < 0)
            return;

        c_proto(di, ss, es, s->out_python, "SLAVE ADDRESS",
                C_U16(slave_addr), C_U8(parity_bit), C_U8(ack_bit), C_END);

        if (s->broadcast_bit == 1 && ack_bit == 1) {
            c_proto(di, s->bits_begin, s->bits_end, s->out_python, "NAK", C_END);
            continue;
        }

        /* Control bits (4 bits) */
        uint16_t control;
        if (read_value(di, s, 4, &control, &ss, &es) < 0)
            return;

        parity_bit = read_parity_bit(di, s, control);
        if (parity_bit < 0)
            return;

        ack_bit = read_ack_bit(di, s);
        if (ack_bit < 0)
            return;

        const char *ctrl_name = NULL;
        if (control <= 0x0f && cmd_names[control])
            ctrl_name = cmd_names[control];

        if (ctrl_name) {
            char ctrl_str[64], ctrl_short[32];
            snprintf(ctrl_str, sizeof(ctrl_str), "Control: %s", ctrl_name);
            snprintf(ctrl_short, sizeof(ctrl_short), "%s", ctrl_name);
            c_put(di, ss, es, s->out_ann, ANN_CONTROL, ctrl_str, ctrl_short);

            c_proto(di, ss, es, s->out_python, "CONTROL",
                    C_STR(ctrl_name), C_U8(parity_bit), C_U8(ack_bit), C_END);
        } else {
            char ctrl_str[32], ctrl_short[16];
            snprintf(ctrl_str, sizeof(ctrl_str), "Control: 0x%02x", control);
            snprintf(ctrl_short, sizeof(ctrl_short), "0x%02x", control);
            c_put(di, ss, es, s->out_ann, ANN_CONTROL, ctrl_str, ctrl_short);

            c_proto(di, ss, es, s->out_python, "CONTROL",
                    C_U8(control), C_U8(parity_bit), C_U8(ack_bit), C_END);
        }

        if (s->broadcast_bit == 1 && ack_bit == 1) {
            c_proto(di, s->bits_begin, s->bits_end, s->out_python, "NAK", C_END);
            continue;
        }

        /* Data length (8 bits) */
        uint16_t n_fields;
        if (read_value(di, s, 8, &n_fields, &ss, &es) < 0)
            return;

        parity_bit = read_parity_bit(di, s, n_fields);
        if (parity_bit < 0)
            return;

        if (n_fields == 0)
            n_fields = 256;

        char dl_str[32], dl_short[16], dl_tiny[8];
        snprintf(dl_str, sizeof(dl_str), "Data Length: %d", n_fields);
        snprintf(dl_short, sizeof(dl_short), "%d", n_fields);
        snprintf(dl_tiny, sizeof(dl_tiny), "Len");
        c_put(di, ss, es, s->out_ann, ANN_DATALEN, dl_str, dl_short, dl_tiny);

        if (n_fields > 128) {
            c_put(di, ss, es, s->out_ann, ANN_WARNING,
                "Message too long, mode 2 allows only for 128 bytes maximum",
                "Message too long", "Too long");
        }

        ack_bit = read_ack_bit(di, s);
        if (ack_bit < 0)
            return;

        c_proto(di, ss, es, s->out_python, "DATA LENGTH",
                C_U16(n_fields), C_U8(parity_bit), C_U8(ack_bit), C_END);

        if (s->broadcast_bit == 1 && ack_bit == 1) {
            c_proto(di, s->bits_begin, s->bits_end, s->out_python, "NAK", C_END);
            continue;
        }

        /* Data bytes */
        handle_data_bytes(di, s, n_fields);
    }
}

static struct srd_c_decoder iebus_c_def = {
    .id = "iebus_c",
    .name = "IEBus(C)",
    .longname = "Inter-Equipment Bus (C)",
    .desc = "Inter-Equipment Bus is an automotive communication bus used in Toyota and Honda vehicles (C implementation)",
    .license = "gplv3+",
    .channels = iebus_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = iebus_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = iebus_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = iebus_ann_rows,
    .inputs = iebus_inputs,
    .num_inputs = 1,
    .outputs = iebus_outputs,
    .num_outputs = 1,
    .tags = iebus_tags,
    .num_tags = 1,
    .state_size = sizeof(iebus_s),
    .reset = iebus_reset,
    .start = iebus_start,
    .decode = iebus_decode,
    .metadata = iebus_metadata,
    .destroy = iebus_destroy,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    iebus_options[0].def = g_variant_new_string("Mode 2");
    {
        GVariant *v0 = g_variant_new_string("Mode 2");
        GSList *vals = g_slist_append(NULL, v0);
        iebus_options[0].values = vals;
    }
    iebus_options[1].def = g_variant_new_string("idle-low");
    {
        GVariant *v0 = g_variant_new_string("idle-low");
        GVariant *v1 = g_variant_new_string("idle-high");
        GSList *vals = g_slist_append(NULL, v0);
        vals = g_slist_append(vals, v1);
        iebus_options[1].values = vals;
    }
    iebus_options[2].def = g_variant_new_string("Disabled");
    {
        GVariant *v0 = g_variant_new_string("Disabled");
        GVariant *v1 = g_variant_new_string("Enabled");
        GSList *vals = g_slist_append(NULL, v0);
        vals = g_slist_append(vals, v1);
        iebus_options[2].values = vals;
    }
    return &iebus_c_def;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}