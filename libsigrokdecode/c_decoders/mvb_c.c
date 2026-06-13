/*
 * This file is part of the PXView project.
 *
 * Copyright (C) 2024 DreamSourceLab <info@dreamsourcelab.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

#define PREAMBLE_MASTER 0x2C715   /* 0b101100011100010101 */
#define PREAMBLE_SLAVE  0x2A8E3   /* 0b101010100011100011 */
#define PREAMBLE_LENGTH 18
#define PREAMBLE_MASK   0x3FFFF   /* 18-bit mask */
#define MVB_CLOCK_RATE  3000000ULL /* 3 MHz */

enum mvb_ann {
    ANN_MASTER_PREAMBLE = 0,
    ANN_SLAVE_PREAMBLE,
    ANN_MASTER_DATA,
    ANN_F_CODE,
    ANN_SLAVE_DATA,
    ANN_CRC,
    ANN_CRC_ERROR,
    ANN_BIT,
    ANN_ADDR,
    NUM_ANN,
};

enum mvb_state {
    STATE_FIND_START,
    STATE_DECODING,
};

static const char *F_codes[] = {
    "PD 2B", "PD 4B", "PD 8B", "PD 16B", "PD 32B",
    "reserved", "reserved", "reserved",
    "Master transfer", "General event",
    "reserved", "reserved",
    "MD", "Group event", "Single event", "Device status"
};

typedef struct {
    int state;
    uint64_t matching_header_ticks;
    int received_master_header;
    int received_slave_header;
    int last_tick;
    int is_even_tick;
    uint8_t decoded_buffer[512];
    int decoded_len;
    uint64_t frame_data_begin;
    uint64_t mvb_samples_per_bit;
    uint64_t samples_per_tick;
    uint64_t sample_begin;
    uint64_t sample_end;
    uint64_t samplerate;
    int out_ann;
} mvb_priv;

static struct srd_channel mvb_channels[] = {
    { "mvb", "MVB", "TTL from RS485", 0, SRD_CHANNEL_SDATA, NULL },
};

static const char *mvb_ann_labels[][3] = {
    { "", "master_preamble", "Master preamble" },
    { "", "slave_preamble", "Slave preamble" },
    { "", "master_data", "Master data" },
    { "", "f_code", "Function code" },
    { "", "slave_data", "Slave data" },
    { "", "crc", "CRC" },
    { "", "crc_error", "CRC Error" },
    { "", "bit", "Bit" },
    { "", "addr", "Address" },
};

static const int mvb_row_bits_classes[] = { ANN_MASTER_PREAMBLE, ANN_SLAVE_PREAMBLE, ANN_BIT, -1 };
static const int mvb_row_crcs_classes[] = { ANN_CRC, -1 };
static const int mvb_row_data_classes[] = { ANN_MASTER_DATA, ANN_SLAVE_DATA, -1 };
static const int mvb_row_fcodes_classes[] = { ANN_F_CODE, ANN_ADDR, -1 };
static const int mvb_row_errors_classes[] = { ANN_CRC_ERROR, -1 };

static const struct srd_c_ann_row mvb_ann_rows[] = {
    { "bits", "Bits", mvb_row_bits_classes, 3 },
    { "crcs", "Check sequence", mvb_row_crcs_classes, 1 },
    { "ma-sl-data", "Data", mvb_row_data_classes, 2 },
    { "f_codes", "Function code", mvb_row_fcodes_classes, 2 },
    { "errors", "Decoding errors", mvb_row_errors_classes, 1 },
};

static const char *mvb_inputs[] = { "logic" };
static const char *mvb_tags[] = { "Frame" };

/* Polynomial: 11100101 (0xE5) */
static const uint8_t crc_poly[] = {1, 1, 1, 0, 0, 1, 0, 1};
#define CRC_POLY_LEN 8

static int check_check_sequence(const uint8_t *frame_bits, int total_bits)
{
    int data_len = total_bits - 8;
    if (data_len <= 0) return 0;

    /* Create appended_data = data + 7 zeros */
    int appended_len = data_len + 7;
    uint8_t *appended = (uint8_t *)malloc(appended_len);
    if (!appended) return 0;
    memcpy(appended, frame_bits, data_len);
    memset(appended + data_len, 0, 7);

    /* Polynomial division to get 7-bit remainder (matches Python mod2div) */
    uint8_t *tmp = (uint8_t *)malloc(CRC_POLY_LEN);
    if (!tmp) { free(appended); return 0; }
    memcpy(tmp, appended, CRC_POLY_LEN);

    int pick = CRC_POLY_LEN;
    while (pick < appended_len) {
        if (tmp[0] == 1) {
            for (int i = 0; i < CRC_POLY_LEN - 1; i++)
                tmp[i] = tmp[i + 1] ^ crc_poly[i + 1];
        } else {
            for (int i = 0; i < CRC_POLY_LEN - 1; i++)
                tmp[i] = tmp[i + 1];
        }
        tmp[CRC_POLY_LEN - 1] = appended[pick];
        pick++;
    }

    /* Final step */
    uint8_t remainder[7];
    if (tmp[0] == 1) {
        for (int i = 0; i < CRC_POLY_LEN - 1; i++)
            remainder[i] = tmp[i + 1] ^ crc_poly[i + 1];
    } else {
        for (int i = 0; i < CRC_POLY_LEN - 1; i++)
            remainder[i] = tmp[i + 1];
    }

    free(tmp);
    free(appended);

    /* Compute parity of (data + 7-bit-remainder) */
    int p = 0;
    for (int i = 0; i < data_len; i++)
        p ^= frame_bits[i];
    for (int i = 0; i < 7; i++)
        p ^= remainder[i];

    /* Concatenate inverted(remainder + parity) */
    uint8_t calculated[8];
    for (int i = 0; i < 7; i++)
        calculated[i] = 1 ^ remainder[i];
    calculated[7] = 1 ^ p;

    /* Compare with received check bits */
    for (int i = 0; i < 8; i++) {
        if (calculated[i] != frame_bits[data_len + i])
            return 0;
    }
    return 1;
}

static void reset_frame(mvb_priv *s)
{
    s->received_master_header = 0;
    s->received_slave_header = 0;
    s->decoded_len = 0;
    s->matching_header_ticks = 0;
    s->is_even_tick = 1;
    s->last_tick = 0;
    s->state = STATE_FIND_START;
}

/* Process master frame: outputs CRC/CRC_ERROR annotation and returns fcode/addr.
 * Returns 1 if CRC OK, 0 if CRC error. */
static int process_master_frame(struct srd_decoder_inst *di, mvb_priv *s, int *out_fcode, uint16_t *out_addr)
{
    if (s->decoded_len < 24) return 0;

    /* Master frame: 4-bit flag + 12-bit address + 8-bit CRC = 24 bits */
    int crc_ok = check_check_sequence(s->decoded_buffer, s->decoded_len);

    /* F-code: bits 0-3 (4 bits, MSB first) */
    int fcode = 0;
    for (int i = 0; i < 4 && i < s->decoded_len; i++)
        fcode = (fcode << 1) | s->decoded_buffer[i];

    /* Address: bits 4-15 (12 bits, MSB first) */
    uint16_t addr = 0;
    for (int i = 4; i < 16 && i < s->decoded_len; i++)
        addr = (addr << 1) | s->decoded_buffer[i];

    *out_fcode = fcode;
    *out_addr = addr;

    /* CRC annotation at bits 16-23 */
    uint64_t crc_begin = s->frame_data_begin + 16 * s->mvb_samples_per_bit;
    uint64_t crc_end = crc_begin + 8 * s->mvb_samples_per_bit;

    if (crc_ok) {
        uint8_t crc_val = 0;
        for (int i = s->decoded_len - 8; i < s->decoded_len; i++)
            crc_val = (crc_val << 1) | s->decoded_buffer[i];
        char crc_str[16];
        snprintf(crc_str, sizeof(crc_str), "0x%x", crc_val);
        c_put(di, crc_begin, crc_end, s->out_ann, ANN_CRC, crc_str);
    } else {
        c_put(di, crc_begin, crc_end, s->out_ann, ANN_CRC_ERROR, "CRC Error");
    }

    return crc_ok;
}

static void process_slave_frame(struct srd_decoder_inst *di, mvb_priv *s)
{
    int ret = s->decoded_len;

    if (ret == 24) {
        /* 16-bit data + 8-bit CRC */
        uint64_t data_begin = s->frame_data_begin;
        uint64_t data_end = data_begin + 16 * s->mvb_samples_per_bit;
        uint64_t crc_end = data_end + 8 * s->mvb_samples_per_bit;
        int check_ok = check_check_sequence(s->decoded_buffer, s->decoded_len);
        if (check_ok) {
            uint8_t crc_val = 0;
            for (int i = s->decoded_len - 8; i < s->decoded_len; i++)
                crc_val = (crc_val << 1) | s->decoded_buffer[i];
            char crc_str[16];
            snprintf(crc_str, sizeof(crc_str), "0x%x", crc_val);
            c_put(di, data_end, crc_end, s->out_ann, ANN_CRC, crc_str);

            int num_bytes = 16 / 8;
            char data_hex[64];
            int pos = 0;
            for (int byte_idx = 0; byte_idx < num_bytes; byte_idx++) {
                uint8_t byte_val = 0;
                for (int bit = 0; bit < 8; bit++)
                    byte_val = (byte_val << 1) | s->decoded_buffer[byte_idx * 8 + bit];
                pos += snprintf(data_hex + pos, sizeof(data_hex) - pos, "%02x", byte_val);
            }
            char slave_str[80];
            snprintf(slave_str, sizeof(slave_str), "0x%s", data_hex);
            c_put(di, data_begin, data_end, s->out_ann, ANN_SLAVE_DATA, slave_str);
        } else {
            c_put(di, data_end, crc_end, s->out_ann, ANN_CRC_ERROR, "CRC Error");
        }
        return;
    }

    if (ret == 40) {
        /* 32-bit data + 8-bit CRC */
        uint64_t data_begin = s->frame_data_begin;
        uint64_t data_end = data_begin + 32 * s->mvb_samples_per_bit;
        uint64_t crc_end = data_end + 8 * s->mvb_samples_per_bit;
        int check_ok = check_check_sequence(s->decoded_buffer, s->decoded_len);
        if (check_ok) {
            uint8_t crc_val = 0;
            for (int i = s->decoded_len - 8; i < s->decoded_len; i++)
                crc_val = (crc_val << 1) | s->decoded_buffer[i];
            char crc_str[16];
            snprintf(crc_str, sizeof(crc_str), "0x%x", crc_val);
            c_put(di, data_end, crc_end, s->out_ann, ANN_CRC, crc_str);

            int num_bytes = 32 / 8;
            char data_hex[64];
            int pos = 0;
            for (int byte_idx = 0; byte_idx < num_bytes; byte_idx++) {
                uint8_t byte_val = 0;
                for (int bit = 0; bit < 8; bit++)
                    byte_val = (byte_val << 1) | s->decoded_buffer[byte_idx * 8 + bit];
                pos += snprintf(data_hex + pos, sizeof(data_hex) - pos, "%02x", byte_val);
            }
            char slave_str[80];
            snprintf(slave_str, sizeof(slave_str), "0x%s", data_hex);
            c_put(di, data_begin, data_end, s->out_ann, ANN_SLAVE_DATA, slave_str);
        } else {
            c_put(di, data_end, crc_end, s->out_ann, ANN_CRC_ERROR, "CRC Error");
        }
        return;
    }

    /* 64-bit data segments: each 72-bit segment (64 data + 8 CRC) checked independently */
    {
        int data_segment_length = 64 + 8;
        int segments = s->decoded_len / data_segment_length;
        char all_data_hex[256];
        int all_pos = 0;

        for (int seg = 0; seg < segments; seg++) {
            uint64_t seg_data_begin = s->frame_data_begin + seg * data_segment_length * s->mvb_samples_per_bit;
            uint64_t seg_data_end = seg_data_begin + 64 * s->mvb_samples_per_bit;
            uint64_t seg_crc_end = seg_data_end + 8 * s->mvb_samples_per_bit;

            uint8_t sub_bits[72];
            int sub_len = data_segment_length;
            for (int i = 0; i < sub_len; i++)
                sub_bits[i] = s->decoded_buffer[seg * data_segment_length + i];

            int check_ok = check_check_sequence(sub_bits, sub_len);
            if (check_ok) {
                uint8_t crc_val = 0;
                for (int i = sub_len - 8; i < sub_len; i++)
                    crc_val = (crc_val << 1) | sub_bits[i];
                char crc_str[16];
                snprintf(crc_str, sizeof(crc_str), "0x%x", crc_val);
                c_put(di, seg_data_end, seg_crc_end, s->out_ann, ANN_CRC, crc_str);

                for (int byte_idx = 0; byte_idx < 8 && all_pos < (int)sizeof(all_data_hex) - 3; byte_idx++) {
                    uint8_t byte_val = 0;
                    for (int bit = 0; bit < 8; bit++)
                        byte_val = (byte_val << 1) | sub_bits[byte_idx * 8 + bit];
                    all_pos += snprintf(all_data_hex + all_pos, sizeof(all_data_hex) - all_pos, "%02x", byte_val);
                }
            } else {
                c_put(di, seg_data_end, seg_crc_end, s->out_ann, ANN_CRC_ERROR, "CRC Error");
                return;
            }
        }

        if (all_pos > 0) {
            char slave_str[280];
            snprintf(slave_str, sizeof(slave_str), "0x%s", all_data_hex);
            c_put(di, s->frame_data_begin, s->frame_data_begin + s->decoded_len * s->mvb_samples_per_bit,
                  s->out_ann, ANN_SLAVE_DATA, slave_str);
        }
    }
}

static int process_tick(struct srd_decoder_inst *di, mvb_priv *s, int tick_value, uint64_t samplenum)
{
    (void)samplenum;
    if (!s->received_master_header && !s->received_slave_header) {
        s->matching_header_ticks = ((s->matching_header_ticks << 1) | tick_value) & PREAMBLE_MASK;
        if (s->matching_header_ticks == PREAMBLE_MASTER) {
            s->received_master_header = 1;
            s->matching_header_ticks = 0;
            c_put(di, samplenum - ((PREAMBLE_LENGTH + 1) * s->mvb_samples_per_bit / 2),
                      s->sample_end, s->out_ann, ANN_MASTER_PREAMBLE, "Master p");
            s->frame_data_begin = s->sample_end;
        }
        if (s->matching_header_ticks == PREAMBLE_SLAVE) {
            s->received_slave_header = 1;
            s->matching_header_ticks = 0;
            c_put(di, samplenum - ((PREAMBLE_LENGTH + 1) * s->mvb_samples_per_bit / 2),
                      s->sample_end, s->out_ann, ANN_SLAVE_PREAMBLE, "Slave p");
            s->frame_data_begin = s->sample_end;
        }
        return 1;
    }

    /* Manchester decoding */
    if (!s->is_even_tick) {
        uint64_t bit_begin = s->sample_end - s->mvb_samples_per_bit;
        if (s->last_tick == 0 && tick_value == 1) {
            if (s->decoded_len < 512)
                s->decoded_buffer[s->decoded_len++] = 0;
            c_put(di, bit_begin, s->sample_end, s->out_ann, ANN_BIT, "0");
        } else if (s->last_tick == 1 && tick_value == 0) {
            if (s->decoded_len < 512)
                s->decoded_buffer[s->decoded_len++] = 1;
            c_put(di, bit_begin, s->sample_end, s->out_ann, ANN_BIT, "1");
        } else {
            /* Same bit twice = transition error = frame boundary */
            if (s->received_master_header) {
                int fcode = 0;
                uint16_t addr = 0;
                process_master_frame(di, s, &fcode, &addr);
                uint64_t addr_begin = s->frame_data_begin + 4 * s->mvb_samples_per_bit;
                char fc_hex[16], addr_hex[16];
                snprintf(fc_hex, sizeof(fc_hex), "0x%x", fcode);
                snprintf(addr_hex, sizeof(addr_hex), "0x%x", addr);
                c_put(di, s->frame_data_begin, addr_begin, s->out_ann, ANN_MASTER_DATA, fc_hex);
                c_put(di, addr_begin, addr_begin + 12 * s->mvb_samples_per_bit, s->out_ann, ANN_MASTER_DATA, addr_hex);
                if (fcode < 16) {
                    c_put(di, s->frame_data_begin, addr_begin, s->out_ann, ANN_F_CODE, F_codes[fcode]);
                }
                if (fcode >= 0 && fcode <= 4) {
                    char addr_str[16];
                    snprintf(addr_str, sizeof(addr_str), "%d", addr);
                    c_put(di, addr_begin, addr_begin + 12 * s->mvb_samples_per_bit, s->out_ann, ANN_ADDR, addr_str);
                }
            }
            if (s->received_slave_header)
                process_slave_frame(di, s);
            reset_frame(s);
            return 0;
        }
    }
    s->is_even_tick = !s->is_even_tick;
    s->last_tick = tick_value;
    return 1;
}

static void mvb_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(mvb_priv)));
    mvb_priv *s = (mvb_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mvb_priv));
    s->state = STATE_FIND_START;
    s->is_even_tick = 1;
    s->out_ann = -1;
}

static void mvb_start(struct srd_decoder_inst *di)
{
    mvb_priv *s = (mvb_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mvb");
}

static void mvb_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    mvb_priv *s = (mvb_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (value > 0) {
            s->samples_per_tick = value / MVB_CLOCK_RATE;
            s->mvb_samples_per_bit = 2 * s->samples_per_tick;
        }
    }
}

static void mvb_decode(struct srd_decoder_inst *di)
{
    mvb_priv *s = (mvb_priv *)c_decoder_get_private(di);
    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (s->samplerate == 0)
        return;

    if (s->samples_per_tick == 0) {
        s->samples_per_tick = s->samplerate / MVB_CLOCK_RATE;
        s->mvb_samples_per_bit = 2 * s->samples_per_tick;
    }

    /* Wait for first falling edge */
    int ret = c_wait(di, CW_F(0), CW_END);
    if (ret != SRD_OK)
        return;

    s->sample_begin = di_samplenum(di);
    s->sample_end = di_samplenum(di);

    uint64_t notch_begin = di_samplenum(di);
    int phase = 0;

    while (1) {
        ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;
        uint64_t notch = di_samplenum(di) - s->sample_end;
        s->sample_begin = s->sample_end;
        s->sample_end = di_samplenum(di);

        /* Convert notch length to tick count */
        int num_ticks = 0;
        if (s->samples_per_tick > 0) {
            num_ticks = (int)((notch + s->samples_per_tick / 2) / s->samples_per_tick);
            if (num_ticks < 1) num_ticks = 1;
            if (num_ticks > 4) num_ticks = 4;
        }

        /* Handle long notches (>= 4 ticks): process one tick then reset */
        if (num_ticks >= 4) {
            s->sample_begin = notch_begin;
            s->sample_end = notch_begin + s->samples_per_tick;
            process_tick(di, s, 1, s->sample_end);
            reset_frame(s);
            notch_begin = di_samplenum(di);
            phase = !phase;
            continue;
        }

        /* Process each tick with phase-based tick values */
        for (int i = 0; i < num_ticks; i++) {
            int tick_value = phase ? 1 : 0;
            s->sample_begin = notch_begin + i * s->samples_per_tick;
            s->sample_end = notch_begin + (i + 1) * s->samples_per_tick;
            int cont = process_tick(di, s, tick_value, s->sample_end);
            if (!cont) break;
        }

        notch_begin = di_samplenum(di);
        phase = !phase;
    }
}

static void mvb_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder mvb_c_decoder = {
    .id = "mvb_c",
    .name = "MVB(C)",
    .longname = "Multifunction Vehicle Bus (C)",
    .desc = "Multifunction Vehicle Bus Manchester II with custom preamble. (C implementation)",
    .license = "gplv2+",
    .channels = mvb_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mvb_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = mvb_ann_rows,
    .inputs = mvb_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .tags = mvb_tags,
    .num_tags = 1,
    .binary = NULL,
    .num_binary = 0,
    .reset = mvb_reset,
    .start = mvb_start,
    .decode = mvb_decode,
    .destroy = mvb_destroy,
    .state_size = 0,
    .metadata = mvb_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &mvb_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}