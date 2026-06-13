/*
 * Copyright (C) 2023 DreamSourceLab <support@dreamsourcelab.com>
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
    ANN_DATA = 0,
    ANN_CONTROL,
    ANN_ERROR,
    ANN_INLINE_ERROR,
    NUM_ANN,
};

enum {
    FIND_BREAK,
    SYNC,
    PID,
    DATA,
    CHECKSUM,
};

struct lin_byte_entry {
    uint64_t ss;
    uint64_t es;
    uint8_t val;
};

struct lin_priv {
    int state;
    int version;
    int out_ann;

    /* Current byte position from upstream UART */
    uint64_t cur_ss;
    uint64_t cur_es;

    /* Break start sample */
    uint64_t ss_break;

    /* Header bytes: sync + pid */
    struct lin_byte_entry header[2];
    int header_cnt;

    /* Response/data bytes (including checksum) */
    struct lin_byte_entry rsp[9]; /* max 8 data + 1 checksum */
    int rsp_cnt;

    int done_break;
};

static int pid_to_data_len(uint8_t pid)
{
    int id = pid & 0x3F;
    if (id >= 48) return 8;
    if (id >= 32) return 4;
    return 2;
}

static uint8_t calc_parity(uint8_t pid)
{
    int id0 = (pid >> 0) & 1;
    int id1 = (pid >> 1) & 1;
    int id2 = (pid >> 2) & 1;
    int id3 = (pid >> 3) & 1;
    int id4 = (pid >> 4) & 1;
    int id5 = (pid >> 5) & 1;
    int p0 = id0 ^ id1 ^ id2 ^ id4;
    int p1 = (~(id1 ^ id3 ^ id4 ^ id5)) & 1;
    return (p0 << 0) | (p1 << 1);
}

static uint8_t lin_checksum_compute(uint8_t pid, uint8_t *data, int len, int enhanced)
{
    uint16_t sum = 0;
    if (enhanced)
        sum += pid;
    for (int i = 0; i < len; i++)
        sum += data[i];
    while (sum > 0xFF)
        sum = (sum & 0xFF) + (sum >> 8);
    return (~sum) & 0xFF;
}

static void lin_output_checksum(struct srd_decoder_inst *di, struct lin_priv *priv)
{
    if (priv->header_cnt < 2 || priv->rsp_cnt < 1)
        return;

    struct lin_byte_entry *sync_entry = &priv->header[0];
    struct lin_byte_entry *pid_entry = &priv->header[1];
    struct lin_byte_entry *checksum_entry = &priv->rsp[priv->rsp_cnt - 1];

    /* Sync byte annotation */
    c_put(di, sync_entry->ss, sync_entry->es, priv->out_ann, ANN_DATA,
              "Sync", "S");

    if (sync_entry->val != 0x55) {
        c_put(di, sync_entry->ss, sync_entry->es, priv->out_ann, ANN_ERROR,
                  "Sync is not 0x55", "Not 0x55", "!= 0x55");
    }

    /* PID byte annotation */
    uint8_t pid_val = pid_entry->val;
    int id = pid_val & 0x3F;
    int parity = (pid_val >> 6) & 0x3;
    uint8_t expected_parity = calc_parity(pid_val);
    int parity_valid = (parity == expected_parity);

    if (!parity_valid) {
        char pt[16];
        snprintf(pt, sizeof(pt), "P != %d", expected_parity);
        c_put(di, pid_entry->ss, pid_entry->es, priv->out_ann, ANN_ERROR, pt);
    }

    int ann_cls = parity_valid ? ANN_DATA : ANN_INLINE_ERROR;
    char t1[64], t2[32], t3[16];
    const char *pstr = parity_valid ? "ok" : "bad";
    snprintf(t1, sizeof(t1), "ID: %02X Parity: %d (%s)", id, parity, pstr);
    snprintf(t2, sizeof(t2), "ID: 0x%02X", id);
    snprintf(t3, sizeof(t3), "I: %d", id);
    c_put(di, pid_entry->ss, pid_entry->es, priv->out_ann, ann_cls, t1, t2, t3);

    /* Data bytes and checksum — rsp_cnt includes the checksum byte */
    int data_cnt = priv->rsp_cnt - 1;
    if (data_cnt > 0) {
        /* Data byte annotations */
        for (int i = 0; i < data_cnt; i++) {
            struct lin_byte_entry *b = &priv->rsp[i];
            char d1[32], d2[16];
            snprintf(d1, sizeof(d1), "Data: 0x%02X", b->val);
            snprintf(d2, sizeof(d2), "D: 0x%02X", b->val);
            c_put(di, b->ss, b->es, priv->out_ann, ANN_DATA, d1, d2);
        }

        /* Checksum validation */
        uint8_t data_bytes[8];
        for (int i = 0; i < data_cnt && i < 8; i++)
            data_bytes[i] = priv->rsp[i].val;

        int enhanced = (priv->version >= 2) ? (id != 60 && id != 61) : 0;
        uint8_t expected = lin_checksum_compute(pid_val, data_bytes, data_cnt, enhanced);
        int checksum_ok = (checksum_entry->val == expected);

        int chk_cls = checksum_ok ? ANN_DATA : ANN_INLINE_ERROR;
        char c1[32], c2[16], c3[8];
        snprintf(c1, sizeof(c1), "Checksum: 0x%02X", checksum_entry->val);
        snprintf(c2, sizeof(c2), "Checksum");
        snprintf(c3, sizeof(c3), "Chk");
        c_put(di, checksum_entry->ss, checksum_entry->es, priv->out_ann, chk_cls, c1, c2, c3);

        if (!checksum_ok) {
            c_put(di, checksum_entry->ss, checksum_entry->es, priv->out_ann, ANN_ERROR,
                      "Checksum invalid");
        }
    }
}

static void lin_reset_state(struct lin_priv *priv)
{
    priv->state = FIND_BREAK;
    priv->header_cnt = 0;
    priv->rsp_cnt = 0;
}

static struct srd_decoder_option lin_options_arr[1];

static const char *lin_ann_labels[][3] = {
    {"", "data", "LIN data"},
    {"", "control", "Protocol info"},
    {"", "error", "Error descriptions"},
    {"", "inline_error", "Protocol violations and errors"},
};

static const int lin_row_data_classes[] = {ANN_DATA, ANN_CONTROL, ANN_INLINE_ERROR};
static const int lin_row_error_classes[] = {ANN_ERROR};
static const struct srd_c_ann_row lin_ann_rows[] = {
    {"data", "Data", lin_row_data_classes, 3},
    {"error", "Error", lin_row_error_classes, 1},
};

static const char *lin_inputs[] = {"uart", NULL};
static const char *lin_outputs[] = {NULL};
static const char *lin_tags[] = {"Automotive", NULL};

static void lin_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct lin_priv)));
    struct lin_priv *priv = (struct lin_priv *)c_decoder_get_private(di);
    memset(priv, 0, sizeof(struct lin_priv));
    priv->state = FIND_BREAK;
    priv->out_ann = 0;
}

static void lin_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    (void)di;
    (void)key;
    (void)value;
}

static void lin_start(struct srd_decoder_inst *di)
{
    struct lin_priv *priv = (struct lin_priv *)c_decoder_get_private(di);
    priv->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "lin");
    priv->version = (int)c_opt_int(di, "version", 2);
}

static void lin_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    struct lin_priv *priv = (struct lin_priv *)c_decoder_get_private(di);
    if (!priv)
        return;

    priv->cur_ss = start_sample;
    priv->cur_es = end_sample;

    if (strcmp(cmd, "BREAK") == 0) {
        /* Handle BREAK from upstream UART decoder */
        if (priv->state != FIND_BREAK) {
            /* Mid-frame break: wipe the null byte that accompanies the break,
               then output the checksum for the partial frame before resetting.
               This matches Python's wipe_break_null_byte() + handle_break() logic. */

            /* Remove the null byte (0x00) that the UART decoder emits alongside
               the break condition. In the Python version, wipe_break_null_byte()
               pops the last rsp byte (or header byte) to discard this null byte. */
            if (priv->rsp_cnt > 0) {
                priv->rsp_cnt--;
            } else if (priv->header_cnt > 0) {
                priv->header_cnt--;
            }

            /* Output checksum for the partial frame collected so far */
            lin_output_checksum(di, priv);
        }

        /* Reset state machine for new frame */
        lin_reset_state(priv);

        /* Output break annotation only for mid-frame breaks or the very first break.
           Subsequent breaks while already waiting (FIND_BREAK state) are expected
           and don't need a separate LIN-level annotation. */
        if (priv->state != FIND_BREAK || !priv->done_break) {
            c_put(di, start_sample, end_sample, priv->out_ann, ANN_CONTROL,
                      "Break condition", "Break", "Brk", "B");
        }

        priv->state = SYNC;
        priv->done_break = 1;
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 1)
        return;

    uint8_t byte_val = fields[0].u8;

    switch (priv->state) {

    case FIND_BREAK:
        /* Waiting for a BREAK; ignore data bytes */
        break;

    case SYNC: {
        struct lin_byte_entry *entry = &priv->header[priv->header_cnt++];
        entry->ss = start_sample;
        entry->es = end_sample;
        entry->val = byte_val;
        priv->state = PID;
        break;
    }

    case PID: {
        struct lin_byte_entry *entry = &priv->header[priv->header_cnt++];
        entry->ss = start_sample;
        entry->es = end_sample;
        entry->val = byte_val;

        int n_fields = pid_to_data_len(byte_val);
        priv->state = (n_fields > 0) ? DATA : CHECKSUM;
        break;
    }

    case DATA: {
        struct lin_byte_entry *entry = &priv->rsp[priv->rsp_cnt++];
        entry->ss = start_sample;
        entry->es = end_sample;
        entry->val = byte_val;

        /* Determine expected data length from PID */
        uint8_t pid_val = priv->header[1].val;
        int expected_data_len = pid_to_data_len(pid_val);

        if (priv->rsp_cnt >= expected_data_len)
            priv->state = CHECKSUM;
        break;
    }

    case CHECKSUM: {
        struct lin_byte_entry *entry = &priv->rsp[priv->rsp_cnt++];
        entry->ss = start_sample;
        entry->es = end_sample;
        entry->val = byte_val;

        /* Output all annotations for the complete frame */
        lin_output_checksum(di, priv);

        /* Reset for next frame */
        lin_reset_state(priv);
        break;
    }

    }
}

static void lin_end(struct srd_decoder_inst *di)
{
    struct lin_priv *priv = (struct lin_priv *)c_decoder_get_private(di);
    if (!priv)
        return;
    if (priv->done_break && priv->rsp_cnt > 0)
        lin_output_checksum(di, priv);
}

static void lin_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void lin_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder lin_c_decoder = {
    .id = "lin_c",
    .name = "LIN",
    .longname = "Local Interconnect Network",
    .desc = "Local Interconnect Network (LIN) protocol.",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = lin_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = lin_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = lin_ann_rows,
    .inputs = lin_inputs,
    .num_inputs = 1,
    .outputs = lin_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = lin_tags,
    .num_tags = 1,
    .metadata = lin_metadata,
    .reset = lin_reset,
    .start = lin_start,
    .decode = lin_decode,
    .end = lin_end,
    .destroy = lin_destroy,
    .decode_upper = lin_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GSList *version_list = NULL;
    version_list = g_slist_append(version_list, g_variant_new_int64(1));
    version_list = g_slist_append(version_list, g_variant_new_int64(2));

    lin_options_arr[0].id = "version";
    lin_options_arr[0].idn = "dec_lin_opt_version";
    lin_options_arr[0].desc = "Protocol version";
    lin_options_arr[0].def = g_variant_new_int64(2);
    lin_options_arr[0].values = version_list;

    return &lin_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}