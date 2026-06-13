/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015-2020 Uwe Hermann <uwe@hermann-uwe.de>
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

#define SDCARD_SD_MAX_TOKEN_BITS 136

#define CH_CMD 0
#define CH_CLK 1

enum sdcard_sd_state {
  STATE_GET_COMMAND_TOKEN = 0,
  STATE_HANDLE_CMD0 = 1,
  STATE_HANDLE_CMD999 = 64,
  STATE_HANDLE_ACMD0 = 65,
  STATE_HANDLE_ACMD999 = 128,
  STATE_GET_RESPONSE_R1 = 131,
  STATE_GET_RESPONSE_R1B = 132,
  STATE_GET_RESPONSE_R2 = 133,
  STATE_GET_RESPONSE_R3 = 134,
  STATE_GET_RESPONSE_R6 = 135,
  STATE_GET_RESPONSE_R7 = 136,
};

enum sdcard_sd_ann {
  ANN_CMD0 = 0,
  ANN_CMD2 = 2,
  ANN_CMD9 = 9,
  ANN_CMD10 = 10,
  ANN_ACMD0 = 64,
  ANN_RESPONSE_R1 = 128,
  ANN_RESPONSE_R1B = 129,
  ANN_RESPONSE_R2 = 130,
  ANN_RESPONSE_R3 = 131,
  ANN_RESPONSE_R6 = 132,
  ANN_RESPONSE_R7 = 133,
  ANN_R_STATUS_OUT_OF_RANGE = 134,
  ANN_R_STATUS_ADDRESS_ERROR = 135,
  ANN_R_STATUS_BLOCK_LEN_ERROR = 136,
  ANN_R_STATUS_ERASE_SEQ_ERROR = 137,
  ANN_R_STATUS_ERASE_PARAM = 138,
  ANN_R_STATUS_WP_VIOLATION = 139,
  ANN_R_STATUS_CARD_IS_LOCKED = 140,
  ANN_R_STATUS_LOCK_UNLOCK_FAILED = 141,
  ANN_R_STATUS_COM_CRC_ERROR = 142,
  ANN_R_STATUS_ILLEGAL_COMMAND = 143,
  ANN_R_STATUS_CARD_ECC_FAILED = 144,
  ANN_R_STATUS_CC_ERROR = 145,
  ANN_R_STATUS_ERROR = 146,
  ANN_R_STATUS_RSVD = 147,
  ANN_R_STATUS_RSVD_DEFERRED_RESPONSE = 148,
  ANN_R_STATUS_CSD_OVERWRITE = 149,
  ANN_R_STATUS_WP_ERASE_SKIP = 150,
  ANN_R_STATUS_CARD_ECC_DISABLED = 151,
  ANN_R_STATUS_ERASE_RESET = 152,
  ANN_R_STATUS_CURRENT_STATE = 153,
  ANN_R_STATUS_READY_FOR_DATA = 154,
  ANN_R_STATUS_FX_EVENT = 155,
  ANN_R_STATUS_APP_CMD = 156,
  ANN_R_STATUS_RSVD_SDIO = 157,
  ANN_R_STATUS_AKE_SEQ_ERROR = 158,
  ANN_R_STATUS_RSVD_APP_CMD = 159,
  ANN_R_STATUS_RSVD_TESTMODE = 160,
  ANN_R_CID_MID = 161,
  ANN_R_CID_OID = 162,
  ANN_R_CID_PNM = 163,
  ANN_R_CID_PRV = 164,
  ANN_R_CID_PSN = 165,
  ANN_R_CID_RSVD = 166,
  ANN_R_CID_MDT = 167,
  ANN_R_CID_CRC = 168,
  ANN_R_CID_ONE = 169,
  ANN_R_CSD_CSD_STRUCTURE = 170,
  ANN_R_CSD_RSVD = 171,
  ANN_R_CSD_TAAC = 172,
  ANN_R_CSD_NSAC = 173,
  ANN_R_CSD_TRAN_SPEED = 174,
  ANN_R_CSD_CCC = 175,
  ANN_R_CSD_READ_BL_LEN = 176,
  ANN_R_CSD_READ_BL_PARTIAL = 177,
  ANN_R_CSD_WRITE_BLK_MISALIGN = 178,
  ANN_R_CSD_READ_BLK_MISALIGN = 179,
  ANN_R_CSD_DSR_IMP = 180,
  ANN_R_CSD_C_SIZE = 181,
  ANN_R_CSD_VDD_R_CURR_MIN = 182,
  ANN_R_CSD_VDD_R_CURR_MAX = 183,
  ANN_R_CSD_VDD_W_CURR_MIN = 184,
  ANN_R_CSD_VDD_W_CURR_MAX = 185,
  ANN_R_CSD_C_SIZE_MULT = 186,
  ANN_R_CSD_ERASE_BLK_EN = 187,
  ANN_R_CSD_SECTOR_SIZE = 188,
  ANN_R_CSD_WP_GRP_SIZE = 189,
  ANN_R_CSD_WP_GRP_ENABLE = 190,
  ANN_R_CSD_R2W_FACTOR = 191,
  ANN_R_CSD_WRITE_BL_LEN = 192,
  ANN_R_CSD_WRITE_BL_PARTIAL = 193,
  ANN_R_CSD_FILE_FORMAT_GRP = 194,
  ANN_R_CSD_COPY = 195,
  ANN_R_CSD_PERM_WRITE_PROTECT = 196,
  ANN_R_CSD_TMP_WRITE_PROTECT = 197,
  ANN_R_CSD_FILE_FORMAT = 198,
  ANN_R_CSD_CRC = 199,
  ANN_R_CSD_ONE = 200,
  ANN_BIT_0 = 201,
  ANN_BIT_1 = 202,
  ANN_F_START = 203,
  ANN_F_TRANSMISSION = 204,
  ANN_F_CMD = 205,
  ANN_F_ARG = 206,
  ANN_F_CRC = 207,
  ANN_F_END = 208,
  ANN_DECODED_BIT = 209,
  ANN_DECODED_F = 210,
  NUM_ANN = 211,
};

struct sd_bit {
  uint64_t ss;
  uint64_t es;
  int bit;
};

struct sdcard_sd_priv {
  int state;
  struct sd_bit token[SDCARD_SD_MAX_TOKEN_BITS];
  int token_len;
  int is_acmd;
  int cmd;
  int last_cmd;
  uint32_t arg;
  uint8_t crc;
  char cmd_str[64];
  int out_ann;
};

static struct srd_channel sdcard_sd_channels[] = {
    {"cmd", "CMD", "Command", 0, SRD_CHANNEL_SDATA, "dec_sdcard_sd_chan_cmd"},
    {"clk", "CLK", "Clock", 1, SRD_CHANNEL_SCLK, "dec_sdcard_sd_chan_clk"},
};

static struct srd_channel sdcard_sd_optional_channels[] = {
    {"dat0", "DAT0", "Data pin 0", 2, SRD_CHANNEL_SDATA,
     "dec_sdcard_sd_opt_chan_dat0"},
    {"dat1", "DAT1", "Data pin 1", 3, SRD_CHANNEL_SDATA,
     "dec_sdcard_sd_opt_chan_dat1"},
    {"dat2", "DAT2", "Data pin 2", 4, SRD_CHANNEL_SDATA,
     "dec_sdcard_sd_opt_chan_dat2"},
    {"dat3", "DAT3", "Data pin 3", 5, SRD_CHANNEL_SDATA,
     "dec_sdcard_sd_opt_chan_dat3"},
};

static const char *cmd_names[] = {
    "GO_IDLE_STATE",
    "SEND_OP_COND",
    "ALL_SEND_CID",
    "SEND_RELATIVE_ADDR",
    "SET_DSR",
    "IO_SEND_OP_COND",
    "SWITCH_FUNC",
    "SELECT/DESELECT_CARD",
    "SEND_IF_COND",
    "SEND_CSD",
    "SEND_CID",
    "VOLTAGE_SWITCH",
    "STOP_TRANSMISSION",
    "SEND_STATUS",
    "Reserved",
    "GO_INACTIVE_STATE",
    "SET_BLOCKLEN",
    "READ_SINGLE_BLOCK",
    "READ_MULTIPLE_BLOCK",
    "SEND_TUNING_BLOCK",
    "SPEED_CLASS_CONTROL",
    "Reserved",
    "Reserved",
    "SET_BLOCK_COUNT",
    "WRITE_BLOCK",
    "WRITE_MULTIPLE_BLOCK",
    "Reserved for manufacturer",
    "PROGRAM_CSD",
    "SET_WRITE_PROT",
    "CLR_WRITE_PROT",
    "SEND_WRITE_PROT",
    "Reserved",
    "ERASE_WR_BLK_START",
    "ERASE_WR_BLK_END",
    "Reserved for CMD6",
    "Reserved for CMD6",
    "Reserved for CMD6",
    "Reserved for CMD6",
    "ERASE",
    "Reserved",
    "Reserved for security",
    "Reserved",
    "LOCK_UNLOCK",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved for CMD6",
    "Reserved",
    "Reserved",
    "IO_RW_DIRECT",
    "IO_RW_EXTENDED",
    "Unknown",
    "APP_CMD",
    "GEN_CMD",
    "Reserved for CMD6",
    "READ_OCR",
    "CRC_ON_OFF",
    "Reserved for manufacturer",
    "Reserved for manufacturer",
    "Reserved for manufacturer",
    "Reserved for manufacturer",
};

static const char *acmd_names[] = {
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "SET_BUS_WIDTH",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "SD_STATUS",
    "Reserved for Security",
    "Reserved for Security",
    "Reserved for Security",
    "Reserved",
    "Reserved for SD security",
    "Reserved",
    "Reserved",
    "Reserved",
    "SEND_NUM_WR_BLOCKS",
    "SET_WR_BLK_ERASE_COUNT",
    "Reserved",
    "Reserved for SD security",
    "Reserved for SD security",
    "Reserved for security",
    "Reserved for security",
    "Reserved",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Reserved",
    "Reserved",
    "Reserved for SD security",
    "Reserved",
    "Reserved",
    "SD_SEND_OP_COND",
    "SET_CLR_CARD_DETECT",
    "Reserved for SD security",
    "Reserved for SD security",
    "Reserved for SD security",
    "Reserved for SD security",
    "Reserved for SD security",
    "Reserved for SD security",
    "Reserved for SD security",
    "Unknown",
    "SEND_SCR",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Non-existant",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Reserved for security",
    "Unknown",
    "Unknown",
    "Unknown",
    "Unknown",
};

static const char *accepted_voltages[] = {
    "not defined", "2.7-3.6V",    "reserved for low voltage range",
    "not defined", "reserved",    "not defined",
    "not defined", "not defined", "reserved",
    "not defined", "not defined", "not defined",
    "not defined", "not defined", "not defined",
    "not defined",
};


static const char *ann_label_ptrs[NUM_ANN][3];
static int ann_labels_initialized = 0;

static void init_ann_labels(void) {
  if (ann_labels_initialized)
    return;
  ann_labels_initialized = 1;

  for (int i = 0; i < 64; i++) {

    char buf[16];
    snprintf(buf, sizeof(buf), "cmd%d", i);
    ann_label_ptrs[i][0] = "";
    ann_label_ptrs[i][1] = g_strdup_printf("CMD%d", i);
    ann_label_ptrs[i][2] = g_strdup_printf("CMD%d", i);
  }
  for (int i = 64; i < 128; i++) {
    ann_label_ptrs[i][0] = "";
    ann_label_ptrs[i][1] = g_strdup_printf("ACMD%d", i - 64);
    ann_label_ptrs[i][2] = g_strdup_printf("ACMD%d", i - 64);
  }
  ann_label_ptrs[128][0] = "";
  ann_label_ptrs[128][1] = "R1";
  ann_label_ptrs[128][2] = "R1";
  ann_label_ptrs[129][0] = "";
  ann_label_ptrs[129][1] = "R1b";
  ann_label_ptrs[129][2] = "R1b";
  ann_label_ptrs[130][0] = "";
  ann_label_ptrs[130][1] = "R2";
  ann_label_ptrs[130][2] = "R2";
  ann_label_ptrs[131][0] = "";
  ann_label_ptrs[131][1] = "R3";
  ann_label_ptrs[131][2] = "R3";
  ann_label_ptrs[132][0] = "";
  ann_label_ptrs[132][1] = "R6";
  ann_label_ptrs[132][2] = "R6";
  ann_label_ptrs[133][0] = "";
  ann_label_ptrs[133][1] = "R7";
  ann_label_ptrs[133][2] = "R7";

  static const char *status_names[] = {"OUT_OF_RANGE",
                                       "ADDRESS_ERROR",
                                       "BLOCK_LEN_ERROR",
                                       "ERASE_SEQ_ERROR",
                                       "ERASE_PARAM",
                                       "WP_VIOLATION",
                                       "CARD_IS_LOCKED",
                                       "LOCK_UNLOCK_FAILED",
                                       "COM_CRC_ERROR",
                                       "ILLEGAL_COMMAND",
                                       "CARD_ECC_FAILED",
                                       "CC_ERROR",
                                       "ERROR",
                                       "RSVD",
                                       "RSVD_DEFERRED_RESPONSE",
                                       "CSD_OVERWRITE",
                                       "WP_ERASE_SKIP",
                                       "CARD_ECC_DISABLED",
                                       "ERASE_RESET",
                                       "CURRENT_STATE",
                                       "READY_FOR_DATA",
                                       "FX_EVENT",
                                       "APP_CMD",
                                       "RSVD_SDIO",
                                       "AKE_SEQ_ERROR",
                                       "RSVD_APP_CMD",
                                       "RSVD_TESTMODE"};
  for (int i = 0; i < 27; i++) {
    ann_label_ptrs[134 + i][0] = "";
    ann_label_ptrs[134 + i][1] = g_strdup_printf("Status: %s", status_names[i]);
    ann_label_ptrs[134 + i][2] = g_strdup_printf("Status: %s", status_names[i]);
  }

  static const char *cid_names[] = {"MID",  "OID", "PNM", "PRV", "PSN",
                                    "RSVD", "MDT", "CRC", "ONE"};
  for (int i = 0; i < 9; i++) {
    ann_label_ptrs[161 + i][0] = "";
    ann_label_ptrs[161 + i][1] = g_strdup_printf("CID: %s", cid_names[i]);
    ann_label_ptrs[161 + i][2] = g_strdup_printf("CID: %s", cid_names[i]);
  }

  static const char *csd_names[] = {"CSD_STRUCTURE",
                                    "RSVD",
                                    "TAAC",
                                    "NSAC",
                                    "TRAN_SPEED",
                                    "CCC",
                                    "READ_BL_LEN",
                                    "READ_BL_PARTIAL",
                                    "WRITE_BLK_MISALIGN",
                                    "READ_BLK_MISALIGN",
                                    "DSR_IMP",
                                    "C_SIZE",
                                    "VDD_R_CURR_MIN",
                                    "VDD_R_CURR_MAX",
                                    "VDD_W_CURR_MIN",
                                    "VDD_W_CURR_MAX",
                                    "C_SIZE_MULT",
                                    "ERASE_BLK_EN",
                                    "SECTOR_SIZE",
                                    "WP_GRP_SIZE",
                                    "WP_GRP_ENABLE",
                                    "R2W_FACTOR",
                                    "WRITE_BL_LEN",
                                    "WRITE_BL_PARTIAL",
                                    "FILE_FORMAT_GRP",
                                    "COPY",
                                    "PERM_WRITE_PROTECT",
                                    "TMP_WRITE_PROTECT",
                                    "FILE_FORMAT",
                                    "CRC",
                                    "ONE"};
  for (int i = 0; i < 30; i++) {
    ann_label_ptrs[170 + i][0] = "";
    ann_label_ptrs[170 + i][1] = g_strdup_printf("CSD: %s", csd_names[i]);
    ann_label_ptrs[170 + i][2] = g_strdup_printf("CSD: %s", csd_names[i]);
  }

  ann_label_ptrs[201][0] = "";
  ann_label_ptrs[201][1] = "Bit 0";
  ann_label_ptrs[201][2] = "Bit 0";
  ann_label_ptrs[202][0] = "";
  ann_label_ptrs[202][1] = "Bit 1";
  ann_label_ptrs[202][2] = "Bit 1";
  ann_label_ptrs[203][0] = "";
  ann_label_ptrs[203][1] = "Start bit";
  ann_label_ptrs[203][2] = "Start bit";
  ann_label_ptrs[204][0] = "";
  ann_label_ptrs[204][1] = "Transmission bit";
  ann_label_ptrs[204][2] = "Transmission bit";
  ann_label_ptrs[205][0] = "";
  ann_label_ptrs[205][1] = "Command";
  ann_label_ptrs[205][2] = "Command";
  ann_label_ptrs[206][0] = "";
  ann_label_ptrs[206][1] = "Argument";
  ann_label_ptrs[206][2] = "Argument";
  ann_label_ptrs[207][0] = "";
  ann_label_ptrs[207][1] = "CRC";
  ann_label_ptrs[207][2] = "CRC";
  ann_label_ptrs[208][0] = "";
  ann_label_ptrs[208][1] = "End bit";
  ann_label_ptrs[208][2] = "End bit";
  ann_label_ptrs[209][0] = "";
  ann_label_ptrs[209][1] = "Decoded bit";
  ann_label_ptrs[209][2] = "Decoded bit";
  ann_label_ptrs[210][0] = "";
  ann_label_ptrs[210][1] = "Decoded field";
  ann_label_ptrs[210][2] = "Decoded field";
}

static const char *cmd_name(int cmd, int is_acmd) {
  if (is_acmd) {
    if (cmd >= 0 && cmd < 64)
      return acmd_names[cmd];
  } else {
    if (cmd >= 0 && cmd < 64)
      return cmd_names[cmd];
  }
  return "Unknown";
}

static void sdcard_sd_putt(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s, int cls,
                           const char *text) {
  if (s->token_len < 48)
    return;
  c_put(di, s->token[0].ss, s->token[47].es, s->out_ann, cls, text);
}

static void sdcard_sd_putf(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s, int start, int end,
                           int cls, const char *text) {
  if (start >= s->token_len || end >= s->token_len)
    return;
  c_put(di, s->token[start].ss, s->token[end].es, s->out_ann, cls, text);
}

static void sdcard_sd_puta(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s, int start, int end,
                           int cls, const char *text) {
  if (s->token_len < 48)
    return;
  int s_idx = 47 - 8 - end;
  int e_idx = 47 - 8 - start;
  if (s_idx < 0 || e_idx < 0 || s_idx >= s->token_len || e_idx >= s->token_len)
    return;
  c_put(di, s->token[s_idx].ss, s->token[e_idx].es, s->out_ann, cls, text);
}

static void sdcard_sd_putc(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s, const char *desc) {
  int cmd = s->is_acmd ? ANN_ACMD0 + s->cmd : s->cmd;
  s->last_cmd = cmd;
  char text[256];
  snprintf(text, sizeof(text), "%s: %s", s->cmd_str, desc);
  sdcard_sd_putt(di, s, cmd, text);
}

static void sdcard_sd_putr(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s, int r) {
  const char *name;
  switch (r) {
  case ANN_RESPONSE_R1:
    name = "R1";
    break;
  case ANN_RESPONSE_R1B:
    name = "R1b";
    break;
  case ANN_RESPONSE_R2:
    name = "R2";
    break;
  case ANN_RESPONSE_R3:
    name = "R3";
    break;
  case ANN_RESPONSE_R6:
    name = "R6";
    break;
  case ANN_RESPONSE_R7:
    name = "R7";
    break;
  default:
    name = "R?";
    break;
  }
  char text[64];
  snprintf(text, sizeof(text), "Response: %s", name);
  sdcard_sd_putt(di, s, r, text);
}

static int get_token_bits(struct sdcard_sd_priv *s, int cmd_pin,
                          uint64_t samplenum, int n) {
  if (s->token_len < SDCARD_SD_MAX_TOKEN_BITS) {
    s->token[s->token_len].ss = samplenum;
    s->token[s->token_len].es = samplenum;
    s->token[s->token_len].bit = cmd_pin;
    if (s->token_len > 0)
      s->token[s->token_len - 1].es = samplenum;
    s->token_len++;
  }
  if (s->token_len < n)
    return 0;
  s->token[n - 1].es += s->token[n - 1].ss - s->token[n - 2].ss;
  return 1;
}

static int is_from_host(struct sdcard_sd_priv *s) {
  return (s->token_len > 1 && s->token[1].bit == 1);
}

static int is_from_card(struct sdcard_sd_priv *s) {
  return (s->token_len > 1 && s->token[1].bit == 0);
}

static void handle_common_token_fields(struct srd_decoder_inst *di,
                                       struct sdcard_sd_priv *s) {
  for (int bit = 0; bit < s->token_len; bit++) {
    char text[4];
    snprintf(text, sizeof(text), "%d", s->token[bit].bit);
    sdcard_sd_putf(di, s, bit, bit, ANN_BIT_0 + s->token[bit].bit, text);
  }

  sdcard_sd_putf(di, s, 0, 0, ANN_F_START, "Start bit");

  const char *t = is_from_host(s) ? "host" : "card";
  char text[64];
  snprintf(text, sizeof(text), "Transmission: %s", t);
  sdcard_sd_putf(di, s, 1, 1, ANN_F_TRANSMISSION, text);

  s->cmd = 0;
  for (int i = 2; i < 8 && i < s->token_len; i++)
    s->cmd = (s->cmd << 1) | s->token[i].bit;

  const char *cname = cmd_name(s->cmd, s->is_acmd);
  snprintf(text, sizeof(text), "Command: %s (%d)", cname, s->cmd);
  sdcard_sd_putf(di, s, 2, 7, ANN_F_CMD, text);

  s->arg = 0;
  for (int i = 8; i < 40 && i < s->token_len; i++)
    s->arg = (s->arg << 1) | s->token[i].bit;
  snprintf(text, sizeof(text), "Argument: 0x%08x", s->arg);
  sdcard_sd_putf(di, s, 8, 39, ANN_F_ARG, text);

  s->crc = 0;
  for (int i = 40; i < 47 && i < s->token_len; i++)
    s->crc = (s->crc << 1) | s->token[i].bit;
  snprintf(text, sizeof(text), "CRC: 0x%x", s->crc);
  sdcard_sd_putf(di, s, 40, 46, ANN_F_CRC, text);

  sdcard_sd_putf(di, s, 47, 47, ANN_F_END, "End bit");
}

static void handle_reg_status(struct srd_decoder_inst *di,
                              struct sdcard_sd_priv *s) {
  static const int status_ann[] = {ANN_R_STATUS_OUT_OF_RANGE,
                                   ANN_R_STATUS_ADDRESS_ERROR,
                                   ANN_R_STATUS_BLOCK_LEN_ERROR,
                                   ANN_R_STATUS_ERASE_SEQ_ERROR,
                                   ANN_R_STATUS_ERASE_PARAM,
                                   ANN_R_STATUS_WP_VIOLATION,
                                   ANN_R_STATUS_CARD_IS_LOCKED,
                                   ANN_R_STATUS_LOCK_UNLOCK_FAILED,
                                   ANN_R_STATUS_COM_CRC_ERROR,
                                   ANN_R_STATUS_ILLEGAL_COMMAND,
                                   ANN_R_STATUS_CARD_ECC_FAILED,
                                   ANN_R_STATUS_CC_ERROR,
                                   ANN_R_STATUS_ERROR,
                                   ANN_R_STATUS_RSVD,
                                   ANN_R_STATUS_RSVD_DEFERRED_RESPONSE,
                                   ANN_R_STATUS_CSD_OVERWRITE,
                                   ANN_R_STATUS_WP_ERASE_SKIP,
                                   ANN_R_STATUS_CARD_ECC_DISABLED,
                                   ANN_R_STATUS_ERASE_RESET,
                                   ANN_R_STATUS_CURRENT_STATE,
                                   ANN_R_STATUS_READY_FOR_DATA,
                                   ANN_R_STATUS_FX_EVENT,
                                   ANN_R_STATUS_APP_CMD,
                                   ANN_R_STATUS_RSVD_SDIO,
                                   ANN_R_STATUS_AKE_SEQ_ERROR,
                                   ANN_R_STATUS_RSVD_APP_CMD,
                                   ANN_R_STATUS_RSVD_TESTMODE};
  static const char *status_names[] = {"OUT_OF_RANGE",
                                       "ADDRESS_ERROR",
                                       "BLOCK_LEN_ERROR",
                                       "ERASE_SEQ_ERROR",
                                       "ERASE_PARAM",
                                       "WP_VIOLATION",
                                       "CARD_IS_LOCKED",
                                       "LOCK_UNLOCK_FAILED",
                                       "COM_CRC_ERROR",
                                       "ILLEGAL_COMMAND",
                                       "CARD_ECC_FAILED",
                                       "CC_ERROR",
                                       "ERROR",
                                       "Reserved",
                                       "RSVD_DEFERRED_RESPONSE",
                                       "CSD_OVERWRITE",
                                       "WP_ERASE_SKIP",
                                       "CARD_ECC_DISABLED",
                                       "ERASE_RESET",
                                       "CURRENT_STATE",
                                       "READY_FOR_DATA",
                                       "FX_EVENT",
                                       "APP_CMD",
                                       "RSVD_SDIO",
                                       "AKE_SEQ_ERROR",
                                       "RSVD_APP_CMD",
                                       "RSVD_TESTMODE"};
  for (int i = 0; i < 27; i++) {
    if (i == 19)
      sdcard_sd_putf(di, s, 8 + i, 8 + i + 3, status_ann[i], status_names[i]);
    else if (i >= 20)
      sdcard_sd_putf(di, s, 8 + i + 3, 8 + i + 3, status_ann[i],
                     status_names[i]);
    else
      sdcard_sd_putf(di, s, 8 + i, 8 + i, status_ann[i], status_names[i]);
  }
}

static void handle_reg_cid(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s) {
  sdcard_sd_putf(di, s, 8, 15, ANN_R_CID_MID, "Manufacturer ID");
  sdcard_sd_putf(di, s, 16, 31, ANN_R_CID_OID, "OEM/application ID");
  sdcard_sd_putf(di, s, 32, 71, ANN_R_CID_PNM, "Product name");
  sdcard_sd_putf(di, s, 72, 79, ANN_R_CID_PRV, "Product revision");
  sdcard_sd_putf(di, s, 80, 111, ANN_R_CID_PSN, "Product serial number");
  sdcard_sd_putf(di, s, 112, 115, ANN_R_CID_RSVD, "Reserved");
  sdcard_sd_putf(di, s, 116, 127, ANN_R_CID_MDT, "Manufacturing date");
  sdcard_sd_putf(di, s, 128, 134, ANN_R_CID_CRC, "CRC7 checksum");
  sdcard_sd_putf(di, s, 135, 135, ANN_R_CID_ONE, "Always 1");
}

static void handle_reg_csd(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s) {
  sdcard_sd_putf(di, s, 8, 9, ANN_R_CSD_CSD_STRUCTURE, "CSD structure");
  sdcard_sd_putf(di, s, 10, 15, ANN_R_CSD_RSVD, "Reserved");
  sdcard_sd_putf(di, s, 16, 23, ANN_R_CSD_TAAC, "Data read access-time - 1");
  sdcard_sd_putf(di, s, 24, 31, ANN_R_CSD_NSAC, "Data read access-time - 2");
  sdcard_sd_putf(di, s, 32, 39, ANN_R_CSD_TRAN_SPEED,
                 "Max. data transfer rate");
  sdcard_sd_putf(di, s, 40, 51, ANN_R_CSD_CCC, "Card command classes");
  sdcard_sd_putf(di, s, 52, 55, ANN_R_CSD_READ_BL_LEN,
                 "Max. read data block length");
  sdcard_sd_putf(di, s, 56, 56, ANN_R_CSD_READ_BL_PARTIAL,
                 "Partial blocks for read allowed");
  sdcard_sd_putf(di, s, 57, 57, ANN_R_CSD_WRITE_BLK_MISALIGN,
                 "Write block misalignment");
  sdcard_sd_putf(di, s, 58, 58, ANN_R_CSD_READ_BLK_MISALIGN,
                 "Read block misalignment");
  sdcard_sd_putf(di, s, 59, 59, ANN_R_CSD_DSR_IMP, "DSR implemented");
  sdcard_sd_putf(di, s, 60, 61, ANN_R_CSD_RSVD, "Reserved");
  sdcard_sd_putf(di, s, 62, 73, ANN_R_CSD_C_SIZE, "Device size");
  sdcard_sd_putf(di, s, 74, 76, ANN_R_CSD_VDD_R_CURR_MIN,
                 "Max. read current @VDD min");
  sdcard_sd_putf(di, s, 77, 79, ANN_R_CSD_VDD_R_CURR_MAX,
                 "Max. read current @VDD max");
  sdcard_sd_putf(di, s, 80, 82, ANN_R_CSD_VDD_W_CURR_MIN,
                 "Max. write current @VDD min");
  sdcard_sd_putf(di, s, 83, 85, ANN_R_CSD_VDD_W_CURR_MAX,
                 "Max. write current @VDD max");
  sdcard_sd_putf(di, s, 86, 88, ANN_R_CSD_C_SIZE_MULT,
                 "Device size multiplier");
  sdcard_sd_putf(di, s, 89, 89, ANN_R_CSD_ERASE_BLK_EN,
                 "Erase single block enable");
  sdcard_sd_putf(di, s, 90, 96, ANN_R_CSD_SECTOR_SIZE, "Erase sector size");
  sdcard_sd_putf(di, s, 97, 103, ANN_R_CSD_WP_GRP_SIZE,
                 "Write protect group size");
  sdcard_sd_putf(di, s, 104, 104, ANN_R_CSD_WP_GRP_ENABLE,
                 "Write protect group enable");
  sdcard_sd_putf(di, s, 105, 106, ANN_R_CSD_RSVD, "Reserved");
  sdcard_sd_putf(di, s, 107, 109, ANN_R_CSD_R2W_FACTOR, "Write speed factor");
  sdcard_sd_putf(di, s, 110, 113, ANN_R_CSD_WRITE_BL_LEN,
                 "Max. write data block length");
  sdcard_sd_putf(di, s, 114, 114, ANN_R_CSD_WRITE_BL_PARTIAL,
                 "Partial blocks for write allowed");
  sdcard_sd_putf(di, s, 115, 119, ANN_R_CSD_RSVD, "Reserved");
  sdcard_sd_putf(di, s, 120, 120, ANN_R_CSD_FILE_FORMAT_GRP,
                 "File format group");
  sdcard_sd_putf(di, s, 121, 121, ANN_R_CSD_COPY, "Copy flag");
  sdcard_sd_putf(di, s, 122, 122, ANN_R_CSD_PERM_WRITE_PROTECT,
                 "Permanent write protection");
  sdcard_sd_putf(di, s, 123, 123, ANN_R_CSD_TMP_WRITE_PROTECT,
                 "Temporary write protection");
  sdcard_sd_putf(di, s, 124, 125, ANN_R_CSD_FILE_FORMAT, "File format");
  sdcard_sd_putf(di, s, 126, 127, ANN_R_CSD_RSVD, "Reserved");
  sdcard_sd_putf(di, s, 128, 134, ANN_R_CSD_CRC, "CRC");
  sdcard_sd_putf(di, s, 135, 135, ANN_R_CSD_ONE, "Always 1");
}

static void handle_response_r1(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin);
static void handle_response_r2(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin);
static void handle_response_r3(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin);
static void handle_response_r6(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin);
static void handle_response_r7(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin);

static void handle_cmd(struct srd_decoder_inst *di, struct sdcard_sd_priv *s);

static void get_command_token(struct srd_decoder_inst *di,
                              struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 48))
    return;
  if (!is_from_host(s)) {
    s->token_len = 0;
    s->state = STATE_GET_COMMAND_TOKEN;
    return;
  }
  handle_cmd(di, s);
}

static void handle_cmd(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  handle_common_token_fields(di, s);

  
  const char *prefix = s->is_acmd ? "ACMD" : "CMD";
  snprintf(s->cmd_str, sizeof(s->cmd_str), "%s%d (%s)", prefix, s->cmd,
           cmd_name(s->cmd, s->is_acmd));

  switch (s->cmd) {
  case 0:
    s->state = STATE_HANDLE_CMD0;
    break;
  case 2:
  case 3:
  case 6:
  case 7:
  case 8:
  case 9:
  case 10:
  case 13:
  case 16:
  case 55:
    s->state = STATE_HANDLE_CMD0 + s->cmd;
    break;
  default:
    if (s->is_acmd &&
        (s->cmd == 6 || s->cmd == 13 || s->cmd == 41 || s->cmd == 51)) {
      s->state = STATE_HANDLE_ACMD0 + s->cmd;
    } else if (s->is_acmd) {
      s->state = STATE_HANDLE_ACMD999;
      sdcard_sd_putc(di, s, s->cmd_str);
    } else {
      s->state = STATE_HANDLE_CMD999;
      sdcard_sd_putc(di, s, s->cmd_str);
    }
    break;
  }
}

static void handle_cmd0(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Reset all SD cards");
  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void handle_cmd2(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Ask card for CID number");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R2;
}

static void handle_cmd3(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Ask card for new relative card address (RCA)");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R6;
}

static void handle_cmd6(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_putc(di, s, "Switch/check card function");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_cmd7(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_putc(di, s, "Select / deselect card");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R6;
}

static void handle_cmd8(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 12, 31, ANN_DECODED_F, "Reserved");
  sdcard_sd_puta(di, s, 8, 11, ANN_DECODED_F, "Supply voltage");
  sdcard_sd_puta(di, s, 0, 7, ANN_DECODED_F, "Check pattern");
  sdcard_sd_putc(di, s, "Send interface condition to card");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R7;
}

static void handle_cmd9(struct srd_decoder_inst *di, struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 16, 31, ANN_DECODED_F, "RCA");
  sdcard_sd_puta(di, s, 0, 15, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Send card-specific data (CSD)");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R2;
}

static void handle_cmd10(struct srd_decoder_inst *di,
                         struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 16, 31, ANN_DECODED_F, "RCA");
  sdcard_sd_puta(di, s, 0, 15, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Send card identification data (CID)");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R2;
}

static void handle_cmd13(struct srd_decoder_inst *di,
                         struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 16, 31, ANN_DECODED_F, "RCA");
  sdcard_sd_puta(di, s, 0, 15, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Send card status register");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_cmd16(struct srd_decoder_inst *di,
                         struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Block length");
  char text[64];
  snprintf(text, sizeof(text), "Set the block length to %u bytes", s->arg);
  sdcard_sd_putc(di, s, text);
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_cmd55(struct srd_decoder_inst *di,
                         struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 16, 31, ANN_DECODED_F, "RCA");
  sdcard_sd_puta(di, s, 0, 15, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Next command is an application-specific command");
  s->is_acmd = 1;
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_acmd6(struct srd_decoder_inst *di,
                         struct sdcard_sd_priv *s) {
  sdcard_sd_putc(di, s, "Set bus width");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_acmd13(struct srd_decoder_inst *di,
                          struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Stuff bits");
  sdcard_sd_putc(di, s, "Set SD status");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_acmd41(struct srd_decoder_inst *di,
                          struct sdcard_sd_priv *s) {
  sdcard_sd_puta(di, s, 0, 23, ANN_DECODED_F, "VDD voltage window");
  sdcard_sd_puta(di, s, 24, 24, ANN_DECODED_F, "S18R");
  sdcard_sd_puta(di, s, 25, 27, ANN_DECODED_F, "Reserved");
  sdcard_sd_puta(di, s, 28, 28, ANN_DECODED_F, "XPC");
  sdcard_sd_puta(di, s, 29, 29, ANN_DECODED_F, "Reserved for eSD");
  sdcard_sd_puta(di, s, 30, 30, ANN_DECODED_F, "Host capacity support info");
  sdcard_sd_puta(di, s, 31, 31, ANN_DECODED_F, "Reserved");
  sdcard_sd_putc(di, s, "Send HCS info and activate the card init process");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R3;
}

static void handle_acmd51(struct srd_decoder_inst *di,
                          struct sdcard_sd_priv *s) {
  sdcard_sd_putc(di, s, "Read SD config register (SCR)");
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_cmd999(struct srd_decoder_inst *di,
                          struct sdcard_sd_priv *s) {
  (void)di;
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_acmd999(struct srd_decoder_inst *di,
                           struct sdcard_sd_priv *s) {
  (void)di;
  s->token_len = 0;
  s->state = STATE_GET_RESPONSE_R1;
}

static void handle_response_r1(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 48))
    return;
  if (!is_from_card(s)) {
    handle_cmd(di, s);
    return;
  }
  handle_common_token_fields(di, s);
  sdcard_sd_putr(di, s, ANN_RESPONSE_R1);
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Card status");
  handle_reg_status(di, s);
  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void handle_response_r1b(struct srd_decoder_inst *di,
                                struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 48))
    return;
  if (!is_from_card(s)) {
    handle_cmd(di, s);
    return;
  }
  handle_common_token_fields(di, s);
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "Card status");
  sdcard_sd_putr(di, s, ANN_RESPONSE_R1B);
  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void handle_response_r2(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 136))
    return;
  for (int bit = 0; bit < s->token_len; bit++) {
    char text[4];
    snprintf(text, sizeof(text), "%d", s->token[bit].bit);
    sdcard_sd_putf(di, s, bit, bit, ANN_BIT_0 + s->token[bit].bit, text);
  }
  sdcard_sd_putf(di, s, 0, 0, ANN_F_START, "Start bit");
  const char *t = (s->token_len > 1 && s->token[1].bit == 1) ? "host" : "card";
  char text[64];
  snprintf(text, sizeof(text), "Transmission: %s", t);
  sdcard_sd_putf(di, s, 1, 1, ANN_F_TRANSMISSION, text);
  sdcard_sd_putf(di, s, 2, 7, ANN_F_CMD, "Reserved");
  sdcard_sd_putf(di, s, 8, 134, ANN_F_ARG, "Argument");
  sdcard_sd_putf(di, s, 135, 135, ANN_F_END, "End bit");
  sdcard_sd_putf(di, s, 8, 134, ANN_DECODED_F, "CID/CSD register");
  sdcard_sd_putf(di, s, 0, 135, ANN_RESPONSE_R2, "Response: R2");

  if (s->last_cmd == ANN_CMD2 || s->last_cmd == ANN_CMD10)
    handle_reg_cid(di, s);
  if (s->last_cmd == ANN_CMD9)
    handle_reg_csd(di, s);

  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void handle_response_r3(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 48))
    return;
  sdcard_sd_putr(di, s, ANN_RESPONSE_R3);
  for (int bit = 0; bit < s->token_len; bit++) {
    char text[4];
    snprintf(text, sizeof(text), "%d", s->token[bit].bit);
    sdcard_sd_putf(di, s, bit, bit, ANN_BIT_0 + s->token[bit].bit, text);
  }
  sdcard_sd_putf(di, s, 0, 0, ANN_F_START, "Start bit");
  const char *t = (s->token_len > 1 && s->token[1].bit == 1) ? "host" : "card";
  char text[64];
  snprintf(text, sizeof(text), "Transmission: %s", t);
  sdcard_sd_putf(di, s, 1, 1, ANN_F_TRANSMISSION, text);
  sdcard_sd_putf(di, s, 2, 7, ANN_F_CMD, "Reserved");
  sdcard_sd_putf(di, s, 8, 39, ANN_F_ARG, "Argument");
  sdcard_sd_putf(di, s, 40, 46, ANN_F_CRC, "Reserved");
  sdcard_sd_putf(di, s, 47, 47, ANN_F_END, "End bit");
  sdcard_sd_puta(di, s, 0, 31, ANN_DECODED_F, "OCR register");
  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void handle_response_r6(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 48))
    return;
  if (!is_from_card(s)) {
    handle_cmd(di, s);
    return;
  }
  handle_common_token_fields(di, s);
  sdcard_sd_puta(di, s, 0, 15, ANN_DECODED_F, "Card status bits");
  sdcard_sd_puta(di, s, 16, 31, ANN_DECODED_F, "Relative card address");
  sdcard_sd_putr(di, s, ANN_RESPONSE_R6);
  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void handle_response_r7(struct srd_decoder_inst *di,
                               struct sdcard_sd_priv *s, int cmd_pin) {
  if (!get_token_bits(s, cmd_pin, di_samplenum(di), 48))
    return;
  if (!is_from_card(s)) {
    handle_cmd(di, s);
    return;
  }
  handle_common_token_fields(di, s);
  sdcard_sd_putr(di, s, ANN_RESPONSE_R7);
  sdcard_sd_puta(di, s, 12, 31, ANN_DECODED_F, "Reserved");

  int v = 0;
  for (int i = 28; i < 32 && i < s->token_len; i++)
    v = (v << 1) | s->token[i].bit;
  const char *av = (v >= 0 && v < 16) ? accepted_voltages[v] : "Unknown";
  char text[64];
  snprintf(text, sizeof(text), "Voltage accepted: %s", av);
  sdcard_sd_puta(di, s, 8, 11, ANN_DECODED_F, text);
  sdcard_sd_puta(di, s, 0, 7, ANN_DECODED_F, "Echo-back of check pattern");
  s->token_len = 0;
  s->state = STATE_GET_COMMAND_TOKEN;
}

static void sdcard_sd_reset(struct srd_decoder_inst *di) {
  if (!c_decoder_get_private(di)) {
    c_decoder_set_private(di, g_malloc0(sizeof(struct sdcard_sd_priv)));
  }
  struct sdcard_sd_priv *s = (struct sdcard_sd_priv *)c_decoder_get_private(di);
  memset(s, 0, sizeof(struct sdcard_sd_priv));
  s->state = STATE_GET_COMMAND_TOKEN;
  s->out_ann = -1;
}

static void sdcard_sd_start(struct srd_decoder_inst *di) {
  struct sdcard_sd_priv *s = (struct sdcard_sd_priv *)c_decoder_get_private(di);
  s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sdcard_sd");
  init_ann_labels();
}

static void sdcard_sd_decode(struct srd_decoder_inst *di) {
  struct sdcard_sd_priv *s = (struct sdcard_sd_priv *)c_decoder_get_private(di);
  while (1) {
    int ret;
    if ((s->state == STATE_GET_COMMAND_TOKEN ||
         (s->state >= STATE_GET_RESPONSE_R1 &&
          s->state <= STATE_GET_RESPONSE_R7)) &&
        s->token_len == 0) {
      /* Match Python: single dict {CLK:'r', CMD:'l'} = AND condition */
      ret = c_wait(di, CW_R(CH_CLK), CW_L(CH_CMD), CW_END);
    } else {
      ret = c_wait(di, CW_R(CH_CLK), CW_END);
    }
    if (ret != SRD_OK)
      return;

    int cmd_pin = c_pin(di, CH_CMD);

    if ((s->state == STATE_GET_COMMAND_TOKEN ||
         (s->state >= STATE_GET_RESPONSE_R1 &&
          s->state <= STATE_GET_RESPONSE_R7)) &&
        s->token_len == 0) {
      if (cmd_pin != 0)
        continue;
    }

    if (s->state == STATE_GET_COMMAND_TOKEN) {
      get_command_token(di, s, cmd_pin);
    } else if (s->state >= STATE_HANDLE_CMD0 &&
               s->state <= STATE_HANDLE_CMD0 + 63) {
      int cmdidx = s->state - STATE_HANDLE_CMD0;
      switch (cmdidx) {
      case 0:
        handle_cmd0(di, s);
        break;
      case 2:
        handle_cmd2(di, s);
        break;
      case 3:
        handle_cmd3(di, s);
        break;
      case 6:
        handle_cmd6(di, s);
        break;
      case 7:
        handle_cmd7(di, s);
        break;
      case 8:
        handle_cmd8(di, s);
        break;
      case 9:
        handle_cmd9(di, s);
        break;
      case 10:
        handle_cmd10(di, s);
        break;
      case 13:
        handle_cmd13(di, s);
        break;
      case 16:
        handle_cmd16(di, s);
        break;
      case 55:
        handle_cmd55(di, s);
        break;
      default:
        handle_cmd999(di, s);
        break;
      }
      if (s->is_acmd && cmdidx != 55 && cmdidx != 63)
        s->is_acmd = 0;
    } else if (s->state >= STATE_HANDLE_ACMD0 &&
               s->state <= STATE_HANDLE_ACMD0 + 63) {
      int acmdidx = s->state - STATE_HANDLE_ACMD0;
      switch (acmdidx) {
      case 6:
        handle_acmd6(di, s);
        break;
      case 13:
        handle_acmd13(di, s);
        break;
      case 41:
        handle_acmd41(di, s);
        break;
      case 51:
        handle_acmd51(di, s);
        break;
      default:
        handle_acmd999(di, s);
        break;
      }
      if (acmdidx != 55 && acmdidx != 63)
        s->is_acmd = 0;
    } else if (s->state == STATE_HANDLE_CMD999) {
      handle_cmd999(di, s);
    } else if (s->state == STATE_HANDLE_ACMD999) {
      handle_acmd999(di, s);
    } else if (s->state == STATE_GET_RESPONSE_R1) {
      handle_response_r1(di, s, cmd_pin);
    } else if (s->state == STATE_GET_RESPONSE_R1B) {
      handle_response_r1b(di, s, cmd_pin);
    } else if (s->state == STATE_GET_RESPONSE_R2) {
      handle_response_r2(di, s, cmd_pin);
    } else if (s->state == STATE_GET_RESPONSE_R3) {
      handle_response_r3(di, s, cmd_pin);
    } else if (s->state == STATE_GET_RESPONSE_R6) {
      handle_response_r6(di, s, cmd_pin);
    } else if (s->state == STATE_GET_RESPONSE_R7) {
      handle_response_r7(di, s, cmd_pin);
    }
  }
}

static void sdcard_sd_destroy(struct srd_decoder_inst *di) {
  void *priv = c_decoder_get_private(di);
  if (priv) {
    g_free(priv);
    c_decoder_set_private(di, NULL);
  }
}

static const int sd_row_raw_bits_classes[] = {ANN_BIT_0, ANN_BIT_1, -1};
static const int sd_row_decoded_bits_classes[] = {
    ANN_DECODED_BIT, ANN_RESPONSE_R1, ANN_RESPONSE_R1B, ANN_RESPONSE_R2,
    ANN_RESPONSE_R3, ANN_RESPONSE_R6, ANN_RESPONSE_R7,  -1};
static const int sd_row_decoded_fields_classes[] = {ANN_DECODED_F, -1};
static const int sd_row_fields_classes[] = {
    ANN_F_START, ANN_F_TRANSMISSION, ANN_F_CMD, ANN_F_ARG,
    ANN_F_CRC,   ANN_F_END,          -1};
static const int sd_row_commands_classes[] = {
    /* CMD0-63, ACMD0-63, RESPONSE_R1-R7 */ 0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    62,
    63,
    64,
    65,
    66,
    67,
    68,
    69,
    70,
    71,
    72,
    73,
    74,
    75,
    76,
    77,
    78,
    79,
    80,
    81,
    82,
    83,
    84,
    85,
    86,
    87,
    88,
    89,
    90,
    91,
    92,
    93,
    94,
    95,
    96,
    97,
    98,
    99,
    100,
    101,
    102,
    103,
    104,
    105,
    106,
    107,
    108,
    109,
    110,
    111,
    112,
    113,
    114,
    115,
    116,
    117,
    118,
    119,
    120,
    121,
    122,
    123,
    124,
    125,
    126,
    127,
    128,
    129,
    130,
    131,
    132,
    133,
    -1};

static const struct srd_c_ann_row sdcard_sd_ann_rows[] = {
    {"raw-bits", "Raw bits", sd_row_raw_bits_classes, 2},
    {"decoded-bits", "Decoded bits", sd_row_decoded_bits_classes, 7},
    {"decoded-fields", "Decoded fields", sd_row_decoded_fields_classes, 1},
    {"fields", "Fields", sd_row_fields_classes, 6},
    {"commands", "Commands", sd_row_commands_classes, 134},
};

static const char *sdcard_sd_inputs[] = {"logic"};
static const char *sdcard_sd_tags[] = {"Memory"};

struct srd_c_decoder sdcard_sd_c_decoder = {
    .id = "sdcard_sd_c",
    .name = "SD card (SD mode)(C)",
    .longname = "Secure Digital card (SD mode) (C)",
    .desc = "Secure Digital card (SD mode) low-level protocol.",
    .license = "gplv2+",
    .channels = sdcard_sd_channels,
    .num_channels = 2,
    .optional_channels = sdcard_sd_optional_channels,
    .num_optional_channels = 4,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = (const char *(*)[3])ann_label_ptrs,
    .num_annotation_rows = 5,
    .annotation_rows = sdcard_sd_ann_rows,
    .inputs = sdcard_sd_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sdcard_sd_tags,
    .num_tags = 1,
    .reset = sdcard_sd_reset,
    .start = sdcard_sd_start,
    .decode = sdcard_sd_decode,
    .destroy = sdcard_sd_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void) {
  return &sdcard_sd_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void) {
  return SRD_C_DECODER_API_VERSION;
}