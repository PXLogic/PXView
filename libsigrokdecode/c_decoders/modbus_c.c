/*
 * Copyright (C) 2023 DreamSourceLab <support@dreamsourcelab.com>
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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

static void fmt_binary(uint8_t val, char *buf, int bufsize) {
    if (bufsize < 9) { buf[0] = '\0'; return; }
    for (int i = 7; i >= 0; i--) buf[7 - i] = (val & (1 << i)) ? '1' : '0';
    buf[8] = '\0';
}

#define MODBUS_MAX_FRAME 256

enum {
    ANN_SC_SERVER_ID = 0,
    ANN_SC_FUNCTION,
    ANN_SC_CRC,
    ANN_SC_ADDRESS,
    ANN_SC_DATA,
    ANN_SC_LENGTH,
    ANN_SC_ERROR,
    ANN_CS_SERVER_ID,
    ANN_CS_FUNCTION,
    ANN_CS_CRC,
    ANN_CS_ADDRESS,
    ANN_CS_DATA,
    ANN_CS_LENGTH,
    ANN_CS_ERROR,
    ANN_ERROR_INDICATION,
    NUM_ANN,
};

typedef struct {
    uint8_t data[MODBUS_MAX_FRAME];
    uint64_t starts[MODBUS_MAX_FRAME];
    uint64_t ends[MODBUS_MAX_FRAME];
    int n_fields;
    int data_len;
    uint64_t last_read;
    int start_new_frame;
    int has_error;
    int last_byte_put;
    int minimum_length;
} modbus_adu;

typedef struct {
    modbus_adu adu_sc;
    modbus_adu adu_cs;
    double bitlength;
    int framegap;
    int sc_channel; /* 0=RX, 1=TX */
    int cs_channel; /* 0=RX, 1=TX */
    int out_ann;
} modbus_state;

static const char *modbus_inputs[] = {"uart", NULL};
static const char *modbus_outputs[] = {"modbus", NULL};
static const char *modbus_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option modbus_options[] = {
    {"scchannel", "dec_modbus_opt_scchannel", "Server -> client channel", NULL, NULL},
    {"cschannel", "dec_modbus_opt_cschannel", "Client -> server channel", NULL, NULL},
    {"framegap", "dec_modbus_opt_framegap", "Inter-frame bit gap", NULL, NULL},
};

static const char *modbus_ann_labels[][3] = {
    {"", "sc-server-id", "SC server ID"},
    {"", "sc-function", "SC function"},
    {"", "sc-crc", "SC CRC"},
    {"", "sc-address", "SC address"},
    {"", "sc-data", "SC data"},
    {"", "sc-length", "SC length"},
    {"", "sc-error", "SC error"},
    {"", "cs-server-id", "CS server ID"},
    {"", "cs-function", "CS function"},
    {"", "cs-crc", "CS CRC"},
    {"", "cs-address", "CS address"},
    {"", "cs-data", "CS data"},
    {"", "cs-length", "CS length"},
    {"", "cs-error", "CS error"},
    {"", "error-indication", "Error indication"},
};

static const int modbus_row_sc_classes[] = {
    ANN_SC_SERVER_ID, ANN_SC_FUNCTION, ANN_SC_CRC,
    ANN_SC_ADDRESS, ANN_SC_DATA, ANN_SC_LENGTH, ANN_SC_ERROR, -1
};
static const int modbus_row_cs_classes[] = {
    ANN_CS_SERVER_ID, ANN_CS_FUNCTION, ANN_CS_CRC,
    ANN_CS_ADDRESS, ANN_CS_DATA, ANN_CS_LENGTH, ANN_CS_ERROR, -1
};
static const int modbus_row_error_classes[] = {ANN_ERROR_INDICATION, -1};

static const struct srd_c_ann_row modbus_ann_rows[] = {
    {"sc", "Server->client", modbus_row_sc_classes, 7},
    {"cs", "Client->server", modbus_row_cs_classes, 7},
    {"error-indicators", "Errors in frame", modbus_row_error_classes, 1},
};

static uint16_t modbus_crc(uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xA001;
            else
                crc >>= 1;
        }
    }
    return crc;
}

static void modbus_adu_reset(modbus_adu *adu, uint64_t start)
{
    memset(adu, 0, sizeof(modbus_adu));
    adu->last_read = start;
    adu->minimum_length = 4;
    adu->last_byte_put = -1;
}

static void modbus_puta(struct srd_decoder_inst *di, modbus_state *s,
                        uint64_t start, uint64_t end, int ann, const char *msg)
{
    c_put(di, start, end, s->out_ann, ann, msg);
}

/* Put annotation from last_byte_put+1 to byte_to_put */
static void modbus_adu_puti(struct srd_decoder_inst *di, modbus_state *s,
                            modbus_adu *adu, int byte_to_put, int ann, const char *msg)
{
    if (byte_to_put > adu->data_len - 1 || byte_to_put >= MODBUS_MAX_FRAME)
        return;
    if (ann == ANN_SC_ERROR || ann == ANN_CS_ERROR)
        adu->has_error = 1;
    if (byte_to_put > adu->last_byte_put && adu->last_byte_put + 1 >= 0 && adu->last_byte_put + 1 < MODBUS_MAX_FRAME) {
        uint64_t ss = adu->starts[adu->last_byte_put + 1];
        uint64_t es = adu->ends[byte_to_put];
        modbus_puta(di, s, ss, es, ann, msg);
        adu->last_byte_put = byte_to_put;
    }
}

static uint16_t modbus_adu_half_word(modbus_adu *adu, int start)
{
    if (start + 1 > adu->data_len - 1)
        return 0;
    return (uint16_t)(adu->data[start] * 0x100 + adu->data[start + 1]);
}

static void modbus_adu_check_crc(struct srd_decoder_inst *di, modbus_state *s,
                                 modbus_adu *adu, int byte_to_put, int ann_crc, int ann_err)
{
    if (byte_to_put < 3 || byte_to_put > adu->data_len - 1)
        return;
    /* Calculate CRC over all bytes except the last 2 (CRC bytes) */
    uint16_t calc = modbus_crc(adu->data, byte_to_put - 1);
    uint8_t byte1 = calc & 0xFF;
    uint8_t byte2 = (calc >> 8) & 0xFF;
    if (adu->data[adu->data_len - 2] == byte1 && adu->data[adu->data_len - 1] == byte2) {
        modbus_adu_puti(di, s, adu, byte_to_put, ann_crc, "CRC correct");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "CRC should be %d %d", byte1, byte2);
        modbus_adu_puti(di, s, adu, byte_to_put, ann_err, buf);
    }
}

static void modbus_adu_close(struct srd_decoder_inst *di, modbus_state *s,
                             modbus_adu *adu, int ann_err, int ann_err_ind,
                             uint64_t overflow_es)
{
    if (adu->data_len < adu->minimum_length) {
        if (adu->data_len == 0)
            return;
        if (adu->last_byte_put >= 0 && adu->last_byte_put < adu->data_len) {
            modbus_puta(di, s, adu->ends[adu->last_byte_put], overflow_es,
                        ann_err, "Message too short or not finished");
        }
        adu->has_error = 1;
    }
    if (adu->has_error && s->sc_channel != s->cs_channel) {
        modbus_puta(di, s, adu->starts[0], adu->ends[adu->data_len - 1],
                    ann_err_ind, "Frame contains error");
    }
    if (adu->data_len > 256) {
        modbus_adu_puti(di, s, adu, adu->data_len - 1, ann_err,
                        "Modbus data frames are limited to 256 bytes");
    }
}

/* --- SC (Server -> Client) parsing --- */

static void modbus_sc_parse_read_bits(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    uint8_t function = adu->data[1];
    modbus_adu_puti(di, s, adu, 1, ANN_SC_FUNCTION,
                    (function == 1) ? "Function 1: Read Coils" : "Function 2: Read Discrete Inputs");

    int bytecount = adu->data[2];
    adu->minimum_length = 5 + bytecount;
    char buf[64];
    snprintf(buf, sizeof(buf), "Byte count: %d", bytecount);
    modbus_adu_puti(di, s, adu, 2, ANN_SC_LENGTH, buf);

    /* Data bytes */
    for (int i = 3; i < adu->data_len - 2 && i < bytecount + 3; i++) {
        char dbuf[32];
        fmt_binary(adu->data[i], dbuf, sizeof(dbuf));
        modbus_adu_puti(di, s, adu, i, ANN_SC_DATA, dbuf);
    }

    modbus_adu_check_crc(di, s, adu, bytecount + 4, ANN_SC_CRC, ANN_SC_ERROR);
}

static void modbus_sc_parse_read_registers(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    uint8_t function = adu->data[1];
    const char *fn_name = (function == 3) ? "Function 3: Read Holding Registers" :
                          (function == 4) ? "Function 4: Read Input Registers" :
                          "Function 23: Read/Write Multiple Registers";
    modbus_adu_puti(di, s, adu, 1, ANN_SC_FUNCTION, fn_name);

    int bytecount = adu->data[2];
    adu->minimum_length = 5 + bytecount;
    char buf[64];
    if (bytecount % 2 == 0) {
        snprintf(buf, sizeof(buf), "Byte count: %d", bytecount);
        modbus_adu_puti(di, s, adu, 2, ANN_SC_LENGTH, buf);
    } else {
        snprintf(buf, sizeof(buf), "Error: Odd byte count (%d)", bytecount);
        modbus_adu_puti(di, s, adu, 2, ANN_SC_ERROR, buf);
    }

    /* Register values */
    for (int i = 3; i + 1 < adu->data_len - 2 && i < bytecount + 3; i += 2) {
        uint16_t reg_val = modbus_adu_half_word(adu, i);
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "0x%04X / %d", reg_val, reg_val);
        modbus_adu_puti(di, s, adu, i + 1, ANN_SC_DATA, rbuf);
    }

    modbus_adu_check_crc(di, s, adu, bytecount + 4, ANN_SC_CRC, ANN_SC_ERROR);
}

static void modbus_sc_parse_write_single_coil(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 8;
    modbus_adu_puti(di, s, adu, 1, ANN_SC_FUNCTION, "Function 5: Write Single Coil");

    uint16_t address = modbus_adu_half_word(adu, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Address 0x%X / %d", address, address + 10000);
    modbus_adu_puti(di, s, adu, 3, ANN_SC_ADDRESS, buf);

    uint16_t raw_value = modbus_adu_half_word(adu, 4);
    const char *value = (raw_value == 0x0000) ? "Coil Value OFF" :
                        (raw_value == 0xFF00) ? "Coil Value ON" : "Invalid Coil Value";
    modbus_adu_puti(di, s, adu, 5, ANN_SC_DATA, value);

    modbus_adu_check_crc(di, s, adu, 7, ANN_SC_CRC, ANN_SC_ERROR);
}

static void modbus_sc_parse_write_single_register(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 8;
    modbus_adu_puti(di, s, adu, 1, ANN_SC_FUNCTION, "Function 6: Write Single Register");

    uint16_t address = modbus_adu_half_word(adu, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Address 0x%X / %d", address, address + 30000);
    modbus_adu_puti(di, s, adu, 3, ANN_SC_ADDRESS, buf);

    uint16_t value = modbus_adu_half_word(adu, 4);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "Register Value 0x%X / %d", value, value);
    modbus_adu_puti(di, s, adu, 5, ANN_SC_DATA, vbuf);

    modbus_adu_check_crc(di, s, adu, 7, ANN_SC_CRC, ANN_SC_ERROR);
}

static void modbus_sc_parse_write_multiple(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 8;
    uint8_t function = adu->data[1];
    const char *data_unit = (function == 15) ? "Coils" : "Registers";
    int long_offset = (function == 15) ? 10001 : 30001;
    char buf[256];

    snprintf(buf, sizeof(buf), "Function %d: Write Multiple %s", function, data_unit);
    modbus_adu_puti(di, s, adu, 1, ANN_SC_FUNCTION, buf);

    uint16_t address = modbus_adu_half_word(adu, 2);
    snprintf(buf, sizeof(buf), "Start at address 0x%X / %d", address, long_offset + address);
    modbus_adu_puti(di, s, adu, 3, ANN_SC_ADDRESS, buf);

    uint16_t quantity = modbus_adu_half_word(adu, 4);
    snprintf(buf, sizeof(buf), "Write %d %s", quantity, data_unit);
    modbus_adu_puti(di, s, adu, 5, ANN_SC_DATA, buf);

    modbus_adu_check_crc(di, s, adu, 7, ANN_SC_CRC, ANN_SC_ERROR);
}

static void modbus_sc_parse_error(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 5;
    int functioncode = adu->data[1] - 0x80;
    char buf[256];
    snprintf(buf, sizeof(buf), "Error for function %d", functioncode);
    modbus_adu_puti(di, s, adu, 1, ANN_SC_FUNCTION, buf);

    int error = adu->data[2];
    const char *errorcodes[] = {
        [1] = "Illegal Function", [2] = "Illegal Data Address",
        [3] = "Illegal Data Value", [4] = "Slave Device Failure",
        [5] = "Acknowledge", [6] = "Slave Device Busy",
        [8] = "Memory Parity Error", [10] = "Gateway Path Unavailable",
        [11] = "Gateway Target Device failed to respond",
    };
    const char *ename = (error >= 1 && error <= 11 && errorcodes[error]) ?
                        errorcodes[error] : "Unknown";
    snprintf(buf, sizeof(buf), "Error %d: %s", error, ename);
    modbus_adu_puti(di, s, adu, 2, ANN_SC_DATA, buf);

    modbus_adu_check_crc(di, s, adu, 4, ANN_SC_CRC, ANN_SC_ERROR);
}

static void modbus_sc_parse(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    if (adu->data_len < 1)
        return;

    /* Server ID */
    uint8_t server_id = adu->data[0];
    char buf[64];
    if (server_id >= 1 && server_id <= 247) {
        snprintf(buf, sizeof(buf), "Slave ID: %d", server_id);
    } else {
        snprintf(buf, sizeof(buf), "Slave ID {} is invalid");
    }
    modbus_adu_puti(di, s, adu, 0, ANN_SC_SERVER_ID, buf);

    if (adu->data_len < 2)
        return;

    uint8_t function = adu->data[1];
    if (function == 1 || function == 2)
        modbus_sc_parse_read_bits(di, s, adu);
    else if (function == 3 || function == 4 || function == 23)
        modbus_sc_parse_read_registers(di, s, adu);
    else if (function == 5)
        modbus_sc_parse_write_single_coil(di, s, adu);
    else if (function == 6)
        modbus_sc_parse_write_single_register(di, s, adu);
    else if (function == 15 || function == 16)
        modbus_sc_parse_write_multiple(di, s, adu);
    else if (function > 0x80)
        modbus_sc_parse_error(di, s, adu);
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unknown function: %d", function);
        modbus_adu_puti(di, s, adu, 1, ANN_SC_ERROR, buf);
        modbus_adu_puti(di, s, adu, adu->data_len - 1, ANN_SC_ERROR, "Unknown function");
    }
}

/* --- CS (Client -> Server) parsing --- */

static void modbus_cs_parse_read_data_command(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 8;
    uint8_t function = adu->data[1];
    const char *fn_names[] = {
        [1] = "Read Coils", [2] = "Read Discrete Inputs",
        [3] = "Read Holding Registers", [4] = "Read Input Registers",
    };
    const char *fn_name = (function >= 1 && function <= 4) ? fn_names[function] : "Unknown";
    char buf[256];
    snprintf(buf, sizeof(buf), "Function %d: %s", function, fn_name);
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, buf);

    uint16_t address = modbus_adu_half_word(adu, 2);
    int address_name = 10000 * function + 1 + address;
    snprintf(buf, sizeof(buf), "Start at address 0x%X / %d", address, address_name);
    modbus_adu_puti(di, s, adu, 3, ANN_CS_ADDRESS, buf);

    uint16_t quantity = modbus_adu_half_word(adu, 4);
    snprintf(buf, sizeof(buf), "Read %d units of data", quantity);
    modbus_adu_puti(di, s, adu, 5, ANN_CS_LENGTH, buf);

    modbus_adu_check_crc(di, s, adu, 7, ANN_CS_CRC, ANN_CS_ERROR);
}

static void modbus_cs_parse_write_single_coil(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 8;
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, "Function 5: Write Single Coil");

    uint16_t address = modbus_adu_half_word(adu, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Address 0x%X / %d", address, address + 10000);
    modbus_adu_puti(di, s, adu, 3, ANN_CS_ADDRESS, buf);

    uint16_t raw_value = modbus_adu_half_word(adu, 4);
    const char *value = (raw_value == 0x0000) ? "Coil Value OFF" :
                        (raw_value == 0xFF00) ? "Coil Value ON" : "Invalid Coil Value";
    modbus_adu_puti(di, s, adu, 5, ANN_CS_DATA, value);

    modbus_adu_check_crc(di, s, adu, 7, ANN_CS_CRC, ANN_CS_ERROR);
}

static void modbus_cs_parse_write_single_register(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 8;
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, "Function 6: Write Single Register");

    uint16_t address = modbus_adu_half_word(adu, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Address 0x%X / %d", address, address + 30000);
    modbus_adu_puti(di, s, adu, 3, ANN_CS_ADDRESS, buf);

    uint16_t value = modbus_adu_half_word(adu, 4);
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "Register Value 0x%X / %d", value, value);
    modbus_adu_puti(di, s, adu, 5, ANN_CS_DATA, vbuf);

    modbus_adu_check_crc(di, s, adu, 7, ANN_CS_CRC, ANN_CS_ERROR);
}

static void modbus_cs_parse_single_byte_request(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    uint8_t function = adu->data[1];
    const char *fn_names[] = {
        [7] = "Read Exception Status", [11] = "Get Comm Event Counter",
        [12] = "Get Comm Event Log", [17] = "Report Slave ID",
    };
    const char *fn_name = (function == 7 || function == 11 || function == 12 || function == 17) ?
                          fn_names[function] : "Unknown";
    char buf[256];
    snprintf(buf, sizeof(buf), "Function %d: %s", function, fn_name);
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, buf);

    modbus_adu_check_crc(di, s, adu, 3, ANN_CS_CRC, ANN_CS_ERROR);
}

static void modbus_cs_parse_write_multiple(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 9;
    uint8_t function = adu->data[1];
    const char *data_unit = (function == 15) ? "Coils" : "Registers";
    int long_offset = (function == 15) ? 10001 : 30001;
    char buf[256];

    snprintf(buf, sizeof(buf), "Function %d: Write Multiple %s", function, data_unit);
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, buf);

    uint16_t address = modbus_adu_half_word(adu, 2);
    snprintf(buf, sizeof(buf), "Start at address 0x%X / %d", address, long_offset + address);
    modbus_adu_puti(di, s, adu, 3, ANN_CS_ADDRESS, buf);

    uint16_t quantity = modbus_adu_half_word(adu, 4);
    snprintf(buf, sizeof(buf), "Write %d %s", quantity, data_unit);
    modbus_adu_puti(di, s, adu, 5, ANN_CS_LENGTH, buf);

    if (adu->data_len > 6) {
        int bytecount = adu->data[6];
        snprintf(buf, sizeof(buf), "Byte count: %d", bytecount);
        modbus_adu_puti(di, s, adu, 6, ANN_CS_LENGTH, buf);

        /* Data */
        for (int i = 7; i < adu->data_len - 2 && i < 6 + bytecount + 1; i++) {
            char dbuf[32];
            snprintf(dbuf, sizeof(dbuf), "Value 0x%X", adu->data[i]);
            modbus_adu_puti(di, s, adu, i, ANN_CS_DATA, dbuf);
        }

        adu->minimum_length = bytecount + 9;
        modbus_adu_check_crc(di, s, adu, bytecount + 8, ANN_CS_CRC, ANN_CS_ERROR);
    }
}

static void modbus_cs_parse_mask_write_register(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 10;
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, "Function 22: Mask Write Register");

    uint16_t address = modbus_adu_half_word(adu, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Address 0x%X / %d", address, address + 30001);
    modbus_adu_puti(di, s, adu, 3, ANN_CS_ADDRESS, buf);

    if (adu->data_len > 5) {
        { char b1[9], b2[9]; fmt_binary(adu->data[4], b1, 9); fmt_binary(adu->data[5], b2, 9); snprintf(buf, sizeof(buf), "AND mask: %s %s", b1, b2); }
        modbus_adu_puti(di, s, adu, 5, ANN_CS_DATA, buf);
    }
    if (adu->data_len > 7) {
        { char b1[9], b2[9]; fmt_binary(adu->data[6], b1, 9); fmt_binary(adu->data[7], b2, 9); snprintf(buf, sizeof(buf), "OR mask: %s %s", b1, b2); }
        modbus_adu_puti(di, s, adu, 7, ANN_CS_DATA, buf);
    }

    modbus_adu_check_crc(di, s, adu, 9, ANN_CS_CRC, ANN_CS_ERROR);
}

static void modbus_cs_parse_read_write_registers(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    adu->minimum_length = 13;
    modbus_adu_puti(di, s, adu, 1, ANN_CS_FUNCTION, "Function 23: Read/Write Multiple Registers");

    uint16_t r_address = modbus_adu_half_word(adu, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Read starting at address 0x%X / %d", r_address, 30001 + r_address);
    modbus_adu_puti(di, s, adu, 3, ANN_CS_ADDRESS, buf);

    uint16_t r_quantity = modbus_adu_half_word(adu, 4);
    snprintf(buf, sizeof(buf), "Read %d units of data", r_quantity);
    modbus_adu_puti(di, s, adu, 5, ANN_CS_LENGTH, buf);

    uint16_t w_address = modbus_adu_half_word(adu, 6);
    snprintf(buf, sizeof(buf), "Write starting at address 0x%X / %d", w_address, 30001 + w_address);
    modbus_adu_puti(di, s, adu, 7, ANN_CS_ADDRESS, buf);

    uint16_t w_quantity = modbus_adu_half_word(adu, 8);
    snprintf(buf, sizeof(buf), "Write %d registers", w_quantity);
    modbus_adu_puti(di, s, adu, 9, ANN_CS_LENGTH, buf);

    if (adu->data_len > 10) {
        int bytecount = adu->data[10];
        snprintf(buf, sizeof(buf), "Byte count: %d", bytecount);
        modbus_adu_puti(di, s, adu, 10, ANN_CS_LENGTH, buf);

        adu->minimum_length = bytecount + 13;
        modbus_adu_check_crc(di, s, adu, bytecount + 12, ANN_CS_CRC, ANN_CS_ERROR);
    }
}

static void modbus_cs_parse(struct srd_decoder_inst *di, modbus_state *s, modbus_adu *adu)
{
    if (adu->data_len < 1)
        return;

    /* Server ID */
    uint8_t server_id = adu->data[0];
    char buf[64];
    if (server_id == 0) {
        snprintf(buf, sizeof(buf), "Broadcast message");
    } else if (server_id >= 1 && server_id <= 247) {
        snprintf(buf, sizeof(buf), "Slave ID: %d", server_id);
    } else {
        snprintf(buf, sizeof(buf), "Slave ID: %d (reserved address)", server_id);
    }
    modbus_adu_puti(di, s, adu, 0, ANN_CS_SERVER_ID, buf);

    if (adu->data_len < 2)
        return;

    uint8_t function = adu->data[1];
    if (function >= 1 && function <= 4)
        modbus_cs_parse_read_data_command(di, s, adu);
    else if (function == 5)
        modbus_cs_parse_write_single_coil(di, s, adu);
    else if (function == 6)
        modbus_cs_parse_write_single_register(di, s, adu);
    else if (function == 7 || function == 11 || function == 12 || function == 17)
        modbus_cs_parse_single_byte_request(di, s, adu);
    else if (function == 15 || function == 16)
        modbus_cs_parse_write_multiple(di, s, adu);
    else if (function == 22)
        modbus_cs_parse_mask_write_register(di, s, adu);
    else if (function == 23)
        modbus_cs_parse_read_write_registers(di, s, adu);
    else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unknown function: %d", function);
        modbus_adu_puti(di, s, adu, 1, ANN_CS_ERROR, buf);
        modbus_adu_puti(di, s, adu, adu->data_len - 1, ANN_CS_ERROR, "Unknown function");
    }
}

static void modbus_decode_adu(struct srd_decoder_inst *di, modbus_state *s,
                              modbus_adu *adu, uint64_t start_sample, uint64_t end_sample,
                              uint8_t byte_val, int is_sc)
{
    /* Check frame gap */
    if (adu->data_len > 0 && adu->last_read > 0 && s->bitlength > 0) {
        double gap = (double)(start_sample - adu->last_read) / s->bitlength;
        if (gap > s->framegap) {
            /* Close old frame */
            uint64_t overflow_es = adu->ends[adu->data_len - 1] + (uint64_t)(s->bitlength * 3);
            if (is_sc)
                modbus_adu_close(di, s, adu, ANN_SC_ERROR, ANN_ERROR_INDICATION, overflow_es);
            else
                modbus_adu_close(di, s, adu, ANN_CS_ERROR, ANN_ERROR_INDICATION, overflow_es);
            modbus_adu_reset(adu, start_sample);
        }
    }

    /* Add byte */
    if (adu->data_len < MODBUS_MAX_FRAME) {
        adu->data[adu->data_len] = byte_val;
        adu->starts[adu->data_len] = start_sample;
        adu->ends[adu->data_len] = end_sample;
        adu->data_len++;
    }
    adu->last_read = end_sample;

    /* Parse */
    if (is_sc)
        modbus_sc_parse(di, s, adu);
    else
        modbus_cs_parse(di, s, adu);
}

static void modbus_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    modbus_state *s = (modbus_state *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Get bitlength from STARTBIT/STOPBIT */
    if (s->bitlength == 0) {
        if (strcmp(cmd, "STARTBIT") == 0 || strcmp(cmd, "STOPBIT") == 0) {
            s->bitlength = (double)(end_sample - start_sample);
        } else {
            return;
        }
    }

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;
    uint8_t rxtx = fields[1].u8;

    /* Dispatch to SC or CS ADU */
    if (rxtx == s->sc_channel) {
        if (s->adu_sc.start_new_frame || s->adu_sc.data_len == 0) {
            modbus_adu_reset(&s->adu_sc, start_sample);
            s->adu_sc.start_new_frame = 0;
        }
        modbus_decode_adu(di, s, &s->adu_sc, start_sample, end_sample, byte_val, 1);
    }
    if (rxtx == s->cs_channel) {
        if (s->adu_cs.start_new_frame || s->adu_cs.data_len == 0) {
            modbus_adu_reset(&s->adu_cs, start_sample);
            s->adu_cs.start_new_frame = 0;
        }
        modbus_decode_adu(di, s, &s->adu_cs, start_sample, end_sample, byte_val, 0);
    }
}

static void modbus_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(modbus_state)));
    }
    modbus_state *s = (modbus_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(modbus_state));
    s->sc_channel = 0; /* RX */
    s->cs_channel = 1; /* TX */
    s->framegap = 28;
}

static void modbus_start(struct srd_decoder_inst *di)
{
    modbus_state *s = (modbus_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "modbus");

    const char *sc = c_opt_str(di, "scchannel", "RX");
    const char *cs = c_opt_str(di, "cschannel", "TX");
    s->sc_channel = (strcmp(sc, "TX") == 0) ? 1 : 0;
    s->cs_channel = (strcmp(cs, "TX") == 0) ? 1 : 0;
    s->framegap = (int)c_opt_int(di, "framegap", 28);
}

static void modbus_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void modbus_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder modbus_c_decoder = {
    .id = "modbus_c",
    .name = "Modbus(C)",
    .longname = "Modbus RTU over RS232/RS485 (C)",
    .desc = "Modbus RTU protocol for industrial applications. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = modbus_options,
    .num_options = 3,
    .num_annotations = NUM_ANN,
    .ann_labels = modbus_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = modbus_ann_rows,
    .inputs = modbus_inputs,
    .num_inputs = 1,
    .outputs = modbus_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = modbus_tags,
    .num_tags = 1,
    .reset = modbus_reset,
    .start = modbus_start,
    .decode = modbus_decode,
    .destroy = modbus_destroy,
    .decode_upper = modbus_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    modbus_options[0].def = g_variant_new_string("RX");
    GSList *sc_vals = NULL;
    sc_vals = g_slist_append(sc_vals, g_variant_new_string("RX"));
    sc_vals = g_slist_append(sc_vals, g_variant_new_string("TX"));
    modbus_options[0].values = sc_vals;

    modbus_options[1].def = g_variant_new_string("TX");
    GSList *cs_vals = NULL;
    cs_vals = g_slist_append(cs_vals, g_variant_new_string("RX"));
    cs_vals = g_slist_append(cs_vals, g_variant_new_string("TX"));
    modbus_options[1].values = cs_vals;

    modbus_options[2].def = g_variant_new_int64(28);

    return &modbus_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}