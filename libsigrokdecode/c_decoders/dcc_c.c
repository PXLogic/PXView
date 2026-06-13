/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2013-2020 Sven Bursch-Osewold
 *               2020      Roland Noell
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
#include <math.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum dcc_ann {
    ANN_BITS = 0,
    ANN_BITS_OTHER,
    ANN_FRAME,
    ANN_FRAME_OTHER,
    ANN_DATA,
    ANN_DATA_ACC,
    ANN_DATA_DEC,
    ANN_DATA_CV,
    ANN_COMMAND,
    ANN_ERROR,
    ANN_SEARCH_ACC,
    ANN_SEARCH_DEC,
    ANN_SEARCH_CV,
    ANN_SEARCH_BYTE,
    NUM_ANN,
};

enum dcc_status {
    DCC_WAITINGFORPREAMBLE,
    DCC_PREAMBLE,
    DCC_ADDRESSDATABYTE,
};

#define MAX_PACKET_BYTES 16
#define MAX_INTERFERING_PULSE_WIDTH 4

typedef struct {
    uint64_t dccStart;
    uint64_t dccLast;
    int dccBitCounter;
    uint64_t dccBitPos[9];
    int dccValue;

    int decodedBytes[MAX_PACKET_BYTES];
    uint64_t decodedBytesStart[MAX_PACKET_BYTES];
    uint64_t decodedBytesEnd[MAX_PACKET_BYTES];
    int decodedBytesCount;

    int dccStatus;
    int syncSignal;
    int cond1; /* 0=rise, 1=fall */
    int cond2;

    int64_t dec_addr_search;
    int64_t acc_addr_search;
    int64_t cv_addr_search;
    int64_t byte_search;
    int speed14;
    int serviceMode;
    int64_t AddrOffset;
    int ignoreInterferingPulse;

    uint64_t edge_1, edge_2, edge_3, edge_4;
    uint64_t samplerate;
    double accuracy;
    int out_ann;
    int firstChangeCond;
} dcc_state;

static const char *weekday[] = { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" };
static const char *weekday_short[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
static const char *month[] = { "?", "Jan. ", "Feb. ", "Mar. ", "Apr. ", "Mai ", "Jun. ", "Jul. ", "Aug. ", "Sep. ", "Oct. ", "Nov. ", "Dec. " };

static struct srd_channel dcc_channels[] = {
    { "data", "D0", "Data line", 0, SRD_CHANNEL_SDATA, NULL },
};

static struct srd_decoder_option dcc_options[] = {
    { "CV_29_1", NULL, "CV29 Bit 1", NULL, NULL },
    { "Mode_112_127", NULL, "addr. 112-127", NULL, NULL },
    { "Addr_offset", NULL, "accessory addr. offset", NULL, NULL },
    { "Search_acc_addr", NULL, "search acc. addr. [dec]", NULL, NULL },
    { "Search_dec_addr", NULL, "search dec. addr. [dec]", NULL, NULL },
    { "Search_cv", NULL, "search CV [dec]", NULL, NULL },
    { "Search_byte", NULL, "search byte [dec/0b/0x]", NULL, NULL },
    { "Ignore_short_pulse", NULL, "ignore pulse <= 4 µs", NULL, NULL },
};

static const char* dcc_ann_labels[][3] = {
    { "", "Bits", "Bits" },
    { "", "Other", "Other" },
    { "", "Frame", "Frame" },
    { "", "Other", "Other" },
    { "", "Data", "Data" },
    { "", "Accessory address", "Accessory address" },
    { "", "Decoder address", "Decoder address" },
    { "", "CV", "CV" },
    { "", "Command", "Command" },
    { "", "Error", "Error" },
    { "", "Accessory address", "Accessory address" },
    { "", "Decoder address", "Decoder address" },
    { "", "CV", "CV" },
    { "", "Byte", "Byte" },
};

static const int dcc_row_bits_classes[] = { ANN_BITS, ANN_BITS_OTHER, -1 };
static const int dcc_row_frame_classes[] = { ANN_FRAME, ANN_FRAME_OTHER, -1 };
static const int dcc_row_data_classes[] = { ANN_DATA_ACC, ANN_DATA_DEC, ANN_DATA_CV, ANN_DATA, -1 };
static const int dcc_row_command_classes[] = { ANN_COMMAND, -1 };
static const int dcc_row_error_classes[] = { ANN_ERROR, -1 };
static const int dcc_row_search_classes[] = { ANN_SEARCH_ACC, ANN_SEARCH_DEC, ANN_SEARCH_CV, ANN_SEARCH_BYTE, -1 };

static const struct srd_c_ann_row dcc_ann_rows[] = {
    { "bits_", "Bits", dcc_row_bits_classes, 2 },
    { "frame_", "Frame", dcc_row_frame_classes, 2 },
    { "data_", "Data", dcc_row_data_classes, 4 },
    { "command_", "Command", dcc_row_command_classes, 1 },
    { "error_", "Error", dcc_row_error_classes, 1 },
    { "search_", "Search", dcc_row_search_classes, 4 },
};

static const char* dcc_inputs[] = { "logic" };
static const char* dcc_tags[] = { "Encoding" };

#define DCC_PUTX(di, s, start, end, cls, ...) c_put(di, start, end, (s)->out_ann, cls, __VA_ARGS__)

#define DCC_PUT_SIGNAL(di, s, cls, ...) c_put(di, (s)->edge_1, (s)->edge_3, (s)->out_ann, cls, __VA_ARGS__)

#define DCC_PUT_PACKETBYTE(di, s, pos, cls, ...) do { \
    if ((pos) >= 0 && (pos) < (s)->decodedBytesCount) \
        c_put(di, (s)->decodedBytesStart[pos], (s)->decodedBytesEnd[pos], (s)->out_ann, cls, __VA_ARGS__); \
} while(0)

#define DCC_PUT_PACKETBYTES(di, s, start, end, cls, ...) do { \
    if ((start) >= 0 && (start) < (s)->decodedBytesCount && (end) >= 0 && (end) < (s)->decodedBytesCount) \
        c_put(di, (s)->decodedBytesStart[start], (s)->decodedBytesEnd[end], (s)->out_ann, cls, __VA_ARGS__); \
} while(0)

static int dcc_incPos(struct srd_decoder_inst *di, dcc_state *s, int pos)
{
    if (pos + 1 < s->decodedBytesCount)
        return pos + 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "Byte missing at next position: %d", pos + 2);
    DCC_PUT_PACKETBYTE(di, s, pos, ANN_ERROR, buf);
    return pos;
}

static void dcc_setNextStatus(dcc_state *s, int newstatus)
{
    s->dccStatus = newstatus;
    s->dccBitCounter = 0;
    s->decodedBytesCount = 0;
}

static void dcc_handleDecodedBytes(struct srd_decoder_inst *di, dcc_state *s)
{
    int validPacketFound = 0;
    int64_t acc_addr = -1;
    int64_t dec_addr = -1;
    int64_t cv_addr = -1;
    char buf[256], buf2[128], buf3[64];

    if (s->decodedBytesCount < 3) {
        snprintf(buf, sizeof(buf), "Paket too short: %d Byte only", s->decodedBytesCount);
        DCC_PUT_PACKETBYTES(di, s, 0, s->decodedBytesCount - 1, ANN_ERROR, buf);
        return;
    }

    int pos = 0;
    int idPacket = s->decodedBytes[pos];

    /* Servicemode */
    if (s->serviceMode && 112 <= idPacket && idPacket <= 127) {
        if ((s->decodedBytes[pos] >> 4) == 0b0111 && s->decodedBytesCount == 3) {
            if (((s->decodedBytes[pos] >> 3) & 1) == 0) {
                snprintf(buf, sizeof(buf), "Verify, Register:%d", (s->decodedBytes[pos] & 0b111) + 1);
                snprintf(buf2, sizeof(buf2), "v, R:%d", (s->decodedBytes[pos] & 0b111) + 1);
            } else {
                snprintf(buf, sizeof(buf), "Write, Register:%d", (s->decodedBytes[pos] & 0b111) + 1);
                snprintf(buf2, sizeof(buf2), "w, R:%d", (s->decodedBytes[pos] & 0b111) + 1);
            }
            DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
            pos = dcc_incPos(di, s, pos);
            if (pos == s->decodedBytesCount - 1 && s->decodedBytes[pos - 1] == 0b01111101 && s->decodedBytes[pos] == 1) {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Register/Page Mode (outdated): Page Preset");
            } else {
                snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
            }
            DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_COMMAND, "Register/Page Mode (outdated)");
            validPacketFound = 1;
        } else if ((s->decodedBytes[pos] >> 4) == 0b0111 && s->decodedBytesCount == 4) {
            DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Service Mode", "Service");
            if ((s->decodedBytes[pos] >> 2) == 0b01) {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Verify byte", "v");
                pos = dcc_incPos(di, s, pos);
                cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                pos = dcc_incPos(di, s, pos);
                snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Value");
            } else if ((s->decodedBytes[pos] >> 2) == 0b11) {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Write byte", "w");
                pos = dcc_incPos(di, s, pos);
                cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                pos = dcc_incPos(di, s, pos);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Value");
                snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
            } else if ((s->decodedBytes[pos] >> 2) == 0b10) {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Bit manipulation", "bit");
                pos = dcc_incPos(di, s, pos);
                cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                pos = dcc_incPos(di, s, pos);
                const char *op_long = ((s->decodedBytes[pos] & 0b00010000) == 0b00010000) ? "Write, " : "Verify, ";
                const char *op_short = ((s->decodedBytes[pos] & 0b00010000) == 0b00010000) ? "w," : "v,";
                snprintf(buf, sizeof(buf), "%s%d", op_long, s->decodedBytes[pos] & 0b00000111);
                snprintf(buf2, sizeof(buf2), "%s%d", op_short, s->decodedBytes[pos] & 0b00000111);
                if ((s->decodedBytes[pos] & 0b00001000) == 0b00001000) {
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", 1");
                    snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), ",1");
                } else {
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", 0");
                    snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), ",0");
                }
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Operation, Position, Value", "Op.,Pos,Value", "O,P,V");
            } else {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Reserved for future use", "Res.");
            }
            validPacketFound = 1;
        }
    }

    /* Normal (Not Servicemode) */
    if ((!s->serviceMode) || (s->serviceMode && !(112 <= idPacket && idPacket <= 127))) {
        pos = 0;
        if ((0 <= idPacket && idPacket <= 127) || (192 <= idPacket && idPacket <= 231)) {
            /* Multi-Function Decoder */
            if (idPacket == 0) {
                dec_addr = 0;
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_DEC, "Broadcast");
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Broadcast");
            } else if (1 <= idPacket && idPacket <= 127) {
                dec_addr = s->decodedBytes[pos] & 0b01111111;
                snprintf(buf, sizeof(buf), "%lld", (long long)dec_addr);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_DEC, buf);
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Multi Function Decoder with 7 bit address", "Decoder with 7 bit address", "7 bit addr.");
            } else if (192 <= idPacket && idPacket <= 231) {
                pos = dcc_incPos(di, s, pos);
                dec_addr = ((s->decodedBytes[pos - 1] & 0b00111111) * 256) + s->decodedBytes[pos];
                snprintf(buf, sizeof(buf), "%lld", (long long)dec_addr);
                DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_DATA_DEC, buf);
                DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_COMMAND, "Multi Function Decoder with 14 bit address", "Decoder with 14 bit address", "14 bit addr.");
            }

            pos = dcc_incPos(di, s, pos);
            int cmd = (s->decodedBytes[pos] & 0b11100000) >> 5;
            int subcmd = (s->decodedBytes[pos] & 0b00011111);

            if (cmd == 0b000) {
                /* Decoder Control */
                if (subcmd == 0b00000) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, dec_addr == 0 ? "Decoder Reset packet" : "Decoder Reset", "Dec. Reset", "Reset");
                } else if (subcmd == 0b00001) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Decoder Hard Reset", "Hard Reset", "Reset");
                } else if ((subcmd & 0b11110) == 0b00010) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Factory Test Instruction", "Fac. Test", "Test");
                    validPacketFound = 1;
                } else if ((subcmd & 0b11110) == 0b01010) {
                    snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos] & 0b00000001);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Set Advanced Addressing (CV #29 Bit 5)", "Set advanced addressing", "Set adv. addr.");
                } else if (subcmd == 0b01111) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Decoder Acknowledgment Request", "Dec. Ack Req.", "Ack Req.");
                } else if ((subcmd & 0b10000) == 0b10000) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Consist Control");
                    pos = dcc_incPos(di, s, pos);
                    if ((subcmd & 0b11110) == 0b10010) {
                        const char *dir = (s->decodedBytes[pos - 1] & 1) == 0 ? "normal" : "reverse";
                        snprintf(buf, sizeof(buf), "%d, dir:%s", s->decodedBytes[pos] & 0b01111111, dir);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Set consist address", "Set");
                    } else {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Reserved");
                    }
                } else {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Reserved");
                }
            } else if (cmd == 0b001) {
                /* Advanced Operations Instruction */
                if (subcmd == 0b11111) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "128 Speed Step Control - Instruction");
                    pos = dcc_incPos(di, s, pos);
                    const char *dir_l, *dir_s;
                    if (dec_addr == 0) { dir_l = "Broadcast"; dir_s = "B"; }
                    else if (s->decodedBytes[pos] >> 7 == 1) { dir_l = "Forward"; dir_s = "F"; }
                    else { dir_l = "Reverse"; dir_s = "R"; }
                    if ((s->decodedBytes[pos] & 0b01111111) == 0b00000000) {
                        snprintf(buf, sizeof(buf), "STOP (%s)", dir_l);
                        snprintf(buf2, sizeof(buf2), "STOP (%s)", dir_s);
                    } else if ((s->decodedBytes[pos] & 0b01111111) == 0b00000001) {
                        snprintf(buf, sizeof(buf), "EMERGENCY STOP (HALT) (%s)", dir_l);
                        snprintf(buf2, sizeof(buf2), "ESTOP (%s)", dir_s);
                    } else {
                        int speed = (s->decodedBytes[pos] & 0b01111111) - 1;
                        snprintf(buf, sizeof(buf), "%s Speed: %d / 126", dir_l, speed);
                        snprintf(buf2, sizeof(buf2), "%s:%d", dir_s, speed);
                    }
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                } else if (subcmd == 0b11110) {
                    pos = dcc_incPos(di, s, pos);
                    DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_COMMAND, "Special operation mode (unless received via consist address in CV#19)", "Special operation mode");
                    buf[0] = '\0';
                    int v = (s->decodedBytes[pos] >> 2) & 0b11;
                    if (v == 0b00) snprintf(buf, sizeof(buf), "Not part of a multiple traction");
                    else if (v == 0b10) snprintf(buf, sizeof(buf), "Leading loco of multiple traction");
                    else if (v == 0b01) snprintf(buf, sizeof(buf), "Middle loco in a multiple traction");
                    else snprintf(buf, sizeof(buf), "Final loco of a multiple traction");
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", shunting key:%d, west-bit:%d, east-bit:%d, MAN-bit:%d",
                             (s->decodedBytes[pos] >> 4) & 1, (s->decodedBytes[pos] >> 5) & 1,
                             (s->decodedBytes[pos] >> 6) & 1, (s->decodedBytes[pos] >> 7) & 1);
                    DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_DATA, buf);
                } else if (subcmd == 0b11101) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Analog Function Group");
                    pos = dcc_incPos(di, s, pos);
                    if (s->decodedBytes[pos] == 0b00000001)
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Volume control");
                    else if (0b00010000 <= s->decodedBytes[pos] && s->decodedBytes[pos] <= 0b00011111) {
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos] & 0b00001111);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Position control");
                    } else if (0b10000000 <= s->decodedBytes[pos]) {
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos] & 0b01111111);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Any control");
                    } else
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Reserved");
                    pos = dcc_incPos(di, s, pos);
                    snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data");
                } else if (subcmd == 0b11100) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Speed, Direction, Function");
                    pos = dcc_incPos(di, s, pos);
                    const char *dir_l, *dir_s;
                    if (dec_addr == 0) { dir_l = "Broadcast"; dir_s = "B"; }
                    else if (s->decodedBytes[pos] >> 7 == 1) { dir_l = "Forward"; dir_s = "F"; }
                    else { dir_l = "Reverse"; dir_s = "R"; }
                    if ((s->decodedBytes[pos] & 0b01111111) == 0) {
                        snprintf(buf, sizeof(buf), "STOP (%s)", dir_l);
                        snprintf(buf2, sizeof(buf2), "STOP (%s)", dir_s);
                    } else if ((s->decodedBytes[pos] & 0b01111111) == 1) {
                        snprintf(buf, sizeof(buf), "EMERGENCY STOP (HALT) (%s)", dir_l);
                        snprintf(buf2, sizeof(buf2), "ESTOP (%s)", dir_s);
                    } else {
                        int speed = (s->decodedBytes[pos] & 0b01111111) - 1;
                        snprintf(buf, sizeof(buf), "%s Speed: %d / 126", dir_l, speed);
                        snprintf(buf2, sizeof(buf2), "%s:%d", dir_s, speed);
                    }
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                    int numbers[] = {0, 8, 16, 24};
                    for (int fi = 0; fi < 4; fi++) {
                        int f = numbers[fi];
                        if (s->decodedBytesCount > pos + 2) {
                            pos = dcc_incPos(di, s, pos);
                            int value = s->decodedBytes[pos];
                            buf[0] = '\0';
                            snprintf(buf2, sizeof(buf2), "F%d:", f);
                            for (int i = 0; i < 8; i++) {
                                snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "F%d:%d", f + i, value & 1);
                                snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), "%d", value & 1);
                                if (i < 7) { snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", "); snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), ","); }
                                value >>= 1;
                            }
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                        } else break;
                    }
                } else {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Reserved");
                }
            } else if (cmd == 0b010 || cmd == 0b011) {
                if (s->speed14)
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Basis Speed and Direction Instruction 14 speed step mode (CV#29=0)", "Speed + Dir. 14 step", "Speed 14");
                else
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Basis Speed and Direction Instruction 28 speed step mode (CV#29=1)", "Speed + Dir. 28 step", "Speed 28");
                int bit5 = (subcmd & 0b10000) >> 4;
                const char *dir14_l, *dir14_s, *dir28_l, *dir28_s;
                if (dec_addr == 0) { dir14_l = "Broadcast"; dir14_s = "B"; }
                else if ((cmd & 0b001) == 0b001) { dir14_l = "Forward"; dir14_s = "F"; }
                else { dir14_l = "Reverse"; dir14_s = "R"; }
                dir28_l = dir14_l; dir28_s = dir14_s;
                char out14_l[128], out14_s[64], out28_l[128], out28_s[64];
                if ((subcmd & 0b01111) == 0b00000) {
                    snprintf(out14_l, sizeof(out14_l), "STOP (%s)", dir14_l);
                    snprintf(out14_s, sizeof(out14_s), "STOP (%s)", dir14_s);
                    snprintf(out28_l, sizeof(out28_l), "STOP (%s)", dir28_l);
                    snprintf(out28_s, sizeof(out28_s), "STOP (%s)", dir28_s);
                } else if ((subcmd & 0b01111) == 0b00001) {
                    snprintf(out14_l, sizeof(out14_l), "EMERGENCY STOP (HALT) (%s)", dir14_l);
                    snprintf(out14_s, sizeof(out14_s), "ESTOP (%s)", dir14_s);
                    snprintf(out28_l, sizeof(out28_l), "EMERGENCY STOP (HALT) (%s)", dir28_l);
                    snprintf(out28_s, sizeof(out28_s), "ESTOP (%s)", dir28_s);
                } else {
                    int speed14_val = (subcmd & 0b1111) - 1;
                    int speed28_val = ((((subcmd & 0b01111) - 1) * 2) - 1) + bit5;
                    snprintf(out14_l, sizeof(out14_l), "%s Speed: %d / 14", dir14_l, speed14_val);
                    snprintf(out14_s, sizeof(out14_s), "%s:%d", dir14_s, speed14_val);
                    snprintf(out28_l, sizeof(out28_l), "%s Speed: %d / 28", dir28_l, speed28_val);
                    snprintf(out28_s, sizeof(out28_s), "%s:%d", dir28_s, speed28_val);
                }
                if (dec_addr > 0) {
                    snprintf(out14_l + strlen(out14_l), sizeof(out14_l) - strlen(out14_l), ", F0=%d", bit5);
                    snprintf(out14_s + strlen(out14_s), sizeof(out14_s) - strlen(out14_s), ", F0=%d", bit5);
                }
                if (s->speed14)
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, out14_l, out14_s);
                else
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, out28_l, out28_s);
            } else if (cmd == 0b100) {
                if (s->speed14)
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Function Group One Instruction 14 speed step mode (CV#29=0)", "FG1 14 step", "FG1");
                else
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Function Group One Instruction 28/128 speed step mode (CV#29=1)", "FG1 28/128 step", "FG1");
                int f = 1, value = subcmd;
                buf[0] = '\0'; buf2[0] = '\0';
                for (int i = 0; i < 4; i++) {
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "F%d:%d", f, value & 1);
                    snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), "%d", value & 1);
                    if (i < 3) { snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", "); snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), ","); }
                    value >>= 1; f++;
                }
                char fg_buf[256];
                if (s->speed14) {
                    snprintf(fg_buf, sizeof(fg_buf), "F1:%s", buf2);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, fg_buf);
                } else {
                    snprintf(fg_buf, sizeof(fg_buf), "F0:%d, %s", subcmd >> 4, buf);
                    char fg_short[128];
                    snprintf(fg_short, sizeof(fg_short), "F0:%d,%s", subcmd >> 4, buf2);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, fg_buf, fg_short);
                }
            } else if (cmd == 0b101) {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Function Group Two Instruction", "FG2");
                int f = (subcmd & 0b10000) ? 5 : 9;
                int value = subcmd;
                buf[0] = '\0';
                snprintf(buf2, sizeof(buf2), "F%d:", f);
                for (int i = 0; i < 4; i++) {
                    snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "F%d:%d", f, value & 1);
                    snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), "%d", value & 1);
                    if (i < 3) { snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", "); snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), ","); }
                    value >>= 1; f++;
                }
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
            } else if (cmd == 0b110) {
                pos = dcc_incPos(di, s, pos);
                DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Future Expansion Instruction");
                if (subcmd == 0b11110 || subcmd == 0b11111 || subcmd == 0b11100 || subcmd == 0b11011 || subcmd == 0b11010 || subcmd == 0b11001 || subcmd == 0b11000) {
                    int f = 0;
                    if (subcmd == 0b11110) f = 13; else if (subcmd == 0b11111) f = 21;
                    else if (subcmd == 0b11000) f = 29; else if (subcmd == 0b11001) f = 37;
                    else if (subcmd == 0b11010) f = 45; else if (subcmd == 0b11011) f = 53;
                    else if (subcmd == 0b11100) f = 61;
                    int value = s->decodedBytes[pos];
                    buf[0] = '\0';
                    snprintf(buf2, sizeof(buf2), "F%d:", f);
                    for (int i = 0; i < 8; i++) {
                        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "F%d:%d", f + i, value & 1);
                        snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), "%d", value & 1);
                        if (i < 7) { snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", "); snprintf(buf2 + strlen(buf2), sizeof(buf2) - strlen(buf2), ","); }
                        value >>= 1;
                    }
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                } else if (subcmd == 0b11101) {
                    int address = s->decodedBytes[pos] & 0b01111111;
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA, "Binary State Control Instruction short form", "Binarystate short");
                    if (address == 0) {
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos] >> 7);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Broadcast F29-F127");
                    } else if (1 <= address && address <= 15) {
                        if (address == 1) snprintf(buf, sizeof(buf), "XF=1%s", (s->decodedBytes[pos] >> 7) == 0 ? " (Requesting the location information)" : "");
                        else if (address == 2) snprintf(buf, sizeof(buf), "XF=2%s", (s->decodedBytes[pos] >> 7) == 0 ? " (Rerail search)" : "");
                        else snprintf(buf, sizeof(buf), "XF=%d (Reserved)", address);
                        snprintf(buf2, sizeof(buf2), "XF=%d:%s", address, (s->decodedBytes[pos] >> 7) == 0 ? "off" : "on");
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "RailCom");
                    } else if (16 <= address && address <= 28) {
                        snprintf(buf, sizeof(buf), "%X/%d", s->decodedBytes[pos], s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Special uses");
                    } else {
                        snprintf(buf, sizeof(buf), "F%d:%s", address, (s->decodedBytes[pos - 1] >> 7) == 0 ? "off" : "on");
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                    }
                } else if (subcmd == 0b00000) {
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA, "Binary State Control Instruction long form", "Binarystate long");
                    pos = dcc_incPos(di, s, pos);
                    int address = (s->decodedBytes[pos] * 128) + (s->decodedBytes[pos - 1] & 0b01111111);
                    const char *onoff = (s->decodedBytes[pos - 1] >> 7) == 0 ? "off" : "on";
                    if (address == 0) {
                        DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_DATA, onoff);
                        DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_COMMAND, "Broadcast F29-F32767");
                    } else if ((s->decodedBytes[pos - 1] & 0b01111111) == 0) {
                        DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_ERROR, "Use binarystate short");
                    } else {
                        snprintf(buf, sizeof(buf), "F%d:%s", address, onoff);
                        DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_DATA, buf);
                    }
                } else if (subcmd == 0b00001) {
                    if (dec_addr != 0)
                        DCC_PUT_PACKETBYTES(di, s, 0, s->decodedBytesCount - 2, ANN_ERROR, "Only Broadcast allowed");
                    int value = s->decodedBytes[pos];
                    if ((value >> 6) == 0b00) {
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA, "Model-Time");
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "00MMMMMM");
                        pos = dcc_incPos(di, s, pos);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "WWWHHHHH");
                        pos = dcc_incPos(di, s, pos);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "U0BBBBBB");
                        int wd = s->decodedBytes[pos - 1] >> 5;
                        int hr = s->decodedBytes[pos - 1] & 0b00011111;
                        int mn = s->decodedBytes[pos - 2] & 0b00111111;
                        int upd = s->decodedBytes[pos] >> 7;
                        int acc = s->decodedBytes[pos] & 0b00111111;
                        snprintf(buf, sizeof(buf), "%s %02d:%02d hrs, Update:%d, Acceleration:%d", weekday[wd], hr, mn, upd, acc);
                        snprintf(buf2, sizeof(buf2), "%s %02d:%02d, U:%d, Acc:%d", weekday_short[wd], hr, mn, upd, acc);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_DATA, buf, buf2);
                    } else if ((value >> 6) == 0b01) {
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA, "Model-Date");
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "010TTTTT");
                        pos = dcc_incPos(di, s, pos);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "MMMMYYYY");
                        pos = dcc_incPos(di, s, pos);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "YYYYYYYY");
                        int dy = s->decodedBytes[pos - 2] & 0b00011111;
                        int mo = (s->decodedBytes[pos - 1] >> 4);
                        int yr = ((s->decodedBytes[pos - 1] & 0b00001111) << 8) + s->decodedBytes[pos];
                        snprintf(buf, sizeof(buf), "%d. %s%d", dy, mo < 13 ? month[mo] : "?", yr);
                        snprintf(buf2, sizeof(buf2), "%d.%d.%d", dy, mo, yr);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_DATA, buf, buf2);
                    } else {
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA, "Reserved");
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_DATA, "Reserved", "Res.");
                    }
                } else if (subcmd == 0b00010) {
                    if (dec_addr != 0)
                        DCC_PUT_PACKETBYTES(di, s, 0, s->decodedBytesCount - 2, ANN_ERROR, "Only Broadcast allowed");
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA, "Systemtime");
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "MMMMMMMM");
                    int value = s->decodedBytes[pos];
                    pos = dcc_incPos(di, s, pos);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "MMMMMMMM");
                    value = value * 256 + s->decodedBytes[pos];
                    pos = dcc_incPos(di, s, pos);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "MMMMMMMM");
                    value = value * 256 + s->decodedBytes[pos];
                    pos = dcc_incPos(di, s, pos);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "MMMMMMMM");
                    value = value * 256 + s->decodedBytes[pos];
                    snprintf(buf, sizeof(buf), "%d ms since systemstart (%.0f minutes = %.1f hours)", value, value / 60000.0, value / 3600000.0);
                    snprintf(buf2, sizeof(buf2), "%d ms since systemstart", value);
                    char buf3[32]; snprintf(buf3, sizeof(buf3), "%d", value);
                    DCC_PUT_PACKETBYTES(di, s, pos - 3, pos, ANN_DATA, buf, buf2, buf3);
                } else {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Reserved");
                }
            } else if (cmd == 0b111) {
                if (subcmd & 0b10000) {
                    /* Short Form */
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Configuration Variable Access Instruction - Short Form", "CV Access Instruction short", "CV short");
                    if ((subcmd & 0b1111) == 0b0000) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Not available for use", "Not av.");
                    } else if ((subcmd & 0b1111) == 0b0010) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Acceleration Value (CV#23)", "CV#23");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data");
                    } else if ((subcmd & 0b1111) == 0b0011) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Deceleration Value (CV#24)", "CV#24");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data");
                    } else if ((subcmd & 0b1111) == 0b0100) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Write CV#17 + CV#18", "w CV#17+18");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV17");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV18");
                    } else if ((subcmd & 0b1111) == 0b0101) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Write CV#31 + CV#32", "w CV#31+32");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV31");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV32");
                    } else if ((subcmd & 0b1111) == 0b1001) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Reserved (outdated: Service Mode Decoder Lock Instruction)", "Res. (old: Dec. Lock)", "Res.");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos] & 0b01111111);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Short address", "Addr.");
                    } else {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Reserved (maybe service mode packet)", "Reserved", "Res.");
                    }
                } else if ((pos == 1 && s->decodedBytesCount == 5) || (pos == 2 && s->decodedBytesCount == 6)) {
                    /* Long Form (POM) */
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Configuration Variable Access Instruction - Long Form (POM)", "CV Access Instruction long (POM)", "CV long (POM)");
                    if ((subcmd >> 2) == 0b01) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Read/Verify byte", "r/v");
                        pos = dcc_incPos(di, s, pos);
                        cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                        snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Value");
                    } else if ((subcmd >> 2) == 0b11) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Write byte", "w");
                        pos = dcc_incPos(di, s, pos);
                        cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                        snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                        pos = dcc_incPos(di, s, pos);
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Value");
                    } else if ((subcmd >> 2) == 0b10) {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Bit manipulation", "Bit");
                        pos = dcc_incPos(di, s, pos);
                        cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                        snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                        pos = dcc_incPos(di, s, pos);
                        const char *op_l = (s->decodedBytes[pos] & 0b10000) ? "Write, " : "Verify, ";
                        const char *op_s = (s->decodedBytes[pos] & 0b10000) ? "w," : "v,";
                        snprintf(buf, sizeof(buf), "%s%d%s%d", op_l, s->decodedBytes[pos] & 0b111, (s->decodedBytes[pos] & 0b1000) ? ", 1" : ", 0", 0);
                        snprintf(buf2, sizeof(buf2), "%s%d%s%d", op_s, s->decodedBytes[pos] & 0b111, (s->decodedBytes[pos] & 0b1000) ? ",1" : ",0", 0);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Operation, Position, Value", "Op.,Pos,Value", "O,P,V");
                    } else {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Reserved for future use", "Res.");
                    }
                } else if ((pos == 1 && s->decodedBytesCount >= 6) || (pos == 2 && s->decodedBytesCount >= 7)) {
                    /* XPOM */
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "XPOM");
                    if ((subcmd >> 2) == 0b01) {
                        snprintf(buf, sizeof(buf), "Read bytes, SS:%d", s->decodedBytes[pos] & 0b11);
                        snprintf(buf2, sizeof(buf2), "r,SS:%d", s->decodedBytes[pos] & 0b11);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                        pos = dcc_incPos(di, s, pos); pos = dcc_incPos(di, s, pos); pos = dcc_incPos(di, s, pos);
                        cv_addr = (s->decodedBytes[pos - 2] * 256 + s->decodedBytes[pos - 1]) * 256 + s->decodedBytes[pos] + 1;
                        snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_DATA_CV, buf);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_COMMAND, "CV");
                    } else if ((subcmd >> 2) == 0b11) {
                        snprintf(buf, sizeof(buf), "Write byte(s), SS:%d", s->decodedBytes[pos] & 0b11);
                        snprintf(buf2, sizeof(buf2), "w,SS:%d", s->decodedBytes[pos] & 0b11);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                        pos = dcc_incPos(di, s, pos); pos = dcc_incPos(di, s, pos); pos = dcc_incPos(di, s, pos);
                        cv_addr = (s->decodedBytes[pos - 2] * 256 + s->decodedBytes[pos - 1]) * 256 + s->decodedBytes[pos] + 1;
                        snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_DATA_CV, buf);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_COMMAND, "CV");
                        pos = dcc_incPos(di, s, pos);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data-1");
                        snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        if (s->decodedBytesCount > pos + 2) { pos = dcc_incPos(di, s, pos); DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data-2"); snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]); DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf); }
                        if (s->decodedBytesCount > pos + 2) { pos = dcc_incPos(di, s, pos); DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data-3"); snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]); DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf); }
                        if (s->decodedBytesCount > pos + 2) { pos = dcc_incPos(di, s, pos); DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Data-4"); snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]); DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf); }
                    } else if ((subcmd >> 2) == 0b10) {
                        snprintf(buf, sizeof(buf), "Bit write, SS:%d", s->decodedBytes[pos] & 0b11);
                        snprintf(buf2, sizeof(buf2), "bit,SS:%d", s->decodedBytes[pos] & 0b11);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                        pos = dcc_incPos(di, s, pos); pos = dcc_incPos(di, s, pos); pos = dcc_incPos(di, s, pos);
                        cv_addr = (s->decodedBytes[pos - 2] * 256 + s->decodedBytes[pos - 1]) * 256 + s->decodedBytes[pos] + 1;
                        snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_DATA_CV, buf);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_COMMAND, "CV");
                        pos = dcc_incPos(di, s, pos);
                        if ((s->decodedBytes[pos] >> 4) == 0b1111) {
                            snprintf(buf, sizeof(buf), "%d%s%d", s->decodedBytes[pos] & 0b111, (s->decodedBytes[pos] & 0b1000) ? ", 1" : ", 0", 0);
                            snprintf(buf2, sizeof(buf2), "%d%s%d", s->decodedBytes[pos] & 0b111, (s->decodedBytes[pos] & 0b1000) ? ",1" : ",0", 0);
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Position, Value", "Pos, Value", "P,V");
                        }
                    } else {
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Reserved for future use", "Res.");
                    }
                }
            }
        } else if (128 <= idPacket && idPacket <= 191) {
            /* Accessory Decoder */
            pos = dcc_incPos(di, s, pos);
            int A1 = s->decodedBytes[pos - 1] & 0b00111111;
            int A2 = (~((s->decodedBytes[pos] >> 4) & 0b0111)) & 0b0111;
            int A3 = (s->decodedBytes[pos] & 0b00000110) >> 1;
            int decoder = (A2 << 6) + A1;
            int port = A3;
            int decaddr = (A2 << 8) + (A1 << 2) + A3 - 3;
            acc_addr = decaddr + s->AddrOffset;
            int pom = 0;

            if (decaddr < 1)
                DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_ERROR, "Address < 1 not allowed");

            if ((s->decodedBytes[pos] & 0b10001000) == 0b00001000) {
                DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Railcom NOP (AccQuery)", "RC NOP");
                snprintf(buf, sizeof(buf), "%lld", (long long)acc_addr);
                DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, buf);
                if ((s->decodedBytes[pos] & 1) == 0)
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Basic Accessory Decoder", "Basic Accessory", "Basic Acc.");
                else
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Extended Accessory Decoder", "Extended Accessory", "Ext. Acc.");
            } else if (s->decodedBytes[pos] & 0b10000000) {
                if (s->decodedBytesCount == 3 || s->decodedBytesCount == 4) {
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Basic Accessory Decoder", "Basic Accessory", "Basic Acc.");
                    if (acc_addr + 3 == 2047) {
                        if ((((s->decodedBytes[pos] >> 3) & 1) == 0) && ((s->decodedBytes[pos] & 1) == 0)) {
                            DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, "Broadcast");
                            DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Broadcast");
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "ESTOP");
                        } else {
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_ERROR, "Unknown (maybe NMRA-Broadcast)", "Unknown");
                        }
                    } else {
                        if (s->decodedBytesCount == 3) {
                            snprintf(buf, sizeof(buf), "%lld (decoder:%d, port:%d)", (long long)acc_addr, decoder, port);
                            snprintf(buf2, sizeof(buf2), "%lld (%d,%d)", (long long)acc_addr, decoder, port);
                            snprintf(buf3, sizeof(buf3), "%lld", (long long)acc_addr);
                            DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, buf, buf2, buf3);
                            const char *onoff = ((s->decodedBytes[pos] >> 3) & 1) ? "on" : "off";
                            snprintf(buf, sizeof(buf), "%d:%s", s->decodedBytes[pos] & 1, onoff);
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        } else if (s->decodedBytesCount == 4 && (s->decodedBytes[pos] & 0b1001) == 0b0000) {
                            pos = dcc_incPos(di, s, pos);
                            if (s->decodedBytes[pos] == 0) {
                                snprintf(buf, sizeof(buf), "%lld (decoder:%d, port:%d)", (long long)acc_addr, decoder, port);
                                DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, buf);
                                DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Decoder reset", "Reset");
                            } else {
                                DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_ERROR, "Unknown");
                            }
                        } else {
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_ERROR, "Unknown");
                        }
                    }
                } else if (s->decodedBytesCount == 6) {
                    pos = dcc_incPos(di, s, pos);
                    if ((s->decodedBytes[pos] >> 4) == 0b1110) {
                        pom = 1;
                        DCC_PUT_PACKETBYTE(di, s, pos - 2, ANN_COMMAND, "POM for Basic Accessory Decoder", "POM Basic Accessory", "POM Basic Acc.");
                        snprintf(buf, sizeof(buf), "%lld (decoder:%d, port:%d)", (long long)acc_addr, decoder, port);
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Address", "Addr.");
                    } else {
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_ERROR, "Unknown");
                    }
                }
            } else {
                if (s->decodedBytesCount == 4) {
                    DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Extended Accessory Decoder Control Packet", "Extended Accessory", "Ext. Acc.");
                    pos = dcc_incPos(di, s, pos);
                    if (acc_addr + 3 == 2047) {
                        if (s->decodedBytes[pos] == 0) {
                            DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, "Broadcast");
                            DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Broadcast");
                            DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "ESTOP");
                        } else {
                            DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_ERROR, "Unknown");
                        }
                    } else {
                        snprintf(buf, sizeof(buf), "%lld (decoder:%d, port:%d)", (long long)acc_addr, decoder, port);
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos - 1, ANN_DATA_ACC, buf);
                        snprintf(buf, sizeof(buf), "Aspect:%X/%d", s->decodedBytes[pos], s->decodedBytes[pos]);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                        const char *sw_time;
                        if ((s->decodedBytes[pos] & 0b01111111) == 0b01111111) sw_time = "on";
                        else if ((s->decodedBytes[pos] & 0b01111111) == 0b00000000) sw_time = "off";
                        else { snprintf(buf2, sizeof(buf2), "%d", s->decodedBytes[pos] & 0b01111111); sw_time = buf2; }
                        snprintf(buf, sizeof(buf), "Switching time:%s, output:%d", sw_time, s->decodedBytes[pos] >> 7);
                        DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, buf);
                    }
                } else if (s->decodedBytesCount == 6) {
                    pos = dcc_incPos(di, s, pos);
                    if ((s->decodedBytes[pos] >> 4) == 0b1110) {
                        pom = 1;
                        DCC_PUT_PACKETBYTE(di, s, pos - 2, ANN_COMMAND, "POM for Extended Accessory Decoder", "POM Extended Accessory", "POM Extended Acc.");
                        snprintf(buf, sizeof(buf), "%lld (decoder:%d, port:%d)", (long long)acc_addr, decoder, port);
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_DATA_ACC, buf);
                        DCC_PUT_PACKETBYTE(di, s, pos - 1, ANN_COMMAND, "Address", "Addr.");
                    } else {
                        DCC_PUT_PACKETBYTES(di, s, pos - 2, pos, ANN_ERROR, "Unknown");
                    }
                }
            }

            if (pom) {
                int pom_subcmd = s->decodedBytes[pos] & 0b00011111;
                if ((pom_subcmd >> 2) == 0b01) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Read/Verify byte", "r/v");
                    pos = dcc_incPos(di, s, pos);
                    cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                    snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                    pos = dcc_incPos(di, s, pos);
                    snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Value");
                } else if ((pom_subcmd >> 2) == 0b11) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Write byte", "w");
                    pos = dcc_incPos(di, s, pos);
                    cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                    snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                    pos = dcc_incPos(di, s, pos);
                    snprintf(buf, sizeof(buf), "%d", s->decodedBytes[pos]);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Value");
                } else if ((pom_subcmd >> 2) == 0b10) {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Bit manipulation", "Bit");
                    pos = dcc_incPos(di, s, pos);
                    cv_addr = (s->decodedBytes[pos - 1] & 0b00000011) * 256 + s->decodedBytes[pos] + 1;
                    snprintf(buf, sizeof(buf), "%lld", (long long)cv_addr);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA_CV, buf);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "CV");
                    pos = dcc_incPos(di, s, pos);
                    const char *op_l = (s->decodedBytes[pos] & 0b10000) ? "Write, " : "Verify, ";
                    const char *op_s = (s->decodedBytes[pos] & 0b10000) ? "w," : "v,";
                    snprintf(buf, sizeof(buf), "%s%d%s%d", op_l, s->decodedBytes[pos] & 0b111, (s->decodedBytes[pos] & 0b1000) ? ", 1" : ", 0", 0);
                    snprintf(buf2, sizeof(buf2), "%s%d%s%d", op_s, s->decodedBytes[pos] & 0b111, (s->decodedBytes[pos] & 0b1000) ? ",1" : ",0", 0);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, buf, buf2);
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Operation, Position, Value", "Op.,Pos,Value", "O,P,V");
                } else {
                    DCC_PUT_PACKETBYTE(di, s, pos, ANN_DATA, "Reserved for future use", "Res.");
                }
            }
        } else if (232 <= idPacket && idPacket <= 254) {
            DCC_PUT_PACKETBYTE(di, s, pos, ANN_COMMAND, "Reserved");
        } else if (idPacket == 255) {
            pos = dcc_incPos(di, s, pos);
            if (s->decodedBytes[pos] == 0) {
                DCC_PUT_PACKETBYTES(di, s, pos - 1, pos, ANN_COMMAND, "Idle");
            } else {
                validPacketFound = 1;
                DCC_PUT_PACKETBYTES(di, s, pos - 1, pos - 1, ANN_COMMAND, "RailComPlus®");
                if (s->decodedBytesCount >= 5 && pos + 1 < s->decodedBytesCount && s->decodedBytes[pos + 1] == 62 && s->decodedBytes[pos + 2] == 7 && s->decodedBytes[pos + 3] == 64) {
                    DCC_PUT_PACKETBYTES(di, s, pos, s->decodedBytesCount - 2, ANN_COMMAND, "System command (not documented) (IDNotify?)", "System command");
                } else {
                    DCC_PUT_PACKETBYTES(di, s, pos, s->decodedBytesCount - 2, ANN_COMMAND, "System command (not documented)", "System command");
                }
                pos = -1;
            }
        }

        /* Remaining bytes */
        if (pos == -1) pos = 0;
        else if (pos == 0) pos = -1;

        for (int x = pos + 1; x < s->decodedBytesCount - 1; x++) {
            snprintf(buf, sizeof(buf), "?:%X/%d", s->decodedBytes[x], s->decodedBytes[x]);
            DCC_PUT_PACKETBYTE(di, s, x, ANN_DATA, buf);
            if (!validPacketFound) {
                DCC_PUT_PACKETBYTE(di, s, x, ANN_COMMAND, buf);
                if (!s->serviceMode && 112 <= idPacket && idPacket <= 127)
                    DCC_PUT_PACKETBYTE(di, s, x, ANN_ERROR, "Unknown (maybe service mode packet)", "Unknown");
                else if (s->serviceMode)
                    DCC_PUT_PACKETBYTE(di, s, x, ANN_ERROR, "Unknown (maybe operation mode packet)", "Unknown");
                else
                    DCC_PUT_PACKETBYTE(di, s, x, ANN_ERROR, "Unknown");
            }
        }

        /* Checksum */
        if (pos + 1 < s->decodedBytesCount) {
            int checksum = s->decodedBytes[0];
            for (int x = 1; x < s->decodedBytesCount - 1; x++)
                checksum ^= s->decodedBytes[x];
            if (checksum == s->decodedBytes[s->decodedBytesCount - 1]) {
                DCC_PUT_PACKETBYTE(di, s, s->decodedBytesCount - 1, ANN_FRAME, "Checksum: OK", "OK");
            } else {
                DCC_PUT_PACKETBYTES(di, s, 0, s->decodedBytesCount - 1, ANN_ERROR, "Checksum");
                snprintf(buf, sizeof(buf), "Checksum: %d<>%d", checksum, s->decodedBytes[s->decodedBytesCount - 1]);
                DCC_PUT_PACKETBYTE(di, s, s->decodedBytesCount - 1, ANN_FRAME_OTHER, buf, buf + 10);
            }
        } else {
            DCC_PUT_PACKETBYTES(di, s, 0, s->decodedBytesCount - 1, ANN_ERROR, "Checksum missing");
        }

        /* Search function */
        int byte_found = 0;
        for (int x = 0; x < s->decodedBytesCount; x++) {
            if (s->byte_search == s->decodedBytes[x]) {
                byte_found = 1;
                if ((s->dec_addr_search < 0 && s->acc_addr_search < 0 && s->cv_addr_search < 0) ||
                    dec_addr == s->dec_addr_search || acc_addr == s->acc_addr_search || cv_addr == s->cv_addr_search) {
                    snprintf(buf, sizeof(buf), "BYTE:%X/%lld", (unsigned)s->byte_search, (long long)s->byte_search);
                    DCC_PUT_PACKETBYTE(di, s, x, ANN_SEARCH_BYTE, buf);
                }
            }
        }
        if (s->dec_addr_search == dec_addr && (s->byte_search < 0 || byte_found)) {
            snprintf(buf, sizeof(buf), "DECODER:%lld", (long long)s->dec_addr_search);
            DCC_PUT_PACKETBYTE(di, s, 0, ANN_SEARCH_DEC, buf);
        }
        if (s->acc_addr_search == acc_addr && (s->byte_search < 0 || byte_found)) {
            snprintf(buf, sizeof(buf), "ACCESSORY:%lld", (long long)s->acc_addr_search);
            DCC_PUT_PACKETBYTES(di, s, 0, s->decodedBytesCount - 2, ANN_SEARCH_ACC, buf);
        }
        if (s->cv_addr_search == cv_addr && (s->byte_search < 0 || byte_found)) {
            snprintf(buf, sizeof(buf), "CV:%lld", (long long)s->cv_addr_search);
            DCC_PUT_PACKETBYTE(di, s, 1, ANN_SEARCH_CV, buf);
        }
    }
}

static void dcc_collectDataBytes(struct srd_decoder_inst *di, dcc_state *s, uint64_t start, uint64_t stop, const char *value)
{
    if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
        dcc_setNextStatus(s, DCC_WAITINGFORPREAMBLE);
        return;
    }

    if (s->dccStatus == DCC_WAITINGFORPREAMBLE) {
        if (strcmp(value, "1") == 0) {
            s->dccStart = start;
            dcc_setNextStatus(s, DCC_PREAMBLE);
        }
    } else if (s->dccStatus == DCC_PREAMBLE) {
        if (strcmp(value, "1") == 0) {
            s->dccBitCounter++;
            s->dccLast = stop;
        } else {
            if (s->dccBitCounter + 1 + 1 >= 10) {
                char preamble_buf[128], preamble_short[32], preamble_tiny[8];
                snprintf(preamble_buf, sizeof(preamble_buf), "Preamble: %d bits", s->dccBitCounter + 1);
                snprintf(preamble_short, sizeof(preamble_short), "Preamble");
                snprintf(preamble_tiny, sizeof(preamble_tiny), "P");
                if (s->syncSignal) {
                    s->syncSignal = 0;
                    snprintf(preamble_buf + strlen(preamble_buf), sizeof(preamble_buf) - strlen(preamble_buf), " (sync in progress)");
                    snprintf(preamble_short + strlen(preamble_short), sizeof(preamble_short) - strlen(preamble_short), " (sync)");
                    snprintf(preamble_tiny + strlen(preamble_tiny), sizeof(preamble_tiny) - strlen(preamble_tiny), " (s)");
                }
                DCC_PUTX(di, s, start, stop, ANN_FRAME, "Start Packet", "Start", "S");
                DCC_PUTX(di, s, s->dccStart, s->dccLast, ANN_FRAME, preamble_buf, preamble_short, preamble_tiny);
                dcc_setNextStatus(s, DCC_ADDRESSDATABYTE);
            } else {
                if (!s->syncSignal)
                    DCC_PUTX(di, s, s->dccStart, s->dccLast, ANN_ERROR, "Invalid preamble");
                s->syncSignal = 1;
                DCC_PUT_SIGNAL(di, s, ANN_FRAME_OTHER, "Resynchronize (Wait for preamble)", "Resynchronize", "Resync.", "R");
                dcc_setNextStatus(s, DCC_WAITINGFORPREAMBLE);
            }
        }
    } else if (s->dccStatus == DCC_ADDRESSDATABYTE) {
        if (s->dccBitCounter == 0) {
            s->dccValue = 0;
            s->dccStart = start;
        }
        if (s->dccBitCounter < 8) {
            s->dccBitCounter++;
            s->dccValue = (s->dccValue << 1) + (strcmp(value, "1") == 0 ? 1 : 0);
            if (s->dccBitCounter == 8) {
                if (s->decodedBytesCount < MAX_PACKET_BYTES) {
                    s->decodedBytes[s->decodedBytesCount] = s->dccValue;
                    s->decodedBytesStart[s->decodedBytesCount] = s->dccStart;
                    s->decodedBytesEnd[s->decodedBytesCount] = stop;
                    s->decodedBytesCount++;
                }
            }
        } else {
            if (strcmp(value, "0") == 0) {
                s->dccBitCounter = 0;
                s->dccValue = 0;
                DCC_PUTX(di, s, start, stop, ANN_FRAME, "Start Databyte", "Start", "S");
            } else {
                DCC_PUTX(di, s, start, stop, ANN_FRAME, "Stop Packet", "Stop", "S");
                dcc_handleDecodedBytes(di, s);
                dcc_setNextStatus(s, DCC_WAITINGFORPREAMBLE);
            }
        }
    }
}

static void dcc_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(dcc_state)));
    }
    dcc_state *s = (dcc_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(dcc_state));
    s->out_ann = -1;
    s->dccStatus = DCC_WAITINGFORPREAMBLE;
    s->syncSignal = 1;
    s->cond1 = 0; /* rise */
    s->cond2 = 1; /* fall */
    s->dec_addr_search = -2;
    s->acc_addr_search = -2;
    s->cv_addr_search = -2;
    s->byte_search = -2;
    s->firstChangeCond = 1;
}

static void dcc_start(struct srd_decoder_inst *di)
{
    dcc_state *s = (dcc_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "dcc");

    s->AddrOffset = c_opt_int(di, "Addr_offset", 0);
    const char *ignore_str = c_opt_str(di, "Ignore_short_pulse", "no");
    s->ignoreInterferingPulse = (strcmp(ignore_str, "yes") == 0) ? 1 : 0;

    const char *cv29_str = c_opt_str(di, "CV_29_1", "1: 28/128 speed mode");
    s->speed14 = (strcmp(cv29_str, "0: 14 speed mode") == 0) ? 1 : 0;

    const char *mode_str = c_opt_str(di, "Mode_112_127", "operation mode");
    s->serviceMode = (strcmp(mode_str, "service mode") == 0) ? 1 : 0;

    const char *acc_str = c_opt_str(di, "Search_acc_addr", "");
    if (acc_str[0] != '\0') {
        char *endp;
        long long v = strtoll(acc_str, &endp, 10);
        if (*endp == '\0' && v >= 1 && v <= 2047) s->acc_addr_search = v;
    }

    const char *dec_str = c_opt_str(di, "Search_dec_addr", "");
    if (dec_str[0] != '\0') {
        char *endp;
        long long v = strtoll(dec_str, &endp, 10);
        if (*endp == '\0' && v >= 0 && v <= 10239) s->dec_addr_search = v;
    }

    const char *cv_str = c_opt_str(di, "Search_cv", "");
    if (cv_str[0] != '\0') {
        char *endp;
        long long v = strtoll(cv_str, &endp, 10);
        if (*endp == '\0' && v >= 1 && v <= 16777216) s->cv_addr_search = v;
    }

    const char *byte_str = c_opt_str(di, "Search_byte", "");
    if (byte_str[0] != '\0') {
        char *endp;
        long long v = strtoll(byte_str, &endp, 10);
        if (*endp != '\0') { v = strtoll(byte_str, &endp, 2); }
        if (*endp != '\0') { v = strtoll(byte_str, &endp, 16); }
        if (*endp == '\0' && v >= 0 && v <= 255) s->byte_search = v;
    }
}

static void dcc_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    dcc_state *s = (dcc_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (value > 0)
            s->accuracy = 1.0 / (double)value * 1000000.0;
    }
}

static void dcc_decode(struct srd_decoder_inst *di)
{
    dcc_state *s = (dcc_state *)c_decoder_get_private(di);

    if (s->samplerate == 0 || s->samplerate < 25000) return;

    /* Wait for first edge */
    {
        int ret;
        if (s->cond1 == 0) ret = c_wait(di, CW_R(0), CW_END); else ret = c_wait(di, CW_F(0), CW_END);
        if (ret != SRD_OK) return;
        s->edge_1 = di_samplenum(di);
    }
    {
        int ret;
        if (s->cond2 == 0) ret = c_wait(di, CW_R(0), CW_END); else ret = c_wait(di, CW_F(0), CW_END);
        if (ret != SRD_OK) return;
        s->edge_2 = di_samplenum(di);
    }

    /* Info at the start */
    {
        char info_buf[128];
        if (s->samplerate / 1000 < 1000)
            snprintf(info_buf, sizeof(info_buf), "Samplerate: %.0f kHz", (double)s->samplerate / 1000.0);
        else
            snprintf(info_buf, sizeof(info_buf), "Samplerate: %.0f MHz", (double)s->samplerate / 1000000.0);
        if (s->accuracy >= 1)
            snprintf(info_buf + strlen(info_buf), sizeof(info_buf) - strlen(info_buf), ", Accuracy: %.0f µs", s->accuracy);
        else
            snprintf(info_buf + strlen(info_buf), sizeof(info_buf) - strlen(info_buf), ", Accuracy: %.0f ns", s->accuracy * 1000);
        DCC_PUTX(di, s, s->edge_1, s->edge_2, ANN_FRAME_OTHER, info_buf);
    }

    while (1) {
        char output_1[256] = "";
        int unknownTiming = 0;
        int railcomCutout = 0;
        int strechedZero = 0;
        const char *value = NULL;
        char value_buf[128] = "";
        char value_long[128] = "";
        char value_short[64] = "";

        {
            int ret;
            if (s->cond1 == 0) ret = c_wait(di, CW_R(0), CW_END); else ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK) return;
            s->edge_3 = di_samplenum(di);
        }
        {
            int ret;
            if (s->cond2 == 0) ret = c_wait(di, CW_R(0), CW_END); else ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK) return;
            s->edge_4 = di_samplenum(di);
        }

        double total = (double)(s->edge_3 - s->edge_1) / (double)s->samplerate * 1000000.0;
        double part1 = (double)(s->edge_2 - s->edge_1) / (double)s->samplerate * 1000000.0;
        double part2 = (double)(s->edge_3 - s->edge_2) / (double)s->samplerate * 1000000.0;
        double acc = s->accuracy;

        if (52 - acc <= part1 && part1 <= 64 + acc &&
            52 - acc <= part2 && part2 <= 64 + acc &&
            fabs(part1 - part2) <= fmax(6, 2 * acc)) {
            value = "1";
        } else if (((90 - acc <= part1 && part1 <= 10000 + acc && 90 - acc <= part2 && part2 <= 119 + acc)) ||
                   ((90 - acc <= part2 && part2 <= 10000 + acc && 90 - acc <= part1 && part1 <= 119 + acc))) {
            value = "0";
            if (2 * 119 + acc <= total && total <= 12000 + acc) {
                snprintf(output_1, sizeof(output_1), "stretched zero?");
                strechedZero = 1;
            }
        } else if (90 + 52 - acc <= total && total <= 64 + 119 + acc) {
            /* Half '0' + half '1' -> switch edge detection */
            s->cond1 = !s->cond1;
            s->cond2 = !s->cond2;
            if (s->firstChangeCond) {
                s->firstChangeCond = 0;
            } else {
                DCC_PUT_SIGNAL(di, s, ANN_ERROR, "Edge-Detection changed to falling edge - should not occur - dirty signal?");
                DCC_PUT_SIGNAL(di, s, ANN_FRAME_OTHER, "Resynchronize (Wait for preamble)", "Resynchronize", "Resync.", "R");
            }
            s->syncSignal = 1;
            s->decodedBytesCount = 0;
            dcc_setNextStatus(s, DCC_WAITINGFORPREAMBLE);
            /* Skip one edge */
            {
                c_wait(di, CW_E(0), CW_END);
                s->edge_1 = s->edge_4;
                s->edge_2 = di_samplenum(di);
            }
            continue;
        } else {
            snprintf(output_1, sizeof(output_1), "unknown timing");
            unknownTiming = 1;
        }

        /* Filter out short pulses */
        if (s->ignoreInterferingPulse) {
            double p34 = (double)(s->edge_4 - s->edge_3) / (double)s->samplerate * 1000000.0;
            double p23 = (double)(s->edge_3 - s->edge_2) / (double)s->samplerate * 1000000.0;
            if (p34 <= MAX_INTERFERING_PULSE_WIDTH && p23 <= MAX_INTERFERING_PULSE_WIDTH) {
                s->edge_2 = (s->edge_2 + s->edge_4) / 2;
                DCC_PUTX(di, s, s->edge_2, s->edge_4, ANN_ERROR, "Short pulse ignored");
                continue;
            } else if (p34 <= MAX_INTERFERING_PULSE_WIDTH && value && strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
                DCC_PUTX(di, s, s->edge_3, s->edge_4, ANN_ERROR, "Short pulse ignored");
                continue;
            } else if (p23 <= MAX_INTERFERING_PULSE_WIDTH) {
                DCC_PUTX(di, s, s->edge_2, s->edge_3, ANN_ERROR, "Short pulse ignored");
                s->edge_2 = s->edge_4;
                continue;
            }
        }

        if (unknownTiming || strechedZero) {
            if (strechedZero) {
                snprintf(value_buf, sizeof(value_buf), "0 - (%.0fµs=%.0fµs+%.0fµs)", total, part1, part2);
            } else {
                snprintf(value_buf, sizeof(value_buf), "%.0fµs=%.0fµs+%.0fµs", total, part1, part2);
            }
            snprintf(value_long, sizeof(value_long), "%.0fµs=%.0fµs+%.0fµs", total, part1, part2);
            snprintf(value_short, sizeof(value_short), "%.0fµs", total);
        }

        /* Railcom cutout detection */
        if (454 - acc <= total && total <= 488 + 119 + 6 + acc) {
            if (output_1[0] == '\0')
                snprintf(output_1, sizeof(output_1), "Railcom cutout?");
            else {
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "Railcom cutout or %s", output_1);
                snprintf(output_1, sizeof(output_1), "%s", tmp);
            }
            railcomCutout = 1;
        }

        if (unknownTiming && !railcomCutout) {
            s->syncSignal = 1;
            s->decodedBytesCount = 0;
            dcc_setNextStatus(s, DCC_WAITINGFORPREAMBLE);
            DCC_PUT_SIGNAL(di, s, ANN_FRAME_OTHER, "Resynchronize (Wait for preamble)", "Resynchronize", "Resync.", "R");
            DCC_PUT_SIGNAL(di, s, ANN_ERROR, "unknown timing - should not occur - dirty signal?");
        } else if (output_1[0] != '\0') {
            DCC_PUT_SIGNAL(di, s, ANN_FRAME_OTHER, output_1);
        }

        /* Output bit annotations */
        if (s->syncSignal) {
            if (value && (strcmp(value, "0") == 0 || strcmp(value, "1") == 0)) {
                if (strechedZero) {
                    char sync_buf[160], sync_short[128];
                    snprintf(sync_buf, sizeof(sync_buf), "%s (sync in progress)", value_buf);
                    snprintf(sync_short, sizeof(sync_short), "%s (sync)", value_buf);
                    DCC_PUT_SIGNAL(di, s, ANN_BITS_OTHER, sync_buf, sync_short, value_buf);
                } else {
                    char sync_buf[64], sync_short[32];
                    snprintf(sync_buf, sizeof(sync_buf), "%s (sync in progress)", value);
                    snprintf(sync_short, sizeof(sync_short), "%s (sync)", value);
                    DCC_PUT_SIGNAL(di, s, ANN_BITS, sync_buf, sync_short, value);
                }
            } else {
                char sync_buf[160], sync_long[128];
                snprintf(sync_buf, sizeof(sync_buf), "%s (sync in progress)", value_buf);
                snprintf(sync_long, sizeof(sync_long), "%s (sync)", value_long);
                DCC_PUT_SIGNAL(di, s, ANN_BITS_OTHER, sync_buf, sync_long, value_short);
            }
        } else {
            if (value && (strcmp(value, "0") == 0 || strcmp(value, "1") == 0)) {
                if (strechedZero) {
                    DCC_PUT_SIGNAL(di, s, ANN_BITS_OTHER, value_buf);
                } else {
                    DCC_PUT_SIGNAL(di, s, ANN_BITS, value);
                }
            } else {
                DCC_PUT_SIGNAL(di, s, ANN_BITS_OTHER, value_buf, value_long, value_short);
            }
        }

        dcc_collectDataBytes(di, s, s->edge_1, s->edge_3, value ? value : value_buf);
        s->edge_1 = s->edge_3;
        s->edge_2 = s->edge_4;
    }
}

static void dcc_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder dcc_c_decoder = {
    .id = "dcc_c",
    .name = "DCC(C)",
    .longname = "Digital Command Control (C)",
    .desc = "DCC protocol (operate model railways digitally)",
    .license = "gplv2+",
    .channels = dcc_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = dcc_options,
    .num_options = 8,
    .num_annotations = NUM_ANN,
    .ann_labels = dcc_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = dcc_ann_rows,
    .inputs = dcc_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = dcc_tags,
    .num_tags = 1,
    .reset = dcc_reset,
    .start = dcc_start,
    .decode = dcc_decode,
    .destroy = dcc_destroy,
    .state_size = 0,
    .metadata = dcc_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &dcc_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}