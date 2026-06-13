/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2025 Petteri Aimonen <jpa@sigrok.mail.kapsi.fi>
 * Copyright (C) 2025 C port contributors
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
    ANN_TRACE = 0,
    ANN_TIMESTAMP,
    ANN_SOFTWARE,
    ANN_DWT_EVENT,
    ANN_DWT_WATCHPOINT,
    ANN_DWT_EXC,
    ANN_DWT_PC,
    ANN_MODE_THREAD,
    ANN_MODE_IRQ,
    ANN_MODE_EXC,
    ANN_LOCATION,
    ANN_FUNCTION,
    NUM_ANN,
};

enum arm_itm_state {
    ITM_IDLE,
    ITM_COLLECTING,
};

static const char *arm_exceptions[] = {
    "Thread", "Reset", "NMI", "HardFault", "MemManage", "BusFault",
    "UsageFault", NULL, NULL, NULL, NULL, "SVCall", "Debug Monitor",
    NULL, "PendSV", "SysTick"
};

typedef struct {
    int out_ann;
    int out_proto;
    enum arm_itm_state state;
    uint8_t buf[8];
    int buf_len;
    uint8_t syncbuf[6];
    int syncbuf_len;
    uint64_t ss;
    uint64_t es;
    uint64_t ss_packet;
    uint64_t prev_sample;
    uint64_t byte_len;
    uint64_t dwt_timestamp;
    int current_mode;
    uint64_t mode_ss;
    int mode_ann_idx;
} arm_itm_state;

static const char *arm_itm_inputs[] = {"uart", NULL};
static const char *arm_itm_outputs[] = {"arm_itm", NULL};
static const char *arm_itm_tags[] = {"Debug/trace", NULL};

static const char *arm_itm_ann_labels[][3] = {
    {"", "trace", "Trace information"},
    {"", "timestamp", "Timestamp"},
    {"", "software", "Software message"},
    {"", "dwt_event", "DWT event"},
    {"", "dwt_watchpoint", "DWT watchpoint"},
    {"", "dwt_exc", "Exception trace"},
    {"", "dwt_pc", "Program counter"},
    {"", "mode_thread", "Current mode: thread"},
    {"", "mode_irq", "Current mode: IRQ"},
    {"", "mode_exc", "Current mode: Exception"},
    {"", "location", "Current location"},
    {"", "function", "Current function"},
};

static const int arm_itm_row_trace_classes[] = {ANN_TRACE, ANN_TIMESTAMP, -1};
static const int arm_itm_row_software_classes[] = {ANN_SOFTWARE, -1};
static const int arm_itm_row_dwt_event_classes[] = {ANN_DWT_EVENT, -1};
static const int arm_itm_row_dwt_watchpoint_classes[] = {ANN_DWT_WATCHPOINT, -1};
static const int arm_itm_row_dwt_exc_classes[] = {ANN_DWT_EXC, -1};
static const int arm_itm_row_dwt_pc_classes[] = {ANN_DWT_PC, -1};
static const int arm_itm_row_mode_classes[] = {ANN_MODE_THREAD, ANN_MODE_IRQ, ANN_MODE_EXC, -1};
static const int arm_itm_row_location_classes[] = {ANN_LOCATION, -1};
static const int arm_itm_row_function_classes[] = {ANN_FUNCTION, -1};

static const struct srd_c_ann_row arm_itm_ann_rows[] = {
    {"trace", "Trace information", arm_itm_row_trace_classes, 2},
    {"software", "Software trace", arm_itm_row_software_classes, 1},
    {"dwt_event", "DWT event", arm_itm_row_dwt_event_classes, 1},
    {"dwt_watchpoint", "DWT watchpoint", arm_itm_row_dwt_watchpoint_classes, 1},
    {"dwt_exc", "Exception trace", arm_itm_row_dwt_exc_classes, 1},
    {"dwt_pc", "Program counter", arm_itm_row_dwt_pc_classes, 1},
    {"mode", "Current mode", arm_itm_row_mode_classes, 3},
    {"location", "Current location", arm_itm_row_location_classes, 1},
    {"function", "Current function", arm_itm_row_function_classes, 1},
};

static const char *get_packet_type(uint8_t byte)
{
    if ((byte & 0x7F) == 0)
        return "sync";
    if (byte == 0x70)
        return "overflow";
    if ((byte & 0x0F) == 0 && (byte & 0xF0) != 0)
        return "timestamp";
    if ((byte & 0x0F) == 0x08)
        return "sw_extension";
    if ((byte & 0x0F) == 0x0C)
        return "hw_extension";
    if ((byte & 0x0F) == 0x04)
        return "reserved";
    if ((byte & 0x04) == 0x00)
        return "software";
    return "hardware";
}

static int get_payload_len(uint8_t byte)
{
    return (int[]){0, 1, 2, 4}[byte & 0x03];
}

static void arm_itm_mode_change(struct srd_decoder_inst *di, arm_itm_state *s,
                                 const char *mode_str, int ann_idx)
{
    if (s->current_mode) {
        c_put(di, s->mode_ss, s->ss, s->out_ann, s->mode_ann_idx, mode_str);
    }
    if (mode_str) {
        s->current_mode = 1;
        s->mode_ss = s->ss;
        s->mode_ann_idx = ann_idx;
    } else {
        s->current_mode = 0;
    }
}

static const char *get_exception_name(int excnum)
{
    if (excnum >= 0 && excnum <= 15 && arm_exceptions[excnum])
        return arm_exceptions[excnum];
    static char buf[32];
    snprintf(buf, sizeof(buf), "IRQ %d", excnum - 16);
    return buf;
}

static void arm_itm_handle_packet(struct srd_decoder_inst *di, arm_itm_state *s)
{
    const char *ptype = get_packet_type(s->buf[0]);
    char t[256];

    if (strcmp(ptype, "overflow") == 0) {
        c_put(di, s->ss_packet, s->es, s->out_ann, ANN_TRACE, "Overflow");
    } else if (strcmp(ptype, "sync") == 0) {
        snprintf(t, sizeof(t), "Unhandled %s:", ptype);
        int pos = (int)strlen(t);
        for (int i = 0; i < s->buf_len && pos < (int)sizeof(t) - 5; i++)
            pos += snprintf(t + pos, sizeof(t) - pos, " %02x", s->buf[i]);
        c_put(di, s->ss_packet, s->es, s->out_ann, ANN_TRACE, t);
    } else if (strcmp(ptype, "timestamp") == 0) {
        if (s->buf[s->buf_len - 1] & 0x80)
            return; /* Not complete yet */
        int tc = 0;
        uint32_t ts = 0;
        if ((s->buf[0] & 0x80) == 0) {
            tc = 0;
            ts = s->buf[0] >> 4;
        } else {
            tc = (s->buf[0] & 0x30) >> 4;
            ts = s->buf[1] & 0x7F;
            if (s->buf_len > 2)
                ts |= (s->buf[2] & 0x7F) << 7;
            if (s->buf_len > 3)
                ts |= (s->buf[3] & 0x7F) << 14;
            if (s->buf_len > 4)
                ts |= (s->buf[4] & 0x7F) << 21;
        }
        s->dwt_timestamp += ts;
        const char *msg;
        if (tc == 0) msg = "(exact)";
        else if (tc == 1) msg = "(timestamp delayed)";
        else if (tc == 2) msg = "(event delayed)";
        else msg = "(event and timestamp delayed)";
        snprintf(t, sizeof(t), "Timestamp: %llu %s", (unsigned long long)s->dwt_timestamp, msg);
        c_put(di, s->ss_packet, s->es, s->out_ann, ANN_TIMESTAMP, t);
    } else if (strcmp(ptype, "software") == 0) {
        int plen = get_payload_len(s->buf[0]);
        int pid = s->buf[0] >> 3;
        if (s->buf_len != plen + 1)
            return;
        if (plen == 1) {
            snprintf(t, sizeof(t), "%d: 0x%02x", pid, s->buf[1]);
        } else if (plen == 2) {
            snprintf(t, sizeof(t), "%d: 0x%02x%02x", pid, s->buf[2], s->buf[1]);
        } else if (plen == 4) {
            snprintf(t, sizeof(t), "%d: 0x%02x%02x%02x%02x", pid,
                     s->buf[4], s->buf[3], s->buf[2], s->buf[1]);
        } else {
            snprintf(t, sizeof(t), "%d: (empty)", pid);
        }
        c_put(di, s->ss_packet, s->es, s->out_ann, ANN_SOFTWARE, t);
    } else if (strcmp(ptype, "hardware") == 0) {
        int plen = get_payload_len(s->buf[0]);
        int pid = s->buf[0] >> 3;
        if (s->buf_len != plen + 1)
            return;
        if (pid == 0 && plen >= 1) {
            /* DWT events */
            int pos = 0;
            pos += snprintf(t + pos, sizeof(t) - pos, "DWT events:");
            if (s->buf[1] & 0x20) pos += snprintf(t + pos, sizeof(t) - pos, " Cyc");
            if (s->buf[1] & 0x10) pos += snprintf(t + pos, sizeof(t) - pos, " Fold");
            if (s->buf[1] & 0x08) pos += snprintf(t + pos, sizeof(t) - pos, " LSU");
            if (s->buf[1] & 0x04) pos += snprintf(t + pos, sizeof(t) - pos, " Sleep");
            if (s->buf[1] & 0x02) pos += snprintf(t + pos, sizeof(t) - pos, " Exc");
            if (s->buf[1] & 0x01) pos += snprintf(t + pos, sizeof(t) - pos, " CPI");
            c_put(di, s->ss_packet, s->es, s->out_ann, ANN_DWT_EVENT, t);
        } else if (pid == 1 && plen >= 2) {
            /* Exception trace */
            int excnum = ((s->buf[2] & 1) << 8) | s->buf[1];
            int event = (s->buf[2] >> 4);
            const char *excstr = get_exception_name(excnum);
            if (event == 1) {
                arm_itm_mode_change(di, s, excstr, ANN_MODE_EXC);
                snprintf(t, sizeof(t), "Enter: %s", excstr);
            } else if (event == 2) {
                arm_itm_mode_change(di, s, NULL, 0);
                snprintf(t, sizeof(t), "Exit: %s", excstr);
            } else if (event == 3) {
                arm_itm_mode_change(di, s, excstr, ANN_MODE_EXC);
                snprintf(t, sizeof(t), "Resume: %s", excstr);
            } else {
                snprintf(t, sizeof(t), "Exception event %d: %s", event, excstr);
            }
            c_put(di, s->ss_packet, s->es, s->out_ann, ANN_DWT_EXC, t);
        } else if (pid == 2 && plen == 4) {
            /* Program counter */
            uint32_t pc = s->buf[1] | (s->buf[2] << 8) | (s->buf[3] << 16) | (s->buf[4] << 24);
            snprintf(t, sizeof(t), "PC: 0x%08x", pc);
            c_put(di, s->ss_packet, s->es, s->out_ann, ANN_DWT_PC, t);
        } else if ((s->buf[0] & 0xC4) == 0x84) {
            /* Data watchpoint */
            int comp = (s->buf[0] & 0x30) >> 4;
            const char *what = (s->buf[0] & 0x08) ? "Write" : "Read";
            if (plen == 1)
                snprintf(t, sizeof(t), "Watchpoint %d: %s data 0x%02x", comp, what, s->buf[1]);
            else if (plen == 2)
                snprintf(t, sizeof(t), "Watchpoint %d: %s data 0x%04x", comp, what,
                         s->buf[1] | (s->buf[2] << 8));
            else if (plen == 4)
                snprintf(t, sizeof(t), "Watchpoint %d: %s data 0x%08x", comp, what,
                         s->buf[1] | (s->buf[2] << 8) | (s->buf[3] << 16) | (s->buf[4] << 24));
            else
                snprintf(t, sizeof(t), "Watchpoint %d: %s", comp, what);
            c_put(di, s->ss_packet, s->es, s->out_ann, ANN_DWT_WATCHPOINT, t);
        } else {
            /* Unhandled hardware packet */
            snprintf(t, sizeof(t), "Unhandled %s:", ptype);
            int pos = (int)strlen(t);
            for (int i = 0; i < s->buf_len && pos < (int)sizeof(t) - 5; i++)
                pos += snprintf(t + pos, sizeof(t) - pos, " %02x", s->buf[i]);
            c_put(di, s->ss_packet, s->es, s->out_ann, ANN_TRACE, t);
        }
    } else {
        /* Unhandled: sw_extension, hw_extension, reserved */
        snprintf(t, sizeof(t), "Unhandled %s:", ptype);
        int pos = (int)strlen(t);
        for (int i = 0; i < s->buf_len && pos < (int)sizeof(t) - 5; i++)
            pos += snprintf(t + pos, sizeof(t) - pos, " %02x", s->buf[i]);
        c_put(di, s->ss_packet, s->es, s->out_ann, ANN_TRACE, t);
    }
}

static int arm_itm_packet_complete(arm_itm_state *s)
{
    const char *ptype = get_packet_type(s->buf[0]);

    if (strcmp(ptype, "sync") == 0)
        return 1;
    if (strcmp(ptype, "overflow") == 0)
        return 1;
    if (strcmp(ptype, "timestamp") == 0) {
        /* Timestamp packet ends when a byte without bit 7 set is seen */
        for (int i = 1; i < s->buf_len; i++) {
            if ((s->buf[i] & 0x80) == 0)
                return 1;
        }
        if (s->buf[0] & 0x80) {
            /* Multi-byte timestamp, check if last byte has bit7 clear */
            return (s->buf[s->buf_len - 1] & 0x80) == 0;
        }
        return 1; /* Single byte timestamp */
    }
    if (strcmp(ptype, "software") == 0) {
        int plen = get_payload_len(s->buf[0]);
        return s->buf_len >= plen + 1;
    }
    if (strcmp(ptype, "hardware") == 0) {
        int plen = get_payload_len(s->buf[0]);
        return s->buf_len >= plen + 1;
    }
    /* sw_extension, hw_extension, reserved: single byte */
    return 1;
}

static void arm_itm_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    arm_itm_state *s = (arm_itm_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;
    /* fields[1].u8 is rxtx (0=RX, 1=TX), not used for ITM */

    s->byte_len = end_sample - start_sample;

    /* Reset packet if there is a long pause between bytes */
    if (start_sample - s->prev_sample > 16 * s->byte_len)
        s->buf_len = 0;
    s->prev_sample = end_sample;

    /* Build up the current packet byte by byte */
    if (s->buf_len < (int)sizeof(s->buf))
        s->buf[s->buf_len++] = byte_val;

    /* Store the start time of the packet */
    if (s->buf_len == 1)
        s->ss_packet = start_sample;

    s->ss = start_sample;
    s->es = end_sample;

    /* Keep separate buffer for detection of sync packets */
    if (s->syncbuf_len < 6) {
        s->syncbuf[s->syncbuf_len++] = byte_val;
    } else {
        memmove(s->syncbuf, s->syncbuf + 1, 5);
        s->syncbuf[5] = byte_val;
    }

    /* Sync pattern: 0x00 0x00 0x00 0x00 0x00 0x80 */
    if (s->syncbuf_len >= 6 &&
        s->syncbuf[0] == 0x00 && s->syncbuf[1] == 0x00 &&
        s->syncbuf[2] == 0x00 && s->syncbuf[3] == 0x00 &&
        s->syncbuf[4] == 0x00 && s->syncbuf[5] == 0x80) {
        memcpy(s->buf, s->syncbuf + s->syncbuf_len - 6, 6);
        s->buf_len = 6;
        arm_itm_handle_packet(di, s);
        s->buf_len = 0;
        return;
    }

    /* Check if packet is complete */
    if (s->buf_len > 0 && arm_itm_packet_complete(s)) {
        arm_itm_handle_packet(di, s);
        s->buf_len = 0;
    }
}

static void arm_itm_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(arm_itm_state)));
    arm_itm_state *s = (arm_itm_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(arm_itm_state));
}

static void arm_itm_start(struct srd_decoder_inst *di)
{
    arm_itm_state *s = (arm_itm_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "arm_itm");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "arm_itm");
}

static void arm_itm_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void arm_itm_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder arm_itm_c_decoder = {
    .id = "arm_itm_c",
    .name = "ARM ITM(C)",
    .longname = "ARM Instrumentation Trace Macroblock (C)",
    .desc = "ARM Cortex-M / ARMv7m ITM trace protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = arm_itm_ann_labels,
    .num_annotation_rows = 9,
    .annotation_rows = arm_itm_ann_rows,
    .inputs = arm_itm_inputs,
    .num_inputs = 1,
    .outputs = arm_itm_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = arm_itm_tags,
    .num_tags = 1,
    .reset = arm_itm_reset,
    .start = arm_itm_start,
    .decode = arm_itm_decode,
    .destroy = arm_itm_destroy,
    .decode_upper = arm_itm_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &arm_itm_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}