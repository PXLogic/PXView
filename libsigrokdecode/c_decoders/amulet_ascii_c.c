/*
 * Copyright (C) 2019 Vesa-Pekka Palmu <vpalmu@depili.fi>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* Command annotations (0..40) */
enum {
    ANN_PAGE = 0,
    ANN_GBV,
    ANN_GWV,
    ANN_GSV,
    ANN_GLV,
    ANN_GRPC,
    ANN_SBV,
    ANN_SWV,
    ANN_SSV,
    ANN_RPC,
    ANN_LINE,
    ANN_RECT,
    ANN_FRECT,
    ANN_PIXEL,
    ANN_GBVA,
    ANN_GWVA,
    ANN_SBVA,
    ANN_GBVR,
    ANN_GWVR,
    ANN_GSVR,
    ANN_GLVR,
    ANN_GRPCR,
    ANN_SBVR,
    ANN_SWVR,
    ANN_SSVR,
    ANN_RPCR,
    ANN_LINER,
    ANN_RECTR,
    ANN_FRECTR,
    ANN_PIXELR,
    ANN_GBVAR,
    ANN_GWVAR,
    ANN_SBVAR,
    ANN_ACK,
    ANN_NACK,
    ANN_SWVA,
    ANN_SWVAR,
    ANN_GCV,
    ANN_GCVR,
    ANN_SCV,
    ANN_SCVR,
    /* Generic annotations */
    ANN_BIT,
    ANN_FIELD,
    ANN_WARN,
    NUM_ANN,
};

#undef L
#define L 41  /* number of command annotations */

typedef struct {
    uint8_t state;           /* current command byte (0 = idle) */
    int cmdstate;            /* byte index within current command */
    uint64_t ss;
    uint64_t es;
    uint64_t ss_cmd;
    uint64_t es_cmd;
    uint64_t ss_field;
    uint64_t es_field;
    int addr;
    int value;
    char str_val[256];
    int checksum;
    uint8_t page[2];
    int flags;
    int coords[4];
    int ms_chan;             /* 0=RX, 1=TX */
    int sm_chan;             /* 0=RX, 1=TX */
    int out_ann;
} amulet_state;

static const char *amulet_inputs[] = {"uart", NULL};
static const char *amulet_tags[] = {"Display", NULL};

static struct srd_decoder_option amulet_options[] = {
    {"ms_chan", "dec_amulet_ascii_opt_ms_chan", "Master -> slave channel", NULL, NULL},
    {"sm_chan", "dec_amulet_ascii_opt_sm_chan", "Slave -> master channel", NULL, NULL},
};

static const char *amulet_ann_labels[][3] = {
    {"", "page", "Jump to page"},
    {"", "gbv", "Get byte variable"},
    {"", "gwv", "Get word variable"},
    {"", "gsv", "Get string variable"},
    {"", "glv", "Get label variable"},
    {"", "grpc", "Get RPC buffer"},
    {"", "sbv", "Set byte variable"},
    {"", "swv", "Set word variable"},
    {"", "ssv", "Set string variable"},
    {"", "rpc", "Invoke RPC"},
    {"", "line", "Draw line"},
    {"", "rect", "Draw rectangle"},
    {"", "frect", "Draw filled rectangle"},
    {"", "pixel", "Draw pixel"},
    {"", "gbva", "Get byte variable array"},
    {"", "gwva", "Get word variable array"},
    {"", "sbva", "Set byte variable array"},
    {"", "gbvr", "Get byte variable reply"},
    {"", "gwvr", "Get word variable reply"},
    {"", "gsvr", "Get string variable reply"},
    {"", "glvr", "Get label variable reply"},
    {"", "grpcr", "Get RPC buffer reply"},
    {"", "sbvr", "Set byte variable reply"},
    {"", "swvr", "Set word variable reply"},
    {"", "ssvr", "Set string variable reply"},
    {"", "rpcr", "Invoke RPC reply"},
    {"", "liner", "Draw line reply"},
    {"", "rectr", "Draw rectangle reply"},
    {"", "frectr", "Draw filled rectangle reply"},
    {"", "pixelr", "Draw pixel reply"},
    {"", "gbvar", "Get byte variable array reply"},
    {"", "gwvar", "Get word variable array reply"},
    {"", "sbvar", "Set byte variable array reply"},
    {"", "ack", "Acknowledgment"},
    {"", "nack", "Negative acknowledgment"},
    {"", "swva", "Set word variable array"},
    {"", "swvar", "Set word variable array reply"},
    {"", "gcv", "Get color variable"},
    {"", "gcvr", "Get color variable reply"},
    {"", "scv", "Set color variable"},
    {"", "scvr", "Set color variable reply"},
    {"", "bit", "Bit"},
    {"", "field", "Field"},
    {"", "warning", "Warning"},
};

static const int amulet_row_bits_classes[] = {ANN_BIT};
static const int amulet_row_fields_classes[] = {ANN_FIELD};
static int amulet_row_commands_classes_arr[L];
static const int amulet_row_warnings_classes[] = {ANN_WARN};

static struct srd_c_ann_row amulet_ann_rows[4];

/* Commands with high bytes that should not abort current command */
static const uint8_t cmds_with_high_bytes[] = {
    0xA0,  /* PAGE */
    0xD7,  /* SSV */
    0xE7,  /* SSVR */
    0xE2,  /* GSVR */
    0xE3,  /* GLVR */
};

static int amulet_is_high_byte_cmd(uint8_t state)
{
    for (int i = 0; i < (int)sizeof(cmds_with_high_bytes); i++) {
        if (cmds_with_high_bytes[i] == state)
            return 1;
    }
    return 0;
}

/* Command code to annotation class mapping */
static int amulet_cmd_to_ann(uint8_t cmd)
{
    if (cmd == 0xA0) return ANN_PAGE;
    if (cmd >= 0xD0 && cmd <= 0xEF) return cmd - 0xD0 + ANN_GBV;
    if (cmd == 0xF0) return ANN_ACK;
    if (cmd == 0xF1) return ANN_NACK;
    if (cmd == 0xF2) return ANN_SWVA;
    if (cmd == 0xF3) return ANN_SWVAR;
    if (cmd == 0xF4) return ANN_GCV;
    if (cmd == 0xF5) return ANN_GCVR;
    if (cmd == 0xF6) return ANN_SCV;
    if (cmd == 0xF7) return ANN_SCVR;
    return ANN_WARN;
}

/* Command short name lookup */
static const char *amulet_cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case 0xA0: return "PAGE";
    case 0xD0: return "GBV";
    case 0xD1: return "GWV";
    case 0xD2: return "GSV";
    case 0xD3: return "GLV";
    case 0xD4: return "GRPC";
    case 0xD5: return "SBV";
    case 0xD6: return "SWV";
    case 0xD7: return "SSV";
    case 0xD8: return "RPC";
    case 0xD9: return "LINE";
    case 0xDA: return "RECT";
    case 0xDB: return "FRECT";
    case 0xDC: return "PIXEL";
    case 0xDD: return "GBVA";
    case 0xDE: return "GWVA";
    case 0xDF: return "SBVA";
    case 0xE0: return "GBVR";
    case 0xE1: return "GWVR";
    case 0xE2: return "GSVR";
    case 0xE3: return "GLVR";
    case 0xE4: return "GRPCR";
    case 0xE5: return "SBVR";
    case 0xE6: return "SWVR";
    case 0xE7: return "SSVR";
    case 0xE8: return "RPCR";
    case 0xE9: return "LINER";
    case 0xEA: return "RECTR";
    case 0xEB: return "FRECTR";
    case 0xEC: return "PIXELR";
    case 0xED: return "GBVAR";
    case 0xEE: return "GWVAR";
    case 0xEF: return "SBVAR";
    case 0xF0: return "ACK";
    case 0xF1: return "NACK";
    case 0xF2: return "SWVA";
    case 0xF3: return "SWVAR";
    case 0xF4: return "GCV";
    case 0xF5: return "GCVR";
    case 0xF6: return "SCV";
    case 0xF7: return "SCVR";
    default: return "UNKNOWN";
    }
}

static void amulet_emit_cmd_byte(struct srd_decoder_inst *di, amulet_state *s)
{
    s->ss_cmd = s->ss;
    const char *name = amulet_cmd_name(s->state);
    char buf[256];
    snprintf(buf, sizeof(buf), "Command: %s", name);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf, name);
}

static void amulet_emit_cmd_end(struct srd_decoder_inst *di, amulet_state *s, int ann_class)
{
    s->es_cmd = s->es;
    const char *name = amulet_cmd_name(s->state);
    char buf[256];
    snprintf(buf, sizeof(buf), "Command: %s", name);
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ann_class, buf, name);
    s->state = 0;
}

static void amulet_emit_addr_bytes(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    if (s->cmdstate == 2) {
        s->ss_field = s->ss;
        s->addr = (pdata >= '0' && pdata <= '9') ? (pdata - '0') << 4 :
                  (pdata >= 'A' && pdata <= 'F') ? (pdata - 'A' + 10) << 4 :
                  (pdata >= 'a' && pdata <= 'f') ? (pdata - 'a' + 10) << 4 : 0;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr high 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    } else if (s->cmdstate == 3) {
        int lo = (pdata >= '0' && pdata <= '9') ? pdata - '0' :
                 (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                 (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0;
        s->addr += lo;
        s->es_field = s->es;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr low 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        snprintf(buf, sizeof(buf), "Addr: 0x%02X", s->addr);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
    }
}

static void amulet_handle_read(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    if (s->cmdstate == 1) {
        amulet_emit_cmd_byte(di, s);
        s->addr = 0;
    } else if (s->cmdstate == 2 || s->cmdstate == 3) {
        amulet_emit_addr_bytes(di, s, pdata);
    }
    s->cmdstate++;
}

static void amulet_handle_set_common(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    if (s->cmdstate == 1) {
        s->addr = 0;
    }
    amulet_emit_addr_bytes(di, s, pdata);
}

static void amulet_handle_string(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata, int ann_class)
{
    amulet_handle_set_common(di, s, pdata);
    if (s->cmdstate == 4) {
        s->ss_field = s->ss;
        s->str_val[0] = '\0';
    }
    if (pdata == 0x00) {
        s->es_field = s->es;
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, "NULL");
        char buf[512];
        snprintf(buf, sizeof(buf), "Value: %s", s->str_val);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        amulet_emit_cmd_end(di, s, ann_class);
        return;
    }
    if (s->cmdstate > 3) {
        size_t len = strlen(s->str_val);
        if (len < sizeof(s->str_val) - 1) {
            s->str_val[len] = (char)pdata;
            s->str_val[len + 1] = '\0';
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    }
    s->cmdstate++;
}

static void amulet_handle_page(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    if (s->cmdstate == 2) {
        if (pdata == 0x02) {
            s->ss_field = s->ss_cmd;
            s->es_field = s->es;
            const char *name = "PAGE";
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, name);
            s->checksum = 0xA0 + 0x02;
        } else {
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Illegal second byte for page change", "Illegal byte");
            s->state = 0;
            return;
        }
    } else if (s->cmdstate == 3) {
        s->ss_field = s->ss;
        s->checksum += pdata;
        s->page[0] = pdata;
    } else if (s->cmdstate == 4) {
        s->checksum += pdata;
        s->page[1] = pdata;
        s->es_field = s->es;
        if (s->page[0] == 0xFF && s->page[1] == 0xFF) {
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_WARN, "Soft reset", "Reset");
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "Page: 0x%c%c", s->page[0], s->page[1]);
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        }
    } else if (s->cmdstate == 5) {
        s->checksum += pdata;
        if ((s->checksum & 0xFF) != 0) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Checksum error", "ERR");
        } else {
            c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "Checksum OK", "OK");
        }
        amulet_emit_cmd_end(di, s, ANN_PAGE);
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_grpc(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    if (s->cmdstate == 2) {
        s->ss_field = s->ss;
        s->flags = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                    (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                    (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
    } else if (s->cmdstate == 3) {
        s->flags += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                     (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                     (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
        s->es_field = s->es;
        char buf[32];
        snprintf(buf, sizeof(buf), "RPC flag: 0x%02X", s->flags);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        amulet_emit_cmd_end(di, s, ANN_GRPC);
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_sbv(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_handle_set_common(di, s, pdata);
    if (s->cmdstate == 4) {
        s->ss_field = s->ss;
        s->value = pdata;
    } else if (s->cmdstate == 5) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Value: 0x%c%c", (char)s->value, (char)pdata);
        s->es_field = s->es;
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        amulet_emit_cmd_end(di, s, ANN_SBV);
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_swv(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_handle_set_common(di, s, pdata);
    if (s->cmdstate > 3) {
        int nibble = s->cmdstate - 4;
        if (nibble == 0) {
            s->ss_field = s->ss;
            s->value = 0;
        }
        int d = (pdata >= '0' && pdata <= '9') ? pdata - '0' :
                (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0;
        s->value += d << (12 - 4 * nibble);
        if (nibble == 3) {
            s->es_field = s->es;
            char buf[32];
            snprintf(buf, sizeof(buf), "Value: 0x%04x", s->value);
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
            amulet_emit_cmd_end(di, s, ANN_SWV);
            return;
        }
    }
    s->cmdstate++;
}

static void amulet_handle_sbva(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    int nibble = (s->cmdstate - 3) % 2;
    if (s->cmdstate == 2) {
        s->addr = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                   (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                   (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
        s->ss_field = s->ss;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr high 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    } else if (s->cmdstate == 3) {
        s->addr += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                    (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                    (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
        s->es_field = s->ss;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr low 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        snprintf(buf, sizeof(buf), "Addr: 0x%02X", s->addr);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
    } else if (nibble == 0) {
        if (pdata == 0x00) {
            amulet_emit_cmd_end(di, s, ANN_SBVA);
            return;
        }
        s->value = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                    (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                    (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
        s->ss_field = s->ss;
    } else {
        s->value += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                     (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                     (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
        s->es_field = s->es;
        char buf[32];
        snprintf(buf, sizeof(buf), "Value 0x%02X", s->value);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
    }
    s->cmdstate++;
}

static void amulet_handle_swva(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    int nibble = (s->cmdstate - 3) % 4;
    if (s->cmdstate == 2) {
        s->addr = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                   (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                   (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
        s->ss_field = s->ss;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr high 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    } else if (s->cmdstate == 3) {
        s->addr += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                    (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                    (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
        s->es_field = s->ss;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr low 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        snprintf(buf, sizeof(buf), "Addr: 0x%02X", s->addr);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        s->value = 0;
    } else {
        int d = (pdata >= '0' && pdata <= '9') ? pdata - '0' :
                (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0;
        s->value += d << (12 - 4 * nibble);
        if (nibble == 0) {
            if (pdata == 0x00) {
                amulet_emit_cmd_end(di, s, ANN_SWVA);
                return;
            }
            s->ss_field = s->ss;
        }
        if (nibble == 3) {
            s->es_field = s->es;
            char buf[32];
            snprintf(buf, sizeof(buf), "Value 0x%04X", s->value);
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        }
    }
    s->cmdstate++;
}

static void amulet_handle_drawing(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata, int ann_class)
{
    if (s->cmdstate == 1) {
        s->coords[0] = 0;
        s->coords[1] = 0;
        s->coords[2] = 0;
        s->coords[3] = 0;
    }
    if (s->cmdstate < 18) {
        int nibble = (s->cmdstate - 1) % 4;
        int i = (s->cmdstate - 1) / 4;
        int d = (pdata >= '0' && pdata <= '9') ? pdata - '0' :
                (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0;
        if (i < 4)
            s->coords[i] += d << (12 - 4 * nibble);
        if (nibble == 0)
            s->ss_field = s->ss;
        else if (nibble == 3) {
            s->es_field = s->es;
            if (i < 4) {
                char buf[32];
                snprintf(buf, sizeof(buf), "Coordinate 0x%04X", s->coords[i]);
                c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
            }
        }
    }
    if (s->cmdstate == 18) {
        s->es_cmd = s->es;
        const char *name = amulet_cmd_name(s->state);
        char buf[64];
        snprintf(buf, sizeof(buf), "Command: %s", name);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ann_class, buf, name);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN, "Pattern/Color not implemented");
        s->state = 0;
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_not_implemented(struct srd_decoder_inst *di, amulet_state *s, int ann_class)
{
    s->es_cmd = s->es;
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN, "Command not decoded", "Not decoded");
    amulet_emit_cmd_end(di, s, ann_class);
}

static void amulet_handle_gbvr(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_emit_addr_bytes(di, s, pdata);
    if (s->cmdstate == 4) {
        s->ss_field = s->ss;
        s->value = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                    (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                    (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
        char buf[32];
        snprintf(buf, sizeof(buf), "High nibble 0x%02X", s->value >> 4);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    } else if (s->cmdstate == 5) {
        s->value += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                     (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                     (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
        s->es_field = s->es;
        char buf[32];
        snprintf(buf, sizeof(buf), "Value: 0x%02X", s->value);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        amulet_emit_cmd_end(di, s, ANN_GBVR);
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_gwvr(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_emit_addr_bytes(di, s, pdata);
    if (s->cmdstate > 3) {
        int nibble = s->cmdstate - 3;
        if (nibble == 0) {
            s->value = 0;
            s->ss_field = s->ss;
        }
        int d = (pdata >= '0' && pdata <= '9') ? pdata - '0' :
                (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0;
        s->value += d << (12 - 4 * nibble);
        if (nibble == 3) {
            s->es_field = s->es;
            char buf[32];
            snprintf(buf, sizeof(buf), "Value: 0x%04x", s->value);
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
            amulet_emit_cmd_end(di, s, ANN_GWVR);
            return;
        }
    }
    s->cmdstate++;
}

static void amulet_handle_grpcr(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_emit_addr_bytes(di, s, pdata);
    if (s->cmdstate > 3) {
        int nibble = (s->cmdstate - 3) % 2;
        if (nibble == 0) {
            if (pdata == 0x00) {
                amulet_emit_cmd_end(di, s, ANN_GRPCR);
                return;
            }
            s->value = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                        (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                        (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
            s->ss_field = s->ss;
        }
        if (nibble == 1) {
            s->value += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                         (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                         (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
            s->es_field = s->es;
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%02X", s->value);
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        }
    }
    s->cmdstate++;
}

static void amulet_handle_sbvr(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_handle_set_common(di, s, pdata);
    if (s->cmdstate == 4) {
        s->ss_field = s->ss;
        s->value = pdata;
    } else if (s->cmdstate == 5) {
        s->es_field = s->es;
        char buf[32];
        snprintf(buf, sizeof(buf), "Value: 0x%c%c", (char)s->value, (char)pdata);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        amulet_emit_cmd_end(di, s, ANN_SBVR);
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_swvr(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    amulet_handle_set_common(di, s, pdata);
    if (s->cmdstate == 4) {
        s->ss_field = s->ss;
        s->value = (pdata - 0x30) << 4;
    } else if (s->cmdstate == 5) {
        s->value += (pdata - 0x30);
        s->value <<= 8;
    } else if (s->cmdstate == 6) {
        s->value += (pdata - 0x30) << 4;
    } else if (s->cmdstate == 7) {
        s->value += (pdata - 0x30);
        s->es_field = s->es;
        char buf[32];
        snprintf(buf, sizeof(buf), "Value: 0x%04x", s->value);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        amulet_emit_cmd_end(di, s, ANN_SWVR);
        return;
    }
    s->cmdstate++;
}

static void amulet_handle_array_reply(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata, int ann_class, int word_size)
{
    int nibble;
    if (word_size == 2)
        nibble = (s->cmdstate - 3) % 2;
    else
        nibble = (s->cmdstate - 3) % 4;

    if (s->cmdstate == 2) {
        s->addr = ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                   (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                   (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0) << 4;
        s->ss_field = s->ss;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr high 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    } else if (s->cmdstate == 3) {
        s->addr += ((pdata >= '0' && pdata <= '9') ? pdata - '0' :
                    (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                    (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0);
        s->es_field = s->ss;
        char buf[32];
        snprintf(buf, sizeof(buf), "Addr low 0x%c", pdata);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        snprintf(buf, sizeof(buf), "Addr: 0x%02X", s->addr);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        s->value = 0;
    } else {
        int d = (pdata >= '0' && pdata <= '9') ? pdata - '0' :
                (pdata >= 'A' && pdata <= 'F') ? pdata - 'A' + 10 :
                (pdata >= 'a' && pdata <= 'f') ? pdata - 'a' + 10 : 0;
        s->value += d << (word_size == 2 ? (4 - 4 * nibble) : (12 - 4 * nibble));
        if (nibble == 0) {
            if (pdata == 0x00) {
                amulet_emit_cmd_end(di, s, ann_class);
                return;
            }
            s->ss_field = s->ss;
        }
        int max_nibble = word_size == 2 ? 1 : 3;
        if (nibble == max_nibble) {
            s->es_field = s->es;
            char buf[32];
            if (word_size == 2)
                snprintf(buf, sizeof(buf), "Value 0x%02X", s->value);
            else
                snprintf(buf, sizeof(buf), "Value 0x%04X", s->value);
            c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, buf);
        }
    }
    s->cmdstate++;
}

static void amulet_handle_command(struct srd_decoder_inst *di, amulet_state *s, uint8_t pdata)
{
    switch (s->state) {
    case 0xA0: amulet_handle_page(di, s, pdata); break;
    case 0xD0: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_GBV); break;
    case 0xD1: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_GWV); break;
    case 0xD2: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_GSV); break;
    case 0xD3: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_GLV); break;
    case 0xD4: amulet_handle_grpc(di, s, pdata); break;
    case 0xD5: amulet_handle_sbv(di, s, pdata); break;
    case 0xD6: amulet_handle_swv(di, s, pdata); break;
    case 0xD7: amulet_handle_string(di, s, pdata, ANN_SSV); break;
    case 0xD8: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_RPC); break;
    case 0xD9: amulet_handle_drawing(di, s, pdata, ANN_LINE); break;
    case 0xDA: amulet_handle_drawing(di, s, pdata, ANN_RECT); break;
    case 0xDB: amulet_handle_drawing(di, s, pdata, ANN_FRECT); break;
    case 0xDC: /* PIXEL - undocumented */
        s->es_cmd = s->es;
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN, "Draw pixel documentation is missing", "Undocumented");
        s->state = 0;
        break;
    case 0xDD: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_GBVA); break;
    case 0xDE: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_GWVA); break;
    case 0xDF: amulet_handle_sbva(di, s, pdata); break;
    case 0xE0: amulet_handle_gbvr(di, s, pdata); break;
    case 0xE1: amulet_handle_gwvr(di, s, pdata); break;
    case 0xE2: amulet_handle_string(di, s, pdata, ANN_GSVR); break;
    case 0xE3: amulet_handle_string(di, s, pdata, ANN_GLVR); break;
    case 0xE4: amulet_handle_grpcr(di, s, pdata); break;
    case 0xE5: amulet_handle_sbvr(di, s, pdata); break;
    case 0xE6: amulet_handle_swvr(di, s, pdata); break;
    case 0xE7: amulet_handle_string(di, s, pdata, ANN_SSVR); break;
    case 0xE8: amulet_handle_read(di, s, pdata); amulet_emit_cmd_end(di, s, ANN_RPCR); break;
    case 0xE9: amulet_handle_drawing(di, s, pdata, ANN_LINER); break;
    case 0xEA: amulet_handle_drawing(di, s, pdata, ANN_RECTR); break;
    case 0xEB: amulet_handle_drawing(di, s, pdata, ANN_FRECTR); break;
    case 0xEC: /* PIXELR - undocumented */
        s->es_cmd = s->es;
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN, "Draw pixel documentation is missing", "Undocumented");
        s->state = 0;
        break;
    case 0xED: amulet_handle_array_reply(di, s, pdata, ANN_GBVAR, 2); break;
    case 0xEE: amulet_handle_array_reply(di, s, pdata, ANN_GWVAR, 4); break;
    case 0xEF: amulet_handle_array_reply(di, s, pdata, ANN_SBVAR, 2); break;
    case 0xF0: /* ACK */
        c_put(di, s->ss, s->es, s->out_ann, ANN_ACK, "ACK");
        s->state = 0;
        break;
    case 0xF1: /* NACK */
        c_put(di, s->ss, s->es, s->out_ann, ANN_NACK, "NACK");
        s->state = 0;
        break;
    case 0xF2: amulet_handle_swva(di, s, pdata); break;
    case 0xF3: amulet_handle_array_reply(di, s, pdata, ANN_SWVAR, 4); break;
    case 0xF4: /* GCV - not implemented */
    case 0xF5: /* GCVR - not implemented */
    case 0xF6: /* SCV - not implemented */
    case 0xF7: /* SCVR - not implemented */
        if (s->cmdstate == 8) {
            amulet_handle_not_implemented(di, s, amulet_cmd_to_ann(s->state));
        }
        s->cmdstate++;
        break;
    default:
        c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Unknown command");
        s->state = 0;
        break;
    }
}

static void amulet_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    amulet_state *s = (amulet_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;
    if (n_fields < 1)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t pdata = fields[0].u8;

    /* Check for command abort by high byte */
    int abort_current = (pdata >= 0xD0 && pdata <= 0xF7) &&
                        !amulet_is_high_byte_cmd(s->state) &&
                        s->state != 0;

    if (abort_current) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Command aborted by invalid byte", "Abort");
        s->state = pdata;
        amulet_emit_cmd_byte(di, s);
        s->cmdstate = 1;
    }

    if (s->state == 0) {
        /* Check if this is a known command byte */
        if (pdata == 0xA0 || (pdata >= 0xD0 && pdata <= 0xF7)) {
            s->state = pdata;
            amulet_emit_cmd_byte(di, s);
            s->cmdstate = 1;
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Unknown command: 0x%02X", pdata);
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, buf);
        }
        return;
    }

    amulet_handle_command(di, s, pdata);
}

static void amulet_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(amulet_state)));
    }
    amulet_state *s = (amulet_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(amulet_state));
    s->ms_chan = 0; /* RX */
    s->sm_chan = 1; /* TX */
}

static void amulet_start(struct srd_decoder_inst *di)
{
    amulet_state *s = (amulet_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "amulet_ascii");

    const char *ms = c_opt_str(di, "ms_chan", "RX");
    s->ms_chan = (ms && strcmp(ms, "TX") == 0) ? 1 : 0;
    const char *sm = c_opt_str(di, "sm_chan", "TX");
    s->sm_chan = (sm && strcmp(sm, "TX") == 0) ? 1 : 0;
}

static void amulet_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void amulet_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* Build the commands row classes array at runtime */
static int amulet_cmds_row_built = 0;

static void amulet_build_cmds_row(void)
{
    if (amulet_cmds_row_built) return;
    for (int i = 0; i < L; i++)
        amulet_row_commands_classes_arr[i] = i;
    amulet_cmds_row_built = 1;
}

struct srd_c_decoder amulet_ascii_c_decoder = {
    .id = "amulet_ascii_c",
    .name = "Amulet ASCII(C)",
    .longname = "Amulet LCD ASCII (C)",
    .desc = "Amulet Technologies LCD controller ASCII protocol. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = amulet_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = amulet_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = amulet_ann_rows,
    .inputs = amulet_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = amulet_tags,
    .num_tags = 1,
    .reset = amulet_reset,
    .start = amulet_start,
    .decode = amulet_decode,
    .destroy = amulet_destroy,
    .decode_upper = amulet_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    amulet_options[0].def = g_variant_new_string("RX");
    GSList *ms_vals = NULL;
    ms_vals = g_slist_append(ms_vals, g_variant_new_string("RX"));
    ms_vals = g_slist_append(ms_vals, g_variant_new_string("TX"));
    amulet_options[0].values = ms_vals;

    amulet_options[1].def = g_variant_new_string("TX");
    GSList *sm_vals = NULL;
    sm_vals = g_slist_append(sm_vals, g_variant_new_string("RX"));
    sm_vals = g_slist_append(sm_vals, g_variant_new_string("TX"));
    amulet_options[1].values = sm_vals;

    /* Build the commands annotation row at runtime */
    amulet_build_cmds_row();

    amulet_ann_rows[0].id = "bits";
    amulet_ann_rows[0].desc = "Bits";
    amulet_ann_rows[0].ann_classes = amulet_row_bits_classes;
    amulet_ann_rows[0].num_ann_classes = 1;

    amulet_ann_rows[1].id = "fields";
    amulet_ann_rows[1].desc = "Fields";
    amulet_ann_rows[1].ann_classes = amulet_row_fields_classes;
    amulet_ann_rows[1].num_ann_classes = 1;

    amulet_ann_rows[2].id = "commands";
    amulet_ann_rows[2].desc = "Commands";
    amulet_ann_rows[2].ann_classes = amulet_row_commands_classes_arr;
    amulet_ann_rows[2].num_ann_classes = L;

    amulet_ann_rows[3].id = "warnings";
    amulet_ann_rows[3].desc = "Warnings";
    amulet_ann_rows[3].ann_classes = amulet_row_warnings_classes;
    amulet_ann_rows[3].num_ann_classes = 1;

    return &amulet_ascii_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}