/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * SD card SPI mode protocol decoder (C implementation).
 * Ported from Python decoder sdcard_spi.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation enumeration ===== */
/* CMD0-CMD63: 0-63, ACMD0-ACMD63: 64-127, R1/R1b/R2/R3/R7: 128-132, BIT/BIT_WARNING: 133-134 */
enum {
    ANN_CMD0 = 0,
    ANN_CMD1, ANN_CMD2, ANN_CMD3, ANN_CMD4, ANN_CMD5, ANN_CMD6, ANN_CMD7,
    ANN_CMD8, ANN_CMD9, ANN_CMD10, ANN_CMD11, ANN_CMD12, ANN_CMD13,
    ANN_CMD14, ANN_CMD15, ANN_CMD16, ANN_CMD17, ANN_CMD18, ANN_CMD19,
    ANN_CMD20, ANN_CMD21, ANN_CMD22, ANN_CMD23, ANN_CMD24, ANN_CMD25,
    ANN_CMD26, ANN_CMD27, ANN_CMD28, ANN_CMD29, ANN_CMD30, ANN_CMD31,
    ANN_CMD32, ANN_CMD33, ANN_CMD34, ANN_CMD35, ANN_CMD36, ANN_CMD37,
    ANN_CMD38, ANN_CMD39, ANN_CMD40, ANN_CMD41, ANN_CMD42, ANN_CMD43,
    ANN_CMD44, ANN_CMD45, ANN_CMD46, ANN_CMD47, ANN_CMD48, ANN_CMD49,
    ANN_CMD50, ANN_CMD51, ANN_CMD52, ANN_CMD53, ANN_CMD54, ANN_CMD55,
    ANN_CMD56, ANN_CMD57, ANN_CMD58, ANN_CMD59, ANN_CMD60, ANN_CMD61,
    ANN_CMD62, ANN_CMD63,
    ANN_ACMD0, ANN_ACMD1, ANN_ACMD2, ANN_ACMD3, ANN_ACMD4, ANN_ACMD5,
    ANN_ACMD6, ANN_ACMD7, ANN_ACMD8, ANN_ACMD9, ANN_ACMD10, ANN_ACMD11,
    ANN_ACMD12, ANN_ACMD13, ANN_ACMD14, ANN_ACMD15, ANN_ACMD16, ANN_ACMD17,
    ANN_ACMD18, ANN_ACMD19, ANN_ACMD20, ANN_ACMD21, ANN_ACMD22, ANN_ACMD23,
    ANN_ACMD24, ANN_ACMD25, ANN_ACMD26, ANN_ACMD27, ANN_ACMD28, ANN_ACMD29,
    ANN_ACMD30, ANN_ACMD31, ANN_ACMD32, ANN_ACMD33, ANN_ACMD34, ANN_ACMD35,
    ANN_ACMD36, ANN_ACMD37, ANN_ACMD38, ANN_ACMD39, ANN_ACMD40, ANN_ACMD41,
    ANN_ACMD42, ANN_ACMD43, ANN_ACMD44, ANN_ACMD45, ANN_ACMD46, ANN_ACMD47,
    ANN_ACMD48, ANN_ACMD49, ANN_ACMD50, ANN_ACMD51, ANN_ACMD52, ANN_ACMD53,
    ANN_ACMD54, ANN_ACMD55, ANN_ACMD56, ANN_ACMD57, ANN_ACMD58, ANN_ACMD59,
    ANN_ACMD60, ANN_ACMD61, ANN_ACMD62, ANN_ACMD63,
    ANN_R1, ANN_R1B, ANN_R2, ANN_R3, ANN_R7,
    ANN_BIT, ANN_BIT_WARNING,
    NUM_ANN = 135,
};

/* ===== State enumeration ===== */
enum sdcard_state {
    SDCARD_IDLE,
    SDCARD_GET_CMD_TOKEN,
    SDCARD_HANDLE_CMD0,
    SDCARD_HANDLE_CMD1,
    SDCARD_HANDLE_CMD9,
    SDCARD_HANDLE_CMD10,
    SDCARD_HANDLE_CMD16,
    SDCARD_HANDLE_CMD17,
    SDCARD_HANDLE_CMD24,
    SDCARD_HANDLE_CMD49,
    SDCARD_HANDLE_CMD55,
    SDCARD_HANDLE_CMD59,
    SDCARD_HANDLE_CMD999,
    SDCARD_GET_RESPONSE_R1,
    SDCARD_GET_RESPONSE_R1B,
    SDCARD_GET_RESPONSE_R2,
    SDCARD_GET_RESPONSE_R3,
    SDCARD_GET_RESPONSE_R7,
    SDCARD_HANDLE_DATA_CMD17,
    SDCARD_HANDLE_DATA_CMD24,
    SDCARD_DATA_RESPONSE,
    SDCARD_WAIT_BUSY,
};

/* ===== State structure ===== */
typedef struct {
    uint64_t ss;
    uint64_t es;
    enum sdcard_state state;
    
    uint64_t ss_cmd, es_cmd;
    uint64_t ss_busy, es_busy;
    uint8_t cmd_token[6];
    int cmd_token_count;
    int is_acmd;
    uint32_t blocklen;
    uint8_t read_buf[520];
    int read_buf_count;
    char cmd_str[64];
    int is_cmd24;
    int cmd24_start_token_found;
    int is_cmd17;
    int cmd17_start_token_found;
    int busy_first_byte;
    uint32_t arg;
    int cmd_index;
    uint64_t ss_data, es_data;
    uint64_t ss_crc, es_crc;
    int out_ann;
} sdcard_state;

/* ===== SD card command names ===== */
static const char *cmd_names[] = {
    "GO_IDLE_STATE", "SEND_OP_COND", "ALL_SEND_CID", "SEND_RELATIVE_ADDR",
    "SET_DSR", "IO_SET_OP_COND", "SWITCH_FUNC", "SELECT_DESELECT_CARD",
    "SEND_IF_COND", "SEND_CSD", "SEND_CID", "VOLTAGE_SWITCH",
    "STOP_TRANSMISSION", "SEND_STATUS", "GO_INACTIVE_STATE", "SET_BLOCKLEN",
    "READ_SINGLE_BLOCK", "READ_MULTIPLE_BLOCK", "WRITE_BLOCK",
    "WRITE_MULTIPLE_BLOCK", "PROGRAM_CSD", "SET_WRITE_PROT",
    "CLR_WRITE_PROT", "SEND_WRITE_PROT", "ERASE_WR_BLK_START",
    "ERASE_WR_BLK_END", "ERASE", "LOCK_UNLOCK", "APP_CMD", "GEN_CMD",
    "RESERVED30", "RESERVED31", "ERASE_WR_BLK_START_ADDR",
    "ERASE_WR_BLK_END_ADDR", "RESERVED34", "RESERVED35", "RESERVED36",
    "RESERVED37", "RESERVED38", "RESERVED39", "RESERVED40", "RESERVED41",
    "RESERVED42", "RESERVED43", "RESERVED44", "RESERVED45", "RESERVED46",
    "RESERVED47", "RESERVED48", "RESERVED49", "RESERVED50", "RESERVED51",
    "RESERVED52", "RESERVED53", "RESERVED54", "RESERVED55", "RESERVED56",
    "RESERVED57", "RESERVED58", "RESERVED59", "RESERVED60", "RESERVED61",
    "RESERVED62", "RESERVED63",
};

static const char *acmd_names[] = {
    "RESERVED0", "RESERVED1", "RESERVED2", "RESERVED3",
    "RESERVED4", "RESERVED5", "SET_BUS_WIDTH", "SD_STATUS",
    "SEND_NUM_WR_BLOCKS", "SEND_WR_BLK_Erase_COUNT", "SD_SEND_OP_COND",
    "RESERVED11", "RESERVED12", "RESERVED13", "RESERVED14", "RESERVED15",
    "RESERVED16", "RESERVED17", "RESERVED18", "RESERVED19", "RESERVED20",
    "RESERVED21", "RESERVED22", "RESERVED23", "SET_WR_BLK_ERASE_COUNT",
    "RESERVED25", "RESERVED26", "RESERVED27", "RESERVED28", "RESERVED29",
    "RESERVED30", "RESERVED31", "RESERVED32", "RESERVED33", "RESERVED34",
    "RESERVED35", "RESERVED36", "RESERVED37", "RESERVED38", "RESERVED39",
    "RESERVED40", "SD_SEND_IF_COND", "RESERVED42", "RESERVED43",
    "RESERVED44", "RESERVED45", "RESERVED46", "RESERVED47", "RESERVED48",
    "RESERVED49", "RESERVED50", "RESERVED51", "RESERVED52", "RESERVED53",
    "RESERVED54", "RESERVED55", "RESERVED56", "RESERVED57", "RESERVED58",
    "RESERVED59", "RESERVED60", "RESERVED61", "RESERVED62", "RESERVED63",
};

/* ===== SPI DATA packet helpers ===== */
static inline int spi_proto_get_mosi(const c_field *fields, int n_fields, uint8_t *mosi_val)
{
    if (n_fields < 17 || !(fields[0].u8 & 1)) {
        *mosi_val = 0;
        return 0;
    }
    *mosi_val = (uint8_t)fields[1].u8;
    return 1;
}

static inline int spi_proto_get_miso(const c_field *fields, int n_fields, uint8_t *miso_val)
{
    if (n_fields < 17 || !((fields[0].u8 >> 1) & 1)) {
        *miso_val = 0;
        return 0;
    }
    *miso_val = (uint8_t)fields[9].u8;
    return 1;
}

static inline int spi_proto_cs_change_get_values(const c_field *fields, int n_fields,
                                                  uint8_t *prev, uint8_t *cur)
{
    if (n_fields < 2) {
        *prev = 0xFF; *cur = 0xFF;
        return -1;
    }
    *prev = fields[0].u8;
    *cur = fields[1].u8;
    return 0;
}

/* ===== Static data ===== */
static const char *sdcard_spi_inputs[] = {"spi", NULL};
static const char *sdcard_spi_tags[] = {"Memory", NULL};

/* Macro-generated annotation labels */
#define CMD_ANN(i) {"", "cmd" #i, "CMD" #i}
#define ACMD_ANN(i) {"", "acmd" #i, "ACMD" #i}

static const char *sdcard_spi_ann_labels[][3] = {
    CMD_ANN(0), CMD_ANN(1), CMD_ANN(2), CMD_ANN(3), CMD_ANN(4),
    CMD_ANN(5), CMD_ANN(6), CMD_ANN(7), CMD_ANN(8), CMD_ANN(9),
    CMD_ANN(10), CMD_ANN(11), CMD_ANN(12), CMD_ANN(13), CMD_ANN(14),
    CMD_ANN(15), CMD_ANN(16), CMD_ANN(17), CMD_ANN(18), CMD_ANN(19),
    CMD_ANN(20), CMD_ANN(21), CMD_ANN(22), CMD_ANN(23), CMD_ANN(24),
    CMD_ANN(25), CMD_ANN(26), CMD_ANN(27), CMD_ANN(28), CMD_ANN(29),
    CMD_ANN(30), CMD_ANN(31), CMD_ANN(32), CMD_ANN(33), CMD_ANN(34),
    CMD_ANN(35), CMD_ANN(36), CMD_ANN(37), CMD_ANN(38), CMD_ANN(39),
    CMD_ANN(40), CMD_ANN(41), CMD_ANN(42), CMD_ANN(43), CMD_ANN(44),
    CMD_ANN(45), CMD_ANN(46), CMD_ANN(47), CMD_ANN(48), CMD_ANN(49),
    CMD_ANN(50), CMD_ANN(51), CMD_ANN(52), CMD_ANN(53), CMD_ANN(54),
    CMD_ANN(55), CMD_ANN(56), CMD_ANN(57), CMD_ANN(58), CMD_ANN(59),
    CMD_ANN(60), CMD_ANN(61), CMD_ANN(62), CMD_ANN(63),
    ACMD_ANN(0), ACMD_ANN(1), ACMD_ANN(2), ACMD_ANN(3), ACMD_ANN(4),
    ACMD_ANN(5), ACMD_ANN(6), ACMD_ANN(7), ACMD_ANN(8), ACMD_ANN(9),
    ACMD_ANN(10), ACMD_ANN(11), ACMD_ANN(12), ACMD_ANN(13), ACMD_ANN(14),
    ACMD_ANN(15), ACMD_ANN(16), ACMD_ANN(17), ACMD_ANN(18), ACMD_ANN(19),
    ACMD_ANN(20), ACMD_ANN(21), ACMD_ANN(22), ACMD_ANN(23), ACMD_ANN(24),
    ACMD_ANN(25), ACMD_ANN(26), ACMD_ANN(27), ACMD_ANN(28), ACMD_ANN(29),
    ACMD_ANN(30), ACMD_ANN(31), ACMD_ANN(32), ACMD_ANN(33), ACMD_ANN(34),
    ACMD_ANN(35), ACMD_ANN(36), ACMD_ANN(37), ACMD_ANN(38), ACMD_ANN(39),
    ACMD_ANN(40), ACMD_ANN(41), ACMD_ANN(42), ACMD_ANN(43), ACMD_ANN(44),
    ACMD_ANN(45), ACMD_ANN(46), ACMD_ANN(47), ACMD_ANN(48), ACMD_ANN(49),
    ACMD_ANN(50), ACMD_ANN(51), ACMD_ANN(52), ACMD_ANN(53), ACMD_ANN(54),
    ACMD_ANN(55), ACMD_ANN(56), ACMD_ANN(57), ACMD_ANN(58), ACMD_ANN(59),
    ACMD_ANN(60), ACMD_ANN(61), ACMD_ANN(62), ACMD_ANN(63),
    {"", "r1", "R1 response"},
    {"", "r1b", "R1b response"},
    {"", "r2", "R2 response"},
    {"", "r3", "R3 response"},
    {"", "r7", "R7 response"},
    {"", "bit", "Bit"},
    {"", "bit-warning", "Bit warning"},
};

/* Annotation rows */
static const int sdcard_row_bits_classes[] = {ANN_BIT, ANN_BIT_WARNING, -1};
static const int sdcard_row_cmds_classes[] = {
    ANN_CMD0, ANN_CMD1, ANN_CMD2, ANN_CMD3, ANN_CMD4, ANN_CMD5, ANN_CMD6, ANN_CMD7,
    ANN_CMD8, ANN_CMD9, ANN_CMD10, ANN_CMD11, ANN_CMD12, ANN_CMD13, ANN_CMD14, ANN_CMD15,
    ANN_CMD16, ANN_CMD17, ANN_CMD18, ANN_CMD19, ANN_CMD20, ANN_CMD21, ANN_CMD22, ANN_CMD23,
    ANN_CMD24, ANN_CMD25, ANN_CMD26, ANN_CMD27, ANN_CMD28, ANN_CMD29, ANN_CMD30, ANN_CMD31,
    ANN_CMD32, ANN_CMD33, ANN_CMD34, ANN_CMD35, ANN_CMD36, ANN_CMD37, ANN_CMD38, ANN_CMD39,
    ANN_CMD40, ANN_CMD41, ANN_CMD42, ANN_CMD43, ANN_CMD44, ANN_CMD45, ANN_CMD46, ANN_CMD47,
    ANN_CMD48, ANN_CMD49, ANN_CMD50, ANN_CMD51, ANN_CMD52, ANN_CMD53, ANN_CMD54, ANN_CMD55,
    ANN_CMD56, ANN_CMD57, ANN_CMD58, ANN_CMD59, ANN_CMD60, ANN_CMD61, ANN_CMD62, ANN_CMD63,
    ANN_ACMD0, ANN_ACMD1, ANN_ACMD2, ANN_ACMD3, ANN_ACMD4, ANN_ACMD5, ANN_ACMD6, ANN_ACMD7,
    ANN_ACMD8, ANN_ACMD9, ANN_ACMD10, ANN_ACMD11, ANN_ACMD12, ANN_ACMD13, ANN_ACMD14, ANN_ACMD15,
    ANN_ACMD16, ANN_ACMD17, ANN_ACMD18, ANN_ACMD19, ANN_ACMD20, ANN_ACMD21, ANN_ACMD22, ANN_ACMD23,
    ANN_ACMD24, ANN_ACMD25, ANN_ACMD26, ANN_ACMD27, ANN_ACMD28, ANN_ACMD29, ANN_ACMD30, ANN_ACMD31,
    ANN_ACMD32, ANN_ACMD33, ANN_ACMD34, ANN_ACMD35, ANN_ACMD36, ANN_ACMD37, ANN_ACMD38, ANN_ACMD39,
    ANN_ACMD40, ANN_ACMD41, ANN_ACMD42, ANN_ACMD43, ANN_ACMD44, ANN_ACMD45, ANN_ACMD46, ANN_ACMD47,
    ANN_ACMD48, ANN_ACMD49, ANN_ACMD50, ANN_ACMD51, ANN_ACMD52, ANN_ACMD53, ANN_ACMD54, ANN_ACMD55,
    ANN_ACMD56, ANN_ACMD57, ANN_ACMD58, ANN_ACMD59, ANN_ACMD60, ANN_ACMD61, ANN_ACMD62, ANN_ACMD63,
    ANN_R1, ANN_R1B, ANN_R2, ANN_R3, ANN_R7, -1
};

static const struct srd_c_ann_row sdcard_spi_ann_rows[] = {
    {"bits", "Bits", sdcard_row_bits_classes, 2},
    {"commands-replies", "Commands/replies", sdcard_row_cmds_classes, 133},
};

/* ===== Helper: get command name ===== */
static const char *sdcard_cmd_name(int cmd, int is_acmd)
{
    if (cmd < 0 || cmd > 63) return "Unknown";
    return is_acmd ? acmd_names[cmd] : cmd_names[cmd];
}

/* ===== Helper: put command annotation ===== */
static void sdcard_putc(struct srd_decoder_inst *di, sdcard_state *s, int cls, const char *desc)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s: %s", s->cmd_str, desc);
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, cls, buf);
}

/* ===== Command handlers ===== */
static void sdcard_handle_cmd0(struct srd_decoder_inst *di, sdcard_state *s)
{
    sdcard_putc(di, s, ANN_CMD0, "Reset the SD card");
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd1(struct srd_decoder_inst *di, sdcard_state *s)
{
    sdcard_putc(di, s, ANN_CMD1, "Send HCS info and activate the card init process");
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd9(struct srd_decoder_inst *di, sdcard_state *s)
{
    sdcard_putc(di, s, ANN_CMD9, "Ask card to send its card specific data (CSD)");
    s->read_buf_count = 0;
    s->state = SDCARD_HANDLE_CMD9;
}

static void sdcard_handle_cmd10(struct srd_decoder_inst *di, sdcard_state *s)
{
    sdcard_putc(di, s, ANN_CMD10, "Ask card to send its card identification (CID)");
    s->read_buf_count = 0;
    s->state = SDCARD_HANDLE_CMD10;
}

static void sdcard_handle_cmd16(struct srd_decoder_inst *di, sdcard_state *s)
{
    s->blocklen = s->arg;
    char buf[256];
    snprintf(buf, sizeof(buf), "Set the block length to %d bytes", s->blocklen);
    sdcard_putc(di, s, ANN_CMD16, buf);
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd17(struct srd_decoder_inst *di, sdcard_state *s)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "Read a block from address 0x%04X", s->arg);
    sdcard_putc(di, s, ANN_CMD17, buf);
    s->is_cmd17 = 1;
    s->cmd17_start_token_found = 0;
    s->read_buf_count = 0;
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd24(struct srd_decoder_inst *di, sdcard_state *s)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "Write a block to address 0x%04X", s->arg);
    sdcard_putc(di, s, ANN_CMD24, buf);
    s->is_cmd24 = 1;
    s->cmd24_start_token_found = 0;
    s->read_buf_count = 0;
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd49(struct srd_decoder_inst *di, sdcard_state *s)
{
    (void)di;
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd55(struct srd_decoder_inst *di, sdcard_state *s)
{
    sdcard_putc(di, s, ANN_CMD55, "Next command is an application-specific command");
    s->is_acmd = 1;
    s->state = SDCARD_GET_RESPONSE_R1;
}

static void sdcard_handle_cmd59(struct srd_decoder_inst *di, sdcard_state *s)
{
    int crc_on_off = s->arg & 1;
    char buf[256];
    snprintf(buf, sizeof(buf), "Turn the SD card CRC option %s", crc_on_off ? "on" : "off");
    sdcard_putc(di, s, ANN_CMD59, buf);
    s->state = SDCARD_GET_RESPONSE_R1;
}



/* ===== Handle command token ===== */
static void sdcard_handle_command_token(struct srd_decoder_inst *di, sdcard_state *s,
                                         uint8_t mosi, uint8_t miso)
{
    (void)miso;

    if (s->cmd_token_count == 0)
        s->ss_cmd = s->ss;

    s->cmd_token[s->cmd_token_count++] = mosi;

    if (s->cmd_token_count < 6)
        return;

    s->es_cmd = s->es;

    /* Parse command token */
    int cmd = s->cmd_index = s->cmd_token[0] & 0x3f;
    s->arg = ((uint32_t)s->cmd_token[1] << 24) | ((uint32_t)s->cmd_token[2] << 16) |
             ((uint32_t)s->cmd_token[3] << 8) | s->cmd_token[4];

    const char *prefix = s->is_acmd ? "ACMD" : "CMD";
    const char *name = sdcard_cmd_name(cmd, s->is_acmd);
    snprintf(s->cmd_str, sizeof(s->cmd_str), "%s%d (%s)", prefix, cmd, name);

    /* Dispatch command handler */
    if (cmd == 0) { sdcard_handle_cmd0(di, s); }
    else if (cmd == 1) { sdcard_handle_cmd1(di, s); }
    else if (cmd == 9) { sdcard_handle_cmd9(di, s); }
    else if (cmd == 10) { sdcard_handle_cmd10(di, s); }
    else if (cmd == 16) { sdcard_handle_cmd16(di, s); }
    else if (cmd == 17) { sdcard_handle_cmd17(di, s); }
    else if (cmd == 24) { sdcard_handle_cmd24(di, s); }
    else if (cmd == 49) { sdcard_handle_cmd49(di, s); }
    else if (cmd == 55) { sdcard_handle_cmd55(di, s); }
    else if (cmd == 59) { sdcard_handle_cmd59(di, s); }
    else {
        /* Unknown or unhandled command */
        int ann = s->is_acmd ? (ANN_ACMD0 + cmd) : (ANN_CMD0 + cmd);
        char hex[32];
        snprintf(hex, sizeof(hex), "%02X %02X %02X %02X %02X %02X",
                 s->cmd_token[0], s->cmd_token[1], s->cmd_token[2],
                 s->cmd_token[3], s->cmd_token[4], s->cmd_token[5]);
        char buf[256];
        snprintf(buf, sizeof(buf), "%s%d: %s", prefix, cmd, hex);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ann, buf);
        s->state = SDCARD_HANDLE_CMD999;
    }

    s->cmd_token_count = 0;
}

/* ===== Handle R1 response ===== */
static void sdcard_handle_response_r1(struct srd_decoder_inst *di, sdcard_state *s, uint8_t res)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "R1: 0x%02x", res);
    c_put(di, s->ss, s->es, s->out_ann, ANN_R1, buf);

    if (s->is_cmd17) {
        s->state = SDCARD_HANDLE_DATA_CMD17;
    } else if (s->is_cmd24) {
        s->state = SDCARD_HANDLE_DATA_CMD24;
    } else {
        s->state = SDCARD_IDLE;
    }
}

/* ===== Handle data block for CMD17 ===== */
static void sdcard_handle_data_cmd17(struct srd_decoder_inst *di, sdcard_state *s, uint8_t miso)
{
    if (s->cmd17_start_token_found) {
        if (s->read_buf_count == 0) {
            s->ss_data = s->ss;
            if (!s->blocklen)
                s->blocklen = 512;
        }
        s->read_buf[s->read_buf_count++] = miso;
        if ((uint32_t)s->read_buf_count < s->blocklen)
            return;
        if ((uint32_t)s->read_buf_count == s->blocklen) {
            s->es_data = s->es;
            c_put(di, s->ss_data, s->es_data, s->out_ann, ANN_CMD17, "Block data");
        } else if ((uint32_t)s->read_buf_count == s->blocklen + 1) {
            s->ss_crc = s->ss;
        } else if ((uint32_t)s->read_buf_count == s->blocklen + 2) {
            s->es_crc = s->es;
            c_put(di, s->ss_crc, s->es_crc, s->out_ann, ANN_CMD17, "CRC");
            s->state = SDCARD_IDLE;
        }
    } else if (miso == 0xfe) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_CMD17, "Start Block");
        s->cmd17_start_token_found = 1;
        s->read_buf_count = 0;
    }
}

/* ===== Handle data block for CMD24 ===== */
static void sdcard_handle_data_cmd24(struct srd_decoder_inst *di, sdcard_state *s, uint8_t mosi)
{
    if (s->cmd24_start_token_found) {
        if (s->read_buf_count == 0) {
            s->ss_data = s->ss;
            if (!s->blocklen)
                s->blocklen = 512;
        }
        s->read_buf[s->read_buf_count++] = mosi;
        if ((uint32_t)s->read_buf_count < s->blocklen)
            return;
        s->es_data = s->es;
        c_put(di, s->ss_data, s->es_data, s->out_ann, ANN_CMD24, "Block data");
        s->read_buf_count = 0;
        s->state = SDCARD_DATA_RESPONSE;
    } else if (mosi == 0xfe) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_CMD24, "Start Block");
        s->cmd24_start_token_found = 1;
        s->read_buf_count = 0;
    }
}

/* ===== Handle data response ===== */
static void sdcard_handle_data_response(struct srd_decoder_inst *di, sdcard_state *s, uint8_t miso)
{
    miso &= 0x1f;
    if ((miso & 0x11) != 0x01)
        return;

    if (miso == 0x05) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, "Data accepted");
    } else if (miso == 0x0b) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, "Data rejected (CRC error)");
    } else if (miso == 0x0d) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, "Data rejected (write error)");
    }

    if (s->is_cmd24) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_CMD24, "Data Response");
        s->state = SDCARD_WAIT_BUSY;
        s->busy_first_byte = 1;
    } else {
        s->state = SDCARD_IDLE;
    }
}

/* ===== Wait while card busy ===== */
static void sdcard_wait_busy(struct srd_decoder_inst *di, sdcard_state *s, uint8_t miso)
{
    if (miso != 0x00) {
        if (s->is_cmd24) {
            c_put(di, s->ss_busy, s->es_busy, s->out_ann, ANN_CMD24, "Card is busy");
        }
        s->state = SDCARD_IDLE;
    } else {
        if (s->busy_first_byte) {
            s->ss_busy = s->ss;
            s->busy_first_byte = 0;
        } else {
            s->es_busy = s->es;
        }
    }
}

/* ===== recv_proto ===== */
static void sdcard_spi_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    sdcard_state *s = (sdcard_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        /* Reset state on CS change */
        s->state = SDCARD_IDLE;
        s->cmd_token_count = 0;
        s->is_cmd17 = 0;
        s->is_cmd24 = 0;
        s->cmd17_start_token_found = 0;
        s->cmd24_start_token_found = 0;
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t mosi = 0, miso = 0;
    spi_proto_get_mosi(fields, n_fields, &mosi);
    spi_proto_get_miso(fields, n_fields, &miso);

    switch (s->state) {
    case SDCARD_IDLE:
        if (mosi == 0xff)
            return;
        s->state = SDCARD_GET_CMD_TOKEN;
        s->cmd_token_count = 0;
        sdcard_handle_command_token(di, s, mosi, miso);
        break;

    case SDCARD_GET_CMD_TOKEN:
        sdcard_handle_command_token(di, s, mosi, miso);
        break;

    case SDCARD_HANDLE_CMD9:
        if (s->read_buf_count == 0)
            s->ss_cmd = s->ss;
        s->read_buf[s->read_buf_count++] = miso;
        if (s->read_buf_count < 20)
            return;
        s->es_cmd = s->es;
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_CMD9, "CSD data");
        s->read_buf_count = 0;
        s->state = SDCARD_IDLE;
        break;

    case SDCARD_HANDLE_CMD10:
        s->read_buf[s->read_buf_count++] = miso;
        if (s->read_buf_count < 16)
            return;
        c_put(di, s->ss_cmd, s->es, s->out_ann, ANN_CMD10, "CID data");
        s->read_buf_count = 0;
        s->state = SDCARD_GET_RESPONSE_R1;
        break;

    case SDCARD_HANDLE_CMD999:
        s->state = SDCARD_GET_RESPONSE_R1;
        break;

    case SDCARD_GET_RESPONSE_R1:
        if (miso == 0xff)
            return;
        s->state = SDCARD_IDLE;
        sdcard_handle_response_r1(di, s, miso);
        /* Leave ACMD mode after first command after CMD55 */
        if (s->is_acmd && s->cmd_index != 55)
            s->is_acmd = 0;
        break;

    case SDCARD_GET_RESPONSE_R1B:
    case SDCARD_GET_RESPONSE_R2:
    case SDCARD_GET_RESPONSE_R3:
    case SDCARD_GET_RESPONSE_R7:
        if (miso == 0xff)
            return;
        s->state = SDCARD_IDLE;
        break;

    case SDCARD_HANDLE_DATA_CMD17:
        sdcard_handle_data_cmd17(di, s, miso);
        break;

    case SDCARD_HANDLE_DATA_CMD24:
        sdcard_handle_data_cmd24(di, s, mosi);
        break;

    case SDCARD_DATA_RESPONSE:
        sdcard_handle_data_response(di, s, miso);
        break;

    case SDCARD_WAIT_BUSY:
        sdcard_wait_busy(di, s, miso);
        break;

    default:
        s->state = SDCARD_IDLE;
        break;
    }
}

/* ===== Lifecycle callbacks ===== */
static void sdcard_spi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(sdcard_state)));
    }
    sdcard_state *s = (sdcard_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(sdcard_state));
    s->state = SDCARD_IDLE;
}

static void sdcard_spi_start(struct srd_decoder_inst *di)
{
    sdcard_state *s = (sdcard_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "sdcard_spi");
}

static void sdcard_spi_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void sdcard_spi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== Decoder struct ===== */
struct srd_c_decoder sdcard_spi_c_decoder = {
    .id = "sdcard_spi_c",
    .name = "SD Card SPI(C)",
    .longname = "Secure Digital card SPI mode (C)",
    .desc = "SD card SPI mode low-level protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = sdcard_spi_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = sdcard_spi_ann_rows,
    .inputs = sdcard_spi_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = sdcard_spi_tags,
    .num_tags = 1,
    .reset = sdcard_spi_reset,
    .start = sdcard_spi_start,
    .decode = sdcard_spi_decode,
    .destroy = sdcard_spi_destroy,
    .decode_upper = sdcard_spi_recv_proto,
    .state_size = 0,
};

/* ===== Export functions ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &sdcard_spi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}