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

#define PN532_MAX_DATA 256

enum {
    ANN_START = 0,
    ANN_LEN,
    ANN_LCS,
    ANN_TFI,
    ANN_DATA,
    ANN_DCS,
    ANN_END,
    ANN_ERROR,
    ANN_FRAME,
    ANN_CMD,
    ANN_PREAMBLE,
    ANN_INSTRUCTION,
    NUM_ANN,
};

enum pn532_state {
    PN532_START_FRAME,
    PN532_LENGTH,
    PN532_TFI,
    PN532_DATA,
    PN532_CHECKSUM,
    PN532_END_FRAME,
};

enum pn532_frame_type {
    FRAME_HOST_TO_PN532 = 0,
    FRAME_PN532_TO_HOST = 1,
    FRAME_ACK = 2,
    FRAME_NACK = 3,
    FRAME_ERROR = 4,
};

typedef struct {
    uint8_t byte_val;
    uint64_t ss;
    uint64_t es;
} pn532_byte_data;

typedef struct {
    enum pn532_state state;
    pn532_byte_data start_frame[3];
    int start_frame_idx;
    pn532_byte_data length[2];
    int length_idx;
    pn532_byte_data tfi;
    pn532_byte_data data_packet[PN532_MAX_DATA];
    int data_packet_len;
    int data_size;
    pn532_byte_data checksum;
    pn532_byte_data preamble_byte;
    enum pn532_frame_type frame_type;
    int out_ann;
    int format; /* 0=hex, 1=ascii, 2=dec, 3=oct, 4=bin */
} pn532_state;

static const char *pn532_inputs[] = {"uart", NULL};
static const char *pn532_outputs[] = {"ISO14443", NULL};
static const char *pn532_tags[] = {"Automotive", NULL};

static struct srd_decoder_option pn532_options[] = {
    {"preamble", "dec_pn532_opt_preamble", "Preamble byte", NULL, NULL},
    {"postamble", "dec_pn532_opt_postamble", "Postamble byte", NULL, NULL},
    {"start frame", "dec_pn532_opt_start_frame", "Start frame byte", NULL, NULL},
    {"format", "dec_pn532_opt_format", "Data format", NULL, NULL},
};

static const char *pn532_ann_labels[][3] = {
    {"", "start", "Start frame"},
    {"", "len", "Data length"},
    {"", "lcs", "Data length checksum"},
    {"", "tfi", "Frame identifier"},
    {"", "data", "Packet data"},
    {"", "dcs", "Data checksum"},
    {"", "end", "Postamble"},
    {"", "error", "Error description"},
    {"", "frame", "Frame type"},
    {"", "cmd", "Command"},
    {"", "preamble", "Preamble"},
    {"", "instruction", "Instruction"},
};

static const int pn532_row_data_vals_classes[] = {
    ANN_START, ANN_LEN, ANN_LCS, ANN_TFI, ANN_DATA, ANN_DCS, ANN_END,
    ANN_PREAMBLE, ANN_INSTRUCTION, -1
};
static const int pn532_row_frame_type_classes[] = {ANN_FRAME, -1};
static const int pn532_row_commands_classes[] = {ANN_CMD, -1};
static const int pn532_row_errors_classes[] = {ANN_ERROR, -1};

static const struct srd_c_ann_row pn532_ann_rows[] = {
    {"data_vals", "Data", pn532_row_data_vals_classes, 9},
    {"frame_type", "Frame type", pn532_row_frame_type_classes, 1},
    {"commands", "Commands", pn532_row_commands_classes, 1},
    {"errors", "Errors", pn532_row_errors_classes, 1},
};

/* PN532 command lookup tables */
typedef struct {
    uint8_t code;
    const char *name;
} pn532_cmd_entry;

static const pn532_cmd_entry miscellaneous_cmds[] = {
    {0x00, "Diagnose"}, {0x02, "GetFirmwareVersion"},
    {0x04, "GetGeneralStatus"}, {0x06, "ReadRegister"},
    {0x08, "WriteRegister"}, {0x0C, "ReadGPIO"},
    {0x0E, "WriteGPIO"}, {0x10, "SetSerialBaudRate"},
    {0x12, "SetParameters"}, {0x14, "SAMConfiguration"},
    {0x16, "PowerDown"},
};

static const pn532_cmd_entry rf_communication_cmds[] = {
    {0x32, "RFConfiguration"}, {0x58, "RFRegulationTest"},
};

static const pn532_cmd_entry initiator_cmds[] = {
    {0x56, "InJumpForDEP"}, {0x46, "InJumpForPSL"},
    {0x4A, "InListPassiveTarget"}, {0x50, "InATR"},
    {0x4E, "InPSL"}, {0x40, "InDataExchange"},
    {0x42, "InCommunicateThru"}, {0x44, "InDeselect"},
    {0x52, "InRelease"}, {0x54, "InSelect"},
    {0x60, "InAutoPoll"},
};

static const pn532_cmd_entry target_cmds[] = {
    {0x8C, "TgInitAsTarget"}, {0x92, "TgSetGeneralBytes"},
    {0x86, "TgGetData"}, {0x8E, "TgSetData"},
    {0x94, "TgSetMetaData"}, {0x88, "TgGetInitiatorCommand"},
    {0x90, "TgResponseToInitiator"}, {0x8A, "TgGetTargetStatus"},
};

static const char *pn532_find_cmd(uint8_t cmd)
{
    for (int i = 0; i < (int)(sizeof(miscellaneous_cmds) / sizeof(miscellaneous_cmds[0])); i++)
        if (miscellaneous_cmds[i].code == cmd)
            return miscellaneous_cmds[i].name;
    for (int i = 0; i < (int)(sizeof(rf_communication_cmds) / sizeof(rf_communication_cmds[0])); i++)
        if (rf_communication_cmds[i].code == cmd)
            return rf_communication_cmds[i].name;
    for (int i = 0; i < (int)(sizeof(initiator_cmds) / sizeof(initiator_cmds[0])); i++)
        if (initiator_cmds[i].code == cmd)
            return initiator_cmds[i].name;
    for (int i = 0; i < (int)(sizeof(target_cmds) / sizeof(target_cmds[0])); i++)
        if (target_cmds[i].code == cmd)
            return target_cmds[i].name;
    return NULL;
}

static const char *frame_type_strings[][2] = {
    {"Host to PN532", "H2C"},
    {"PN532 to Host", "C2H"},
    {"Acknowledge", "ACK"},
    {"Not Acknowledge", "NACK"},
    {"Application Error", "Error"},
};

static const char *pn532_error_strings[] __attribute__((unused)) = {
    [0x01] = "Time Out, the target has not answered",
    [0x02] = "A CRC error has been detected by the CIU",
    [0x03] = "A Parity error has been detected by the CIU",
    [0x04] = "Erroneous Bit Count detected",
    [0x05] = "Framing error during Mifare operation",
    [0x06] = "Abnormal bit-collision detected",
    [0x07] = "Communication buffer size insufficient",
    [0x09] = "RF Buffer overflow detected by the CIU",
    [0x0A] = "RF field not switched on in time",
    [0x0B] = "RF Protocol error",
    [0x0D] = "Temperature error",
    [0x0E] = "Internal buffer overflow",
    [0x10] = "Invalid parameter",
    [0x12] = "Command not supported in target mode",
    [0x13] = "Data format does not match specification",
    [0x14] = "Mifare: Authentication error",
    [0x23] = "UID Check byte is wrong",
    [0x25] = "Invalid device state",
    [0x26] = "Operation not allowed in this configuration",
    [0x27] = "Command not acceptable in current context",
    [0x29] = "Target released by initiator",
    [0x2A] = "Card ID does not match",
    [0x2B] = "Card previously activated has disappeared",
    [0x2C] = "NFCID3 mismatch in DEP 212/424 kbps",
    [0x2D] = "Over-current event detected",
    [0x2E] = "NAD missing in DEP frame",
};

static void pn532_format_value(uint8_t v, int format, char *buf, int buf_len)
{
    switch (format) {
    case 1: /* ascii */
        if (v >= 32 && v <= 126)
            snprintf(buf, buf_len, "%c", v);
        else
            snprintf(buf, buf_len, "[%02X]", v);
        break;
    case 2: /* dec */
        snprintf(buf, buf_len, "%d", v);
        break;
    case 3: /* oct */
        snprintf(buf, buf_len, "%03o", v);
        break;
    case 4: /* bin */
        snprintf(buf, buf_len, "%d%d%d%d%d%d%d%d",
            (v >> 7) & 1, (v >> 6) & 1, (v >> 5) & 1, (v >> 4) & 1,
            (v >> 3) & 1, (v >> 2) & 1, (v >> 1) & 1, v & 1);
        break;
    default: /* hex */
        snprintf(buf, buf_len, "%02X", v);
        break;
    }
}

static int pn532_checksum_ok(pn532_byte_data *bytes, int num_bytes, uint8_t checksum)
{
    int sum = 0;
    for (int i = 0; i < num_bytes; i++)
        sum += bytes[i].byte_val;
    return ((sum + checksum) & 0xFF) == 0;
}

static void pn532_reset_state(pn532_state *s)
{
    s->state = PN532_START_FRAME;
    s->start_frame_idx = 0;
    s->length_idx = 0;
    s->data_packet_len = 0;
    s->data_size = 0;
    s->frame_type = FRAME_HOST_TO_PN532;
}

static void pn532_handle_command_default(struct srd_decoder_inst *di, pn532_state *s,
                                          uint64_t ss, uint64_t es)
{
    if (s->tfi.byte_val == 0xD4 && s->data_packet_len > 0) {
        const char *cmd_name = pn532_find_cmd(s->data_packet[0].byte_val);
        if (cmd_name) {
            c_put(di, ss, es, s->out_ann, ANN_CMD, cmd_name);
        }
    } else if (s->tfi.byte_val == 0xD5 && s->data_packet_len > 0) {
        const char *cmd_name = pn532_find_cmd(s->data_packet[0].byte_val);
        if (cmd_name) {
            c_put(di, ss, es, s->out_ann, ANN_CMD, cmd_name);
        }
    } else if (s->tfi.byte_val == 0x7F) {
        /* Error frame */
    }
}

static void pn532_handle_start_frame(struct srd_decoder_inst *di, pn532_state *s, pn532_byte_data *bd)
{
    s->start_frame[s->start_frame_idx++] = *bd;

    /* Check for START_FRAME pattern: 0x00 0x00 0xFF */
    if (s->start_frame_idx >= 3) {
        if (s->start_frame[0].byte_val == 0x00 &&
            s->start_frame[1].byte_val == 0x00 &&
            s->start_frame[2].byte_val == 0xFF) {
            /* Preamble + Start code */
            s->preamble_byte = s->start_frame[0];
            c_put(di, s->start_frame[0].ss, s->start_frame[0].es, s->out_ann, ANN_PREAMBLE, "Preamble", "PR");
            c_put(di, s->start_frame[1].ss, s->start_frame[2].es, s->out_ann, ANN_START, "Start Frame", "Start", "S");
            s->state = PN532_LENGTH;
            s->length_idx = 0;
            return;
        }
    }

    /* If we have 3 bytes but no match, shift the window */
    if (s->start_frame_idx >= 3) {
        s->start_frame[0] = s->start_frame[1];
        s->start_frame[1] = s->start_frame[2];
        s->start_frame_idx = 2;
    }
}

static void pn532_handle_length(struct srd_decoder_inst *di, pn532_state *s, pn532_byte_data *bd)
{
    s->length[s->length_idx++] = *bd;

    if (s->length_idx < 2)
        return;

    /* Check for ACK: 0x00 0xFF */
    if (s->length[0].byte_val == 0x00 && s->length[1].byte_val == 0xFF) {
        s->frame_type = FRAME_ACK;
        s->state = PN532_END_FRAME;
        return;
    }

    /* Check for NACK: 0xFF 0x00 */
    if (s->length[0].byte_val == 0xFF && s->length[1].byte_val == 0x00) {
        s->frame_type = FRAME_NACK;
        s->state = PN532_END_FRAME;
        return;
    }

    /* Check for extended frame: 0xFF 0xFF */
    if (s->length[0].byte_val == 0xFF && s->length[1].byte_val == 0xFF) {
        /* Extended frame - not fully implemented, skip to end */
        s->frame_type = FRAME_ERROR;
        s->state = PN532_END_FRAME;
        return;
    }

    /* Normal frame */
    s->data_size = s->length[0].byte_val - 1;
    char buf[64];
    char val_buf[16];
    pn532_format_value(s->data_size, s->format, val_buf, sizeof(val_buf));
    snprintf(buf, sizeof(buf), "Data Length: %s", val_buf);
    c_put(di, s->length[0].ss, s->length[0].es, s->out_ann, ANN_LEN, buf, "Length", "LEN");

    /* Verify LCS: LEN + LCS should be 0 */
    if (pn532_checksum_ok(s->length, 1, s->length[1].byte_val)) {
        c_put(di, s->length[1].ss, s->length[1].es, s->out_ann, ANN_LCS, "Data Length Checksum: OK", "Checksum: OK", "LCS");
        s->state = PN532_TFI;
    } else {
        c_put(di, s->length[1].ss, s->length[1].es, s->out_ann, ANN_ERROR, "Checksum Error", "Error", "E");
        s->state = PN532_END_FRAME;
    }
}

static void pn532_handle_tfi(struct srd_decoder_inst *di, pn532_state *s, pn532_byte_data *bd)
{
    s->tfi = *bd;

    if (bd->byte_val == 0xD4) {
        s->frame_type = FRAME_HOST_TO_PN532;
        s->state = PN532_DATA;
    } else if (bd->byte_val == 0xD5) {
        s->frame_type = FRAME_PN532_TO_HOST;
        s->state = PN532_DATA;
    } else if (bd->byte_val == 0x7F) {
        s->frame_type = FRAME_ERROR;
        s->state = PN532_CHECKSUM;
    } else {
        s->frame_type = FRAME_ERROR;
    }

    char buf[64];
    char val_buf[16];
    pn532_format_value(bd->byte_val, s->format, val_buf, sizeof(val_buf));
    snprintf(buf, sizeof(buf), "Frame identifier: %s", val_buf);
    char short_buf[32];
    snprintf(short_buf, sizeof(short_buf), "TFI: %s", val_buf);
    c_put(di, bd->ss, bd->es, s->out_ann, ANN_TFI, buf, short_buf, "TFI");
}

static void pn532_handle_data(struct srd_decoder_inst *di, pn532_state *s, pn532_byte_data *bd)
{
    if (s->data_packet_len < PN532_MAX_DATA) {
        s->data_packet[s->data_packet_len++] = *bd;
    }

    if (s->data_packet_len < s->data_size)
        return;

    /* All data bytes received */
    char val_buf[16];
    char buf[512];

    /* First data byte = command */
    pn532_format_value(s->data_packet[0].byte_val, s->format, val_buf, sizeof(val_buf));
    snprintf(buf, sizeof(buf), "Command: %s", val_buf);
    c_put(di, s->data_packet[0].ss, s->data_packet[0].es, s->out_ann, ANN_DATA, buf, val_buf);

    /* Remaining data bytes */
    if (s->data_packet_len > 1) {
        char data_buf[512];
        int pos = 0;
        pos += snprintf(data_buf + pos, sizeof(data_buf) - pos, "Data: ");
        for (int i = 1; i < s->data_packet_len && pos < 400; i++) {
            pn532_format_value(s->data_packet[i].byte_val, s->format, val_buf, sizeof(val_buf));
            if (i > 1)
                pos += snprintf(data_buf + pos, sizeof(data_buf) - pos, " ");
            pos += snprintf(data_buf + pos, sizeof(data_buf) - pos, "%s", val_buf);
        }
        c_put(di, s->data_packet[1].ss, s->data_packet[s->data_packet_len - 1].es,
                  s->out_ann, ANN_DATA, data_buf);
    }

    s->state = PN532_CHECKSUM;
}

static void pn532_handle_checksum(struct srd_decoder_inst *di, pn532_state *s, pn532_byte_data *bd)
{
    s->checksum = *bd;

    /* Verify DCS: TFI + PD0 + PD1 + ... + PDK + DCS should be 0 */
    int sum = s->tfi.byte_val;
    for (int i = 0; i < s->data_packet_len; i++)
        sum += s->data_packet[i].byte_val;

    if (((sum + bd->byte_val) & 0xFF) == 0) {
        c_put(di, bd->ss, bd->es, s->out_ann, ANN_DCS, "Data Checksum: OK", "DCS");
    } else {
        c_put(di, bd->ss, bd->es, s->out_ann, ANN_DCS, "Data Checksum", "DCS");
        c_put(di, bd->ss, bd->es, s->out_ann, ANN_ERROR, "Checksum Error", "Error", "E");
    }

    s->state = PN532_END_FRAME;
}

static void pn532_handle_end_frame(struct srd_decoder_inst *di, pn532_state *s, pn532_byte_data *bd)
{
    /* Output frame type annotation */
    int ft = (s->frame_type < 5) ? s->frame_type : 4;
    c_put(di, s->preamble_byte.ss, bd->es, s->out_ann, ANN_FRAME,
              frame_type_strings[ft][0], frame_type_strings[ft][1]);

    /* Postamble */
    c_put(di, bd->ss, bd->es, s->out_ann, ANN_END, "Postamble", "PO");

    /* Handle command for non-ACK/NACK/ERROR frames */
    if (s->frame_type != FRAME_ACK && s->frame_type != FRAME_NACK && s->frame_type != FRAME_ERROR) {
        pn532_handle_command_default(di, s, s->preamble_byte.ss, bd->es);
    }

    /* Reset state */
    pn532_reset_state(s);
}

static void pn532_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    pn532_state *s = (pn532_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 1)
        return;

    pn532_byte_data bd;
    bd.byte_val = fields[0].u8;
    bd.ss = start_sample;
    bd.es = end_sample;

    switch (s->state) {
    case PN532_START_FRAME:
        pn532_handle_start_frame(di, s, &bd);
        break;
    case PN532_LENGTH:
        pn532_handle_length(di, s, &bd);
        break;
    case PN532_TFI:
        pn532_handle_tfi(di, s, &bd);
        break;
    case PN532_DATA:
        pn532_handle_data(di, s, &bd);
        break;
    case PN532_CHECKSUM:
        pn532_handle_checksum(di, s, &bd);
        break;
    case PN532_END_FRAME:
        pn532_handle_end_frame(di, s, &bd);
        break;
    }
}

static void pn532_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(pn532_state)));
    }
    pn532_state *s = (pn532_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(pn532_state));
    s->state = PN532_START_FRAME;
    s->format = 0; /* hex */
}

static void pn532_start(struct srd_decoder_inst *di)
{
    pn532_state *s = (pn532_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pn532");

    const char *fmt = c_opt_str(di, "format", "hex");
    if (strcmp(fmt, "ascii") == 0)
        s->format = 1;
    else if (strcmp(fmt, "dec") == 0)
        s->format = 2;
    else if (strcmp(fmt, "oct") == 0)
        s->format = 3;
    else if (strcmp(fmt, "bin") == 0)
        s->format = 4;
    else
        s->format = 0; /* hex */
}

static void pn532_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void pn532_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder pn532_c_decoder = {
    .id = "pn532_c",
    .name = "PN532(C)",
    .longname = "PN532 nfc transceiver (C)",
    .desc = "PN532 chip command decoder. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = pn532_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = pn532_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = pn532_ann_rows,
    .inputs = pn532_inputs,
    .num_inputs = 1,
    .outputs = pn532_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = pn532_tags,
    .num_tags = 1,
    .reset = pn532_reset,
    .start = pn532_start,
    .decode = pn532_decode,
    .destroy = pn532_destroy,
    .decode_upper = pn532_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    pn532_options[0].def = g_variant_new_int64(0x00);
    pn532_options[1].def = g_variant_new_int64(0x00);
    pn532_options[2].def = g_variant_new_int64(0x00);

    pn532_options[3].def = g_variant_new_string("hex");
    GSList *fmt_vals = NULL;
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("ascii"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("dec"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("hex"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("oct"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("bin"));
    pn532_options[3].values = fmt_vals;

    return &pn532_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}