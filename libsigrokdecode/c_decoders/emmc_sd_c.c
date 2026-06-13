/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2025 DreamSourceLab <support@dreamsourcelab.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMMC_SD_MAX_TOKEN_BITS 136

#define CH_CMD 0
#define CH_CLK 1

enum emmc_sd_state {
    STATE_GET_COMMAND_TOKEN = 0,
    STATE_HANDLE_CMD0 = 1,
    STATE_HANDLE_CMD999 = 64,
    STATE_GET_RESPONSE_R1 = 65,
    STATE_GET_RESPONSE_R1B = 66,
    STATE_GET_RESPONSE_R2 = 67,
    STATE_GET_RESPONSE_R3 = 68,
    STATE_GET_RESPONSE_R4 = 69,
    STATE_GET_RESPONSE_R5 = 70,
};

enum emmc_sd_ann {
    ANN_CMD0 = 0,
    /* CMD1-CMD63: 1-63 */
    ANN_BITS = 64,
    ANN_F_START = 65,
    ANN_F_TRANSMISSION = 66,
    ANN_F_CMD = 67,
    ANN_F_ARG = 68,
    ANN_F_CRC = 69,
    ANN_F_END = 70,
    ANN_DECODED_BIT = 71,
    ANN_DECODED_F = 72,
    NUM_ANN = 73,
};

struct emmc_bit {
    uint64_t ss;
    uint64_t es;
    int val;
};

struct emmc_sd_priv {
    int state;
    struct emmc_bit token[EMMC_SD_MAX_TOKEN_BITS];
    int token_len;
    int cmd;
    int last_cmd;
    uint32_t arg;
    uint8_t crc;
    char cmd_str[64];
    int out_ann;
};

static struct srd_channel emmc_sd_channels[] = {
    { "cmd", "CMD", "Command", 0, SRD_CHANNEL_SDATA, NULL },
    { "clk", "CLK", "Clock", 1, SRD_CHANNEL_SCLK, NULL },
};

static const char *emmc_cmd_names[] = {
    "GO_IDLE_STATE", "SEND_OP_COND", "ALL_SEND_CID", "SET_RELATIVE_ADDR",
    "SET_DSR", "SLEEP_AWAKE", "SWITCH", "SELECT/DESELECT_CARD",
    "SEND_EXT_CSD", "SEND_CSD", "SEND_CID", "OBSOLETE",
    "STOP_TRANSMISSION", "SEND_STATUS", "BUSTEST_R", "GO_INACTIVE_STATE",
    "SET_BLOCKLEN", "READ_SINGLE_BLOCK", "READ_MULTIPLE_BLOCK", "BUSTEST_W",
    "OBSOLETE", "SEND_TUNING_BLOCK", "Reserved", "SET_BLOCK_COUNT",
    "WRITE_BLOCK", "WRITE_MULTIPLE_BLOCK", "PROGRAM_CID", "PROGRAM_CSD",
    "SET_WRITE_PROT", "CLR_WRITE_PROT", "SEND_WRITE_PROT", "SEND_WRITE_PROT_TYPE",
    "Reserved", "Reserved", "Reserved", "ERASE_GROUP_START",
    "ERASE_GROUP_END", "Reserved", "ERASE", "FAST_IO",
    "GO_IRQ_STATE", "Reserved", "LOCK_UNLOCK", "Reserved",
    "QUEUED_TASK_PARAMS", "QUEUED_TASK_ADDRESS", "EXECUTE_READ_TASK", "EXECUTE_WRITE_TASK",
    "CMDQ_TASK_MGMT", "SET_TIME", "Reserved", "Reserved",
    "Reserved", "PROTOCOL_RD", "PROTOCOL_WR", "APP_CMD",
    "GEN_CMD", "Reserved", "Reserved", "Reserved",
    "Reserved for manufacturer", "Reserved for manufacturer", "Reserved for manufacturer", "Reserved for manufacturer",
};

static const char *device_status[] = {
    "Reserved for manufacturer test mode",
    "Reserved for manufacturer test mode",
    "Reserved for application specific commands",
    "AKE_SEQ_ERROR",
    "Reserved",
    "APP_CMD",
    "EXCEPTION_EVENT",
    "SWITCH_ERROR",
    "READY_FOR_DATA",
    "CURRENT_STATE",
    "CURRENT_STATE",
    "CURRENT_STATE",
    "CURRENT_STATE",
    "ERASE_RESET",
    "Reserved(must be 0)",
    "WP_ERASE_SKIP",
    "CIS/CSD_OVERWRITE",
    "Obsolete",
    "Obsolete",
    "ERROR",
    "CC_ERROR",
    "DEVICE_ECC_FAILED",
    "ILLEGAL_COMMAND",
    "COM_CRC_ERROR",
    "LOCK_UNLOCK_FAILED",
    "DEVICE_IS_LOCKED",
    "WP_VIOLATION",
    "ERASE_PARAM",
    "ERASE_SEQ_ERROR",
    "BLOCK_LEN_ERROR",
    "ADDR_MISALIGN",
    "ADDR_OUT_OF_RANGE",
};

static const char *emmc_cmd_name(int cmd)
{
    if (cmd >= 0 && cmd < 64)
        return emmc_cmd_names[cmd];
    return "Unknown";
}


static const char *emmc_ann_label_ptrs[NUM_ANN][3];
static int emmc_ann_labels_initialized = 0;

static void emmc_init_ann_labels(void)
{
    if (emmc_ann_labels_initialized)
        return;
    emmc_ann_labels_initialized = 1;

    for (int i = 0; i < 64; i++) {
        emmc_ann_label_ptrs[i][0] = "";
        emmc_ann_label_ptrs[i][1] = g_strdup_printf("CMD%d", i);
        emmc_ann_label_ptrs[i][2] = g_strdup_printf("CMD%d", i);
    }
    emmc_ann_label_ptrs[64][0] = "Bits"; emmc_ann_label_ptrs[64][1] = "Bits"; emmc_ann_label_ptrs[64][2] = "Bits";
    emmc_ann_label_ptrs[65][0] = "Start bit"; emmc_ann_label_ptrs[65][1] = "Start"; emmc_ann_label_ptrs[65][2] = "S";
    emmc_ann_label_ptrs[66][0] = "Transmission bit"; emmc_ann_label_ptrs[66][1] = "Transmission bit"; emmc_ann_label_ptrs[66][2] = "T";
    emmc_ann_label_ptrs[67][0] = "Command"; emmc_ann_label_ptrs[67][1] = "Command"; emmc_ann_label_ptrs[67][2] = "Cmd";
    emmc_ann_label_ptrs[68][0] = "Argument"; emmc_ann_label_ptrs[68][1] = "Argument"; emmc_ann_label_ptrs[68][2] = "Arg";
    emmc_ann_label_ptrs[69][0] = "CRC"; emmc_ann_label_ptrs[69][1] = "CRC"; emmc_ann_label_ptrs[69][2] = "CRC";
    emmc_ann_label_ptrs[70][0] = "End bit"; emmc_ann_label_ptrs[70][1] = "End"; emmc_ann_label_ptrs[70][2] = "E";
    emmc_ann_label_ptrs[71][0] = "Decoded bit"; emmc_ann_label_ptrs[71][1] = "Decoded bit"; emmc_ann_label_ptrs[71][2] = "Decoded bit";
    emmc_ann_label_ptrs[72][0] = "Decoded field"; emmc_ann_label_ptrs[72][1] = "Decoded field"; emmc_ann_label_ptrs[72][2] = "Decoded field";
}

static void emmc_putt(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cls, const char *text)
{
    if (s->token_len < 48) return;
    c_put(di, s->token[0].ss, s->token[47].es, s->out_ann, cls, text);
}

static void emmc_putf(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int start, int end, int cls, const char *text)
{
    if (start >= s->token_len || end >= s->token_len) return;
    c_put(di, s->token[start].ss, s->token[end].es, s->out_ann, cls, text);
}

static void emmc_puta4(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int start, int end, int cls,
                        const char *t1, const char *t2, const char *t3, const char *t4)
{
    if (s->token_len < 48) return;
    int s_idx = 47 - 8 - end;
    int e_idx = 47 - 8 - start;
    if (s_idx < 0 || e_idx < 0 || s_idx >= s->token_len || e_idx >= s->token_len) return;
    if (t2 && t3 && t4)
        c_put(di, s->token[s_idx].ss, s->token[e_idx].es, s->out_ann, cls, t1, t2, t3, t4);
    else if (t2 && t3)
        c_put(di, s->token[s_idx].ss, s->token[e_idx].es, s->out_ann, cls, t1, t2, t3);
    else if (t2)
        c_put(di, s->token[s_idx].ss, s->token[e_idx].es, s->out_ann, cls, t1, t2);
    else
        c_put(di, s->token[s_idx].ss, s->token[e_idx].es, s->out_ann, cls, t1);
}


static void emmc_putc(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd, const char *desc)
{
    s->last_cmd = cmd;
    if (s->token_len < 48) return;
    char text[128];
    snprintf(text, sizeof(text), "%s: %s", s->cmd_str, desc);
    /* Extract short form (e.g., "CMD0" from "CMD0 (GO_IDLE_STATE)") */
    char short_str[64];
    const char *space = strchr(s->cmd_str, ' ');
    if (space) {
        int len = (int)(space - s->cmd_str);
        if (len >= (int)sizeof(short_str)) len = (int)sizeof(short_str) - 1;
        memcpy(short_str, s->cmd_str, len);
        short_str[len] = '\0';
    } else {
        snprintf(short_str, sizeof(short_str), "%s", s->cmd_str);
    }
    c_put(di, s->token[0].ss, s->token[47].es, s->out_ann, cmd, text, s->cmd_str, short_str);
}

static void emmc_putr(struct srd_decoder_inst *di, struct emmc_sd_priv *s, const char *desc)
{
    char text[64];
    snprintf(text, sizeof(text), "Reply: %s", desc);
    emmc_putt(di, s, s->last_cmd, text);
}

static int emmc_get_token_bits(struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum, int n)
{
    if (s->token_len < EMMC_SD_MAX_TOKEN_BITS) {
        s->token[s->token_len].ss = samplenum;
        s->token[s->token_len].es = samplenum;
        s->token[s->token_len].val = cmd_pin;
        if (s->token_len > 0)
            s->token[s->token_len - 1].es = samplenum;
        s->token_len++;
    }
    if (s->token_len < n)
        return 0;
    s->token[n - 1].es += s->token[n - 1].ss - s->token[n - 2].ss;
    return 1;
}

static void emmc_handle_common_token_fields(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    for (int bit = 0; bit < s->token_len; bit++) {
        char text[4];
        snprintf(text, sizeof(text), "%d", s->token[bit].val);
        emmc_putf(di, s, bit, bit, ANN_DECODED_BIT, text);
    }

    c_put(di, s->token[0].ss, s->token[0].es, s->out_ann, ANN_F_START, "Start bit", "Start", "S");

    const char *t = (s->token_len > 1 && s->token[1].val == 1) ? "host" : "card";
    const char *t_short = (s->token_len > 1 && s->token[1].val == 1) ? "H" : "C";
    char text[64];
    snprintf(text, sizeof(text), "Transmission: %s", t);
    char text2[32];
    snprintf(text2, sizeof(text2), "T: %s", t);
    c_put(di, s->token[1].ss, s->token[1].es, s->out_ann, ANN_F_TRANSMISSION, text, text2, t_short);

    s->cmd = 0;
    for (int i = 2; i < 8 && i < s->token_len; i++)
        s->cmd = (s->cmd << 1) | s->token[i].val;
    const char *cname = emmc_cmd_name(s->cmd);
    char cmd_text[64], cmd_text2[64], cmd_text3[16];
    snprintf(cmd_text, sizeof(cmd_text), "Command: %s (%d)", cname, s->cmd);
    snprintf(cmd_text2, sizeof(cmd_text2), "Cmd: %s (%d)", cname, s->cmd);
    snprintf(cmd_text3, sizeof(cmd_text3), "CMD%d", s->cmd);
    c_put(di, s->token[2].ss, s->token[7].es, s->out_ann, ANN_F_CMD, cmd_text, cmd_text2, cmd_text3, "Cmd", "C");

    s->arg = 0;
    for (int i = 8; i < 40 && i < s->token_len; i++)
        s->arg = (s->arg << 1) | s->token[i].val;
    char arg_text[64];
    snprintf(arg_text, sizeof(arg_text), "Argument: 0x%08x", s->arg);
    c_put(di, s->token[8].ss, s->token[39].es, s->out_ann, ANN_F_ARG, arg_text, "Arg", "A");

    s->crc = 0;
    for (int i = 40; i < 47 && i < s->token_len; i++)
        s->crc = (s->crc << 1) | s->token[i].val;
    char crc_text[32];
    snprintf(crc_text, sizeof(crc_text), "CRC: 0x%x", s->crc);
    c_put(di, s->token[40].ss, s->token[46].es, s->out_ann, ANN_F_CRC, crc_text, "CRC", "C");

    c_put(di, s->token[47].ss, s->token[47].es, s->out_ann, ANN_F_END, "End bit", "End", "E");
}

static void emmc_get_command_token(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 48))
        return;

    emmc_handle_common_token_fields(di, s);

    snprintf(s->cmd_str, sizeof(s->cmd_str), "CMD%d (%s)", s->cmd, emmc_cmd_name(s->cmd));

    switch (s->cmd) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
    case 8: case 9: case 10: case 12: case 13: case 14: case 15: case 16:
    case 17: case 18: case 19: case 21: case 23: case 24: case 25: case 26:
    case 27: case 28: case 29: case 30: case 31: case 35: case 36: case 38:
    case 39: case 40: case 42: case 44: case 45: case 46: case 47: case 48:
    case 49: case 53: case 54: case 55: case 56:
        s->state = STATE_HANDLE_CMD0 + s->cmd;
        break;
    default:
        s->state = STATE_HANDLE_CMD999;
        emmc_putc(di, s, s->cmd, s->cmd_str);
        break;
    }
}

static void emmc_handle_cmd0(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "IDLE_STATE", "IDLE", "ID", "I");
    emmc_putc(di, s, 0, "Reset Device to IDLE_STATE");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_cmd1(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "OCR_WO_BUSY", "OCR", NULL, NULL);
    emmc_putc(di, s, 1, "Send OCR in idle state");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R3;
}

static void emmc_handle_cmd2(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 2, "Ask card for CID number");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R2;
}

static void emmc_handle_cmd3(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "Set Relative Card Addr", "Set RCA", "SRCA", "SR");
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 3, "Set relative card address (RCA)");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd4(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "Set DSR", "SDSR", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 4, "Programs the DSR of the Device");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_cmd5(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "Set DSR", "SDSR", NULL, NULL);
    emmc_puta4(di, s, 15, 15, ANN_DECODED_F, "Sleep/Awake", "S/A", NULL, NULL);
    emmc_puta4(di, s, 0, 14, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 5, "Sleep/Awake");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd6(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 26, 31, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
    emmc_puta4(di, s, 24, 25, ANN_DECODED_F, "Access", "A", NULL, NULL);
    emmc_puta4(di, s, 16, 23, ANN_DECODED_F, "Index", "Id", NULL, NULL);
    emmc_puta4(di, s, 8, 15, ANN_DECODED_F, "Value", "Val", "V", NULL);
    emmc_puta4(di, s, 3, 7, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
    emmc_puta4(di, s, 0, 2, ANN_DECODED_F, "CMD Set", "CMD S", NULL, NULL);
    emmc_putc(di, s, 6, "Switch card function");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd7(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_putc(di, s, 7, "Select / deselect card");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd8(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 8, "Device sends its EXT_CSD register");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd9(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 9, "Send card-specific data (CSD)");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R2;
}

static void emmc_handle_cmd10(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 10, "Send card identification data (CID)");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R2;
}

static void emmc_handle_cmd12(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 1, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_puta4(di, s, 0, 0, ANN_DECODED_F, "HPI", NULL, NULL, NULL);
    emmc_putc(di, s, 12, "Forces the Device to stop transmission");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd13(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 15, 15, ANN_DECODED_F, "SQS", NULL, NULL, NULL);
    emmc_puta4(di, s, 1, 14, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_puta4(di, s, 0, 0, ANN_DECODED_F, "HPI", NULL, NULL, NULL);
    emmc_putc(di, s, 13, "Send card status register");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd14(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 14, "Host Bus Test read from Device");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd15(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 15, "Set Device at RCA to Inactive State");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_cmd16(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Block length", "Blocklen", "BL", "B");
    char text[64];
    snprintf(text, sizeof(text), "Read the block length to %u bytes", s->arg);
    emmc_putc(di, s, 16, text);
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd17(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 17, "Read a block of data set by SET_BLOCKLEN");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd18(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 18, "Read Multiple blocks of data");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd19(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 19, "Host Bus Test Write to Device");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd21(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 21, "128 clocks of tuning pattern for HS200");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd23(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 30, 30, ANN_DECODED_F, "Packed", NULL, NULL, NULL);
    if (s->token_len > 30 && s->token[30].val == 1) {
        emmc_puta4(di, s, 31, 31, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
        emmc_puta4(di, s, 16, 29, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
    } else {
        emmc_puta4(di, s, 31, 31, ANN_DECODED_F, "Reliable Write", "RLB W", NULL, NULL);
        emmc_puta4(di, s, 25, 28, ANN_DECODED_F, "Context ID", "CntxtID", NULL, NULL);
        emmc_puta4(di, s, 24, 24, ANN_DECODED_F, "Forced Programming", "Forced Prog", "FP", NULL);
        emmc_puta4(di, s, 16, 23, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
    }
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Number of Blocks", "NUM BLK", "NoB", NULL);
    emmc_putc(di, s, 23, "Defines the number of blocks");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd24(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 24, "Writes a block of Data");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd25(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 25, "Writes multiple blocks of Data");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd26(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 26, "Programming of the Device identification register");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd27(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 27, "Programming of the programmable bits of the CSD");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd28(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 28, "Set Write Protect or Release address group");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd29(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 29, "Clear Write Protect or Ignored");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd30(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 30, "Send status of Write Protect or released group");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd31(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 31, "Send type of Write Protect or 64bit 0s");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd35(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 35, "Set Address of the 1st erase group");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd36(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Data Address", "Dat Addr", "DADD", "DA");
    emmc_putc(di, s, 36, "Set Address of the last erase group");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd38(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 31, 31, ANN_DECODED_F, "Secure Request", "Sec Req", "SR", NULL);
    emmc_puta4(di, s, 16, 30, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
    emmc_puta4(di, s, 15, 15, ANN_DECODED_F, "Force Garbage Collect", "F Garb Clct", "FGC", NULL);
    emmc_puta4(di, s, 2, 14, ANN_DECODED_F, "Set to 0", "Set 0", "S0", "Z");
    emmc_puta4(di, s, 1, 1, ANN_DECODED_F, "Discard Enable", "DISG EN", "DE", NULL);
    emmc_puta4(di, s, 0, 0, ANN_DECODED_F, "TRIM Enable", "TRIM EN", "TE", NULL);
    emmc_putc(di, s, 38, "Erase all groups defined by CMD35/36");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd39(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 15, 15, ANN_DECODED_F, "Write Flag", "W Flag", "WF", "W");
    emmc_puta4(di, s, 8, 14, ANN_DECODED_F, "Register Addr", "REGADD", "RA", NULL);
    emmc_puta4(di, s, 0, 7, ANN_DECODED_F, "Register Data", "REGDAT", "DAT", "D");
    emmc_putc(di, s, 39, "R/W 8 bit (register) data fields");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R4;
}

static void emmc_handle_cmd40(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 40, "Sets the system into interrupt mode");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R5;
}

static void emmc_handle_cmd42(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 42, "set/reset the password or lock/unlock the Device");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd44(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 31, 31, ANN_DECODED_F, "Reliable Write Request", "RWR", NULL, NULL);
    emmc_puta4(di, s, 30, 30, ANN_DECODED_F, "Data Direction(1:R/0:W)", "DD", NULL, NULL);
    emmc_puta4(di, s, 29, 29, ANN_DECODED_F, "Tag request", "Tag Req", "TR", NULL);
    emmc_puta4(di, s, 25, 28, ANN_DECODED_F, "Context ID", "CntxtID", NULL, NULL);
    emmc_puta4(di, s, 24, 24, ANN_DECODED_F, "Forced Programming", "F Prog", "FP", NULL);
    emmc_puta4(di, s, 23, 23, ANN_DECODED_F, "Priority(0:Simple/1:High)", "Prio", NULL, NULL);
    emmc_puta4(di, s, 21, 22, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_puta4(di, s, 16, 20, ANN_DECODED_F, "Task ID", "TID", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Block Numbers", "Blk NUM", NULL, NULL);
    emmc_putc(di, s, 44, "Def data dir for R/W, Priority, Task ID, blk count for qd Task");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd45(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Start of block address", "Start BLK ADDR", "SBA", "SA");
    emmc_putc(di, s, 45, "Defines the block address of queued task");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd46(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 21, 31, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_puta4(di, s, 16, 20, ANN_DECODED_F, "Task ID", "TID", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_putc(di, s, 46, "execute task from the queue with TID");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd47(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 21, 31, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_puta4(di, s, 16, 20, ANN_DECODED_F, "Task ID", "TID", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_putc(di, s, 47, "execute task from the queue with TID");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd48(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 21, 31, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_puta4(di, s, 16, 20, ANN_DECODED_F, "Task ID", "TID", NULL, NULL);
    emmc_puta4(di, s, 4, 15, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_puta4(di, s, 0, 3, ANN_DECODED_F, "TM Op-code", "TMOP", NULL, NULL);
    emmc_putc(di, s, 48, "discard a specific task or entire queue");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1B;
}

static void emmc_handle_cmd49(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 49, "Sets the real time clock according to the RTC");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd53(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "Security Protocol Specific", "Sec P Spec", "SPS", NULL);
    emmc_puta4(di, s, 8, 15, ANN_DECODED_F, "Security Protocol", "Sec P", "SP", NULL);
    emmc_puta4(di, s, 0, 7, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_putc(di, s, 53, "Transfer 512B Blk from device to host");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd54(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "Security Protocol Specific", "Sec P Spec", "SPS", NULL);
    emmc_puta4(di, s, 8, 15, ANN_DECODED_F, "Security Protocol", "Sec P", "SP", NULL);
    emmc_puta4(di, s, 0, 7, ANN_DECODED_F, "Reserved", "RSVD", NULL, NULL);
    emmc_putc(di, s, 54, "Transfer 512B Blk from host to device");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd55(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_putc(di, s, 55, "Next command is an application-specific command");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd56(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    emmc_puta4(di, s, 1, 31, ANN_DECODED_F, "Stuff bits", "Stuff", "SB", "S");
    emmc_puta4(di, s, 0, 0, ANN_DECODED_F, "RD/WR", NULL, NULL, NULL);
    emmc_putc(di, s, 56, "R/W a data block from/to the Device");
    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_cmd999(struct srd_decoder_inst *di, struct emmc_sd_priv *s)
{
    (void)di;

    s->token_len = 0; s->state = STATE_GET_RESPONSE_R1;
}

static void emmc_handle_response_r1(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 48))
        return;
    emmc_handle_common_token_fields(di, s);
    emmc_putr(di, s, "R1");
    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Card status", "Status", "S", NULL);
    for (int i = 0; i < 32; i++) {
        if (8 + i < s->token_len)
            emmc_putf(di, s, 8 + i, 8 + i, ANN_DECODED_BIT, device_status[31 - i]);
    }
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_response_r1b(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 48))
        return;
    emmc_handle_common_token_fields(di, s);
    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "Card status", "Status", "S", NULL);
    emmc_putr(di, s, "R1b");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_response_r2(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 136))
        return;
    for (int bit = 0; bit < s->token_len; bit++) {
        char text[4];
        snprintf(text, sizeof(text), "%d", s->token[bit].val);
        emmc_putf(di, s, bit, bit, ANN_DECODED_BIT, text);
    }
    c_put(di, s->token[0].ss, s->token[0].es, s->out_ann, ANN_F_START, "Start bit", "Start", "S");
    const char *t = (s->token_len > 1 && s->token[1].val == 1) ? "host" : "card";
    const char *t_short = (s->token_len > 1 && s->token[1].val == 1) ? "H" : "C";
    char text[64];
    snprintf(text, sizeof(text), "Transmission: %s", t);
    char text2[32];
    snprintf(text2, sizeof(text2), "T: %s", t);
    c_put(di, s->token[1].ss, s->token[1].es, s->out_ann, ANN_F_TRANSMISSION, text, text2, t_short);
    c_put(di, s->token[2].ss, s->token[7].es, s->out_ann, ANN_F_CMD, "Check Bits", "CHECK", "C");
    c_put(di, s->token[8].ss, s->token[134].es, s->out_ann, ANN_F_ARG, "Argument", "Arg", "A");
    c_put(di, s->token[135].ss, s->token[135].es, s->out_ann, ANN_F_END, "End bit", "End", "E");
    c_put(di, s->token[8].ss, s->token[134].es, s->out_ann, ANN_DECODED_F, "CID/CSD register", "CID/CSD", "C");
    c_put(di, s->token[0].ss, s->token[135].es, s->out_ann, s->last_cmd < NUM_ANN ? s->last_cmd : 0, "R2");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_response_r3(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 48))
        return;
    emmc_putr(di, s, "R3");
    for (int bit = 0; bit < s->token_len; bit++) {
        char text[4];
        snprintf(text, sizeof(text), "%d", s->token[bit].val);
        emmc_putf(di, s, bit, bit, ANN_DECODED_BIT, text);
    }
    c_put(di, s->token[0].ss, s->token[0].es, s->out_ann, ANN_F_START, "Start bit", "Start", "S");
    const char *t = (s->token_len > 1 && s->token[1].val == 1) ? "host" : "card";
    const char *t_short = (s->token_len > 1 && s->token[1].val == 1) ? "H" : "C";
    char text[64];
    snprintf(text, sizeof(text), "Transmission: %s", t);
    char text2[32];
    snprintf(text2, sizeof(text2), "T: %s", t);
    c_put(di, s->token[1].ss, s->token[1].es, s->out_ann, ANN_F_TRANSMISSION, text, text2, t_short);
    c_put(di, s->token[2].ss, s->token[7].es, s->out_ann, ANN_F_CMD, "Check bits", "CHECK", "C");
    c_put(di, s->token[8].ss, s->token[39].es, s->out_ann, ANN_F_ARG, "Argument", "Arg", "A");
    c_put(di, s->token[40].ss, s->token[46].es, s->out_ann, ANN_F_CRC, "Check bits", "CHECK", "C");
    c_put(di, s->token[47].ss, s->token[47].es, s->out_ann, ANN_F_END, "End bit", "End", "E");
    emmc_puta4(di, s, 0, 31, ANN_DECODED_F, "OCR register", "OCR reg", "OCR", "O");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_response_r4(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 39))
        return;
    emmc_handle_common_token_fields(di, s);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Card status bits", "Status", "S", NULL);
    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "Relative card address", "RCA", "R", NULL);
    emmc_putr(di, s, "R4");
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_handle_response_r5(struct srd_decoder_inst *di, struct emmc_sd_priv *s, int cmd_pin, uint64_t samplenum)
{
    (void)samplenum;
    if (!emmc_get_token_bits(s, cmd_pin, samplenum, 40))
        return;
    emmc_handle_common_token_fields(di, s);
    emmc_putr(di, s, "R5");
    emmc_puta4(di, s, 16, 31, ANN_DECODED_F, "RCA", "R", NULL, NULL);
    emmc_puta4(di, s, 0, 15, ANN_DECODED_F, "Not Defined/IRQ data", "NDEF", NULL, NULL);
    s->token_len = 0; s->state = STATE_GET_COMMAND_TOKEN;
}

static void emmc_sd_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct emmc_sd_priv)));
    }
    struct emmc_sd_priv *s = (struct emmc_sd_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct emmc_sd_priv));
    s->state = STATE_GET_COMMAND_TOKEN;
    s->out_ann = -1;
}

static void emmc_sd_start(struct srd_decoder_inst *di)
{
    struct emmc_sd_priv *s = (struct emmc_sd_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "emmc_sd");
    emmc_init_ann_labels();
}

static void emmc_sd_decode(struct srd_decoder_inst *di)
{
    struct emmc_sd_priv *s = (struct emmc_sd_priv *)c_decoder_get_private(di);
    while (1) {
        int ret = c_wait(di, CW_R(CH_CLK), CW_END);
        if (ret != SRD_OK)
            return;

        int cmd_pin = c_pin(di, CH_CMD);

        if (s->state == STATE_GET_COMMAND_TOKEN) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_get_command_token(di, s, cmd_pin, di_samplenum(di));
        } else if (s->state >= STATE_HANDLE_CMD0 && s->state <= STATE_HANDLE_CMD0 + 63) {
            int cmdidx = s->state - STATE_HANDLE_CMD0;
            switch (cmdidx) {
            case 0: emmc_handle_cmd0(di, s); break;
            case 1: emmc_handle_cmd1(di, s); break;
            case 2: emmc_handle_cmd2(di, s); break;
            case 3: emmc_handle_cmd3(di, s); break;
            case 4: emmc_handle_cmd4(di, s); break;
            case 5: emmc_handle_cmd5(di, s); break;
            case 6: emmc_handle_cmd6(di, s); break;
            case 7: emmc_handle_cmd7(di, s); break;
            case 8: emmc_handle_cmd8(di, s); break;
            case 9: emmc_handle_cmd9(di, s); break;
            case 10: emmc_handle_cmd10(di, s); break;
            case 12: emmc_handle_cmd12(di, s); break;
            case 13: emmc_handle_cmd13(di, s); break;
            case 14: emmc_handle_cmd14(di, s); break;
            case 15: emmc_handle_cmd15(di, s); break;
            case 16: emmc_handle_cmd16(di, s); break;
            case 17: emmc_handle_cmd17(di, s); break;
            case 18: emmc_handle_cmd18(di, s); break;
            case 19: emmc_handle_cmd19(di, s); break;
            case 21: emmc_handle_cmd21(di, s); break;
            case 23: emmc_handle_cmd23(di, s); break;
            case 24: emmc_handle_cmd24(di, s); break;
            case 25: emmc_handle_cmd25(di, s); break;
            case 26: emmc_handle_cmd26(di, s); break;
            case 27: emmc_handle_cmd27(di, s); break;
            case 28: emmc_handle_cmd28(di, s); break;
            case 29: emmc_handle_cmd29(di, s); break;
            case 30: emmc_handle_cmd30(di, s); break;
            case 31: emmc_handle_cmd31(di, s); break;
            case 35: emmc_handle_cmd35(di, s); break;
            case 36: emmc_handle_cmd36(di, s); break;
            case 38: emmc_handle_cmd38(di, s); break;
            case 39: emmc_handle_cmd39(di, s); break;
            case 40: emmc_handle_cmd40(di, s); break;
            case 42: emmc_handle_cmd42(di, s); break;
            case 44: emmc_handle_cmd44(di, s); break;
            case 45: emmc_handle_cmd45(di, s); break;
            case 46: emmc_handle_cmd46(di, s); break;
            case 47: emmc_handle_cmd47(di, s); break;
            case 48: emmc_handle_cmd48(di, s); break;
            case 49: emmc_handle_cmd49(di, s); break;
            case 53: emmc_handle_cmd53(di, s); break;
            case 54: emmc_handle_cmd54(di, s); break;
            case 55: emmc_handle_cmd55(di, s); break;
            case 56: emmc_handle_cmd56(di, s); break;
            default: emmc_handle_cmd999(di, s); break;
            }
        } else if (s->state == STATE_HANDLE_CMD999) {
            emmc_handle_cmd999(di, s);
        } else if (s->state == STATE_GET_RESPONSE_R1) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_handle_response_r1(di, s, cmd_pin, di_samplenum(di));
        } else if (s->state == STATE_GET_RESPONSE_R1B) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_handle_response_r1b(di, s, cmd_pin, di_samplenum(di));
        } else if (s->state == STATE_GET_RESPONSE_R2) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_handle_response_r2(di, s, cmd_pin, di_samplenum(di));
        } else if (s->state == STATE_GET_RESPONSE_R3) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_handle_response_r3(di, s, cmd_pin, di_samplenum(di));
        } else if (s->state == STATE_GET_RESPONSE_R4) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_handle_response_r4(di, s, cmd_pin, di_samplenum(di));
        } else if (s->state == STATE_GET_RESPONSE_R5) {
            if (s->token_len == 0 && cmd_pin != 0)
                continue;
            emmc_handle_response_r5(di, s, cmd_pin, di_samplenum(di));
        }
    }
}

static void emmc_sd_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static const int emmc_row_raw_bits_classes[] = { ANN_BITS, -1 };
static const int emmc_row_decoded_bits_classes[] = { ANN_DECODED_BIT, -1 };
static const int emmc_row_decoded_fields_classes[] = { ANN_DECODED_F, -1 };
static const int emmc_row_fields_classes[] = { ANN_F_START, ANN_F_TRANSMISSION, ANN_F_CMD, ANN_F_ARG, ANN_F_CRC, ANN_F_END, -1 };
static const int emmc_row_cmds_classes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, -1 };

static const struct srd_c_ann_row emmc_sd_ann_rows[] = {
    { "raw-bits", "Raw bits", emmc_row_raw_bits_classes, 1 },
    { "decoded-bits", "Decoded bits", emmc_row_decoded_bits_classes, 1 },
    { "decoded-fields", "Decoded fields", emmc_row_decoded_fields_classes, 1 },
    { "fields", "Fields", emmc_row_fields_classes, 6 },
    { "cmds", "Commands", emmc_row_cmds_classes, 64 },
};

static const char *emmc_sd_inputs[] = { "logic" };
static const char *emmc_sd_tags[] = { "Memory" };

struct srd_c_decoder emmc_sd_c_decoder = {
    .id = "emmc_sd_c",
    .name = "eMMC (SD mode)(C)",
    .longname = "Embedded Multimedia card (SD mode) (C)",
    .desc = "Embedded Multimedia card (SD mode) low-level protocol.",
    .license = "gplv2+",
    .channels = emmc_sd_channels,
    .num_channels = 2,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = (const char *(*)[3])emmc_ann_label_ptrs,
    .num_annotation_rows = 5,
    .annotation_rows = emmc_sd_ann_rows,
    .inputs = emmc_sd_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = emmc_sd_tags,
    .num_tags = 1,
    .reset = emmc_sd_reset,
    .start = emmc_sd_start,
    .decode = emmc_sd_decode,
    .destroy = emmc_sd_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &emmc_sd_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}