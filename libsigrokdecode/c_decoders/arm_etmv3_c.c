/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2015 Petteri Aimonen <jpa@sigrok.mail.kapsi.fi>
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
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
    ANN_BRANCH,
    ANN_EXCEPTION,
    ANN_EXECUTION,
    ANN_DATA,
    ANN_PC,
    ANN_INSTR_E,
    ANN_INSTR_N,
    ANN_SOURCE,
    ANN_LOCATION,
    ANN_FUNCTION,
    NUM_ANN,
};

enum {
    CPU_ARM = 0,
    CPU_THUMB,
    CPU_JAZELLE,
};

typedef struct {
    uint8_t buf[64];
    int buf_len;
    uint8_t syncbuf[8];
    int syncbuf_len;
    uint64_t prevsample;
    uint64_t startsample;
    uint64_t byte_len;
    uint32_t last_branch;
    int cpu_state;
    uint32_t current_pc;

    /* Location/function tracking (simplified, no objdump) */
    uint64_t current_loc_ss;
    uint64_t current_loc_es;
    char current_loc[256];
    uint64_t current_func_ss;
    uint64_t current_func_es;
    char current_func[256];

    /* Branch encoding */
    int branch_enc_alt;  /* 1=alternative, 0=original */

    int out_ann;
    int out_proto;
} etmv3_priv;

static const char *exc_names[] = {
    "No exception", "IRQ1", "IRQ2", "IRQ3", "IRQ4", "IRQ5", "IRQ6", "IRQ7",
    "IRQ0", "UsageFault", "NMI", "SVC", "DebugMon", "MemManage", "PendSV",
    "SysTick", "Reserved", "Reset", "BusFault", "Reserved", "Reserved"
};
#define NUM_EXC_NAMES 21

static struct srd_decoder_option etmv3_options[] = {
    {"objdump",      NULL, "objdump path",      NULL, NULL},
    {"objdump_opts", NULL, "objdump options",    NULL, NULL},
    {"elffile",      NULL, ".elf path",          NULL, NULL},
    {"branch_enc",   NULL, "Branch encoding",    NULL, NULL},
};

static const char *etmv3_ann_labels[][3] = {
    {"", "trace",     "Trace info"},
    {"", "branch",    "Branches"},
    {"", "exception", "Exceptions"},
    {"", "execution", "Instruction execution"},
    {"", "data",      "Data access"},
    {"", "pc",        "Program counter"},
    {"", "instr_e",   "Executed instructions"},
    {"", "instr_n",   "Not executed instructions"},
    {"", "source",    "Source code"},
    {"", "location",  "Current location"},
    {"", "function",  "Current function"},
};

static const int etmv3_row_trace_classes[] = {ANN_TRACE, -1};
static const int etmv3_row_flow_classes[] = {ANN_BRANCH, ANN_EXCEPTION, ANN_EXECUTION, -1};
static const int etmv3_row_data_classes[] = {ANN_DATA, -1};
static const int etmv3_row_pc_classes[] = {ANN_PC, -1};
static const int etmv3_row_instruction_classes[] = {ANN_INSTR_E, ANN_INSTR_N, -1};
static const int etmv3_row_source_classes[] = {ANN_SOURCE, -1};
static const int etmv3_row_location_classes[] = {ANN_LOCATION, -1};
static const int etmv3_row_function_classes[] = {ANN_FUNCTION, -1};

static const struct srd_c_ann_row etmv3_ann_rows[] = {
    {"trace",      "Trace info",        etmv3_row_trace_classes,      1},
    {"flow",       "Code flow",         etmv3_row_flow_classes,       3},
    {"data",       "Data access",       etmv3_row_data_classes,       1},
    {"pc",         "Program counter",   etmv3_row_pc_classes,         1},
    {"instruction","Instructions",      etmv3_row_instruction_classes, 2},
    {"source",     "Source code",       etmv3_row_source_classes,     1},
    {"location",   "Current location",  etmv3_row_location_classes,   1},
    {"function",   "Current function",  etmv3_row_function_classes,   1},
};

static const char *etmv3_inputs[] = {"uart"};
static const char *etmv3_outputs[] = {"arm_etmv3"};
static const char *etmv3_tags[] = {"Debug/trace"};

static int parse_varint(const uint8_t *bytes, int len, uint32_t *value, int *parsed_len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++) {
        v |= (uint32_t)(bytes[i] & 0x7F) << (i * 7);
        if ((bytes[i] & 0x80) == 0) {
            *value = v;
            *parsed_len = i + 1;
            return 0;
        }
    }
    *value = v;
    *parsed_len = len;
    return -1;  /* Not complete */
}

static uint32_t parse_uint(const uint8_t *bytes, int len)
{
    uint32_t v = 0;
    for (int i = 0; i < len; i++)
        v |= (uint32_t)bytes[i] << (i * 8);
    return v;
}

static const char *get_packet_type(uint8_t byte)
{
    if (byte & 0x01) return "branch";
    if (byte == 0x00) return "a_sync";
    if (byte == 0x04) return "cyclecount";
    if (byte == 0x08) return "i_sync";
    if (byte == 0x0C) return "trigger";
    if ((byte & 0xF3) == 0x20 || (byte & 0xF3) == 0x40 || (byte & 0xF3) == 0x60)
        return "ooo_data";
    if (byte == 0x50) return "store_failed";
    if (byte == 0x70) return "i_sync";
    if ((byte & 0xDF) == 0x54 || (byte & 0xDF) == 0x58 || (byte & 0xDF) == 0x5C)
        return "ooo_place";
    if (byte == 0x3C) return "vmid";
    if ((byte & 0xD3) == 0x02) return "data";
    if ((byte & 0xFB) == 0x42) return "timestamp";
    if (byte == 0x62) return "data_suppressed";
    if (byte == 0x66) return "ignore";
    if ((byte & 0xEF) == 0x6A) return "value_not_traced";
    if (byte == 0x6E) return "context_id";
    if (byte == 0x76) return "exception_exit";
    if (byte == 0x7E) return "exception_entry";
    if ((byte & 0x81) == 0x80) return "p_header";
    return "unknown";
}

static void put_ann(struct srd_decoder_inst *di, etmv3_priv *s,
                    uint64_t ss, uint64_t es, int ann_idx, const char **txts)
{
    struct srd_c_annotation ann;
    ann.ann_class = ann_idx;
    ann.ann_type = 0;
    ann.ann_text = (char **)txts;
    c_decoder_put(di, ss, es, s->out_ann, &ann);
}

static void handle_a_sync(struct srd_decoder_inst *di, etmv3_priv *s,
                          uint64_t ss, uint64_t es)
{
    if (s->buf[s->buf_len - 1] == 0x80) {
        const char *txts[] = {"Synchronization", NULL};
        put_ann(di, s, ss, es, ANN_TRACE, txts);
        s->buf_len = 0;
    }
}

static void handle_exception_exit(struct srd_decoder_inst *di, etmv3_priv *s,
                                  uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Exception exit", NULL};
    put_ann(di, s, ss, es, ANN_EXCEPTION, txts);
    s->buf_len = 0;
}

static void handle_exception_entry(struct srd_decoder_inst *di, etmv3_priv *s,
                                   uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Exception entry", NULL};
    put_ann(di, s, ss, es, ANN_EXCEPTION, txts);
    s->buf_len = 0;
}

static void handle_i_sync(struct srd_decoder_inst *di, etmv3_priv *s,
                          uint64_t ss, uint64_t es)
{
    int contextid_bytes = 0;

    if (s->buf_len < 6)
        return;  /* Not complete */

    uint32_t cyclecount = 0;
    int has_cyclecount = 0;
    int idx;

    if (s->buf[0] == 0x08) {
        has_cyclecount = 0;
        idx = 1 + contextid_bytes;
    } else if (s->buf[0] == 0x70) {
        has_cyclecount = 1;
        int cyclen;
        parse_varint(&s->buf[1], s->buf_len - 1, &cyclecount, &cyclen);
        idx = 1 + cyclen + contextid_bytes;
    } else {
        return;
    }

    if (s->buf_len <= idx + 4)
        return;

    uint8_t infobyte = s->buf[idx];
    uint32_t addr = parse_uint(&s->buf[idx + 1], 4);

    int reasoncode = (infobyte >> 5) & 3;
    const char *reasons[] = {"Periodic", "Tracing enabled", "After overflow", "Exit from debug"};
    const char *reason = reasons[reasoncode];

    int thumb = addr & 1;
    addr &= 0xFFFFFFFE;

    s->last_branch = addr;
    s->current_pc = addr;

    if (thumb)
        s->cpu_state = CPU_THUMB;
    else
        s->cpu_state = CPU_ARM;

    const char *state_str = (s->cpu_state == CPU_THUMB) ? "thumb" : "arm";

    char t1[128], t2[64];
    if (has_cyclecount) {
        snprintf(t1, sizeof(t1), "I-Sync: %s, PC 0x%08x, %s state, cyclecount %u",
                 reason, addr, state_str, cyclecount);
    } else {
        snprintf(t1, sizeof(t1), "I-Sync: %s, PC 0x%08x, %s state",
                 reason, addr, state_str);
    }
    snprintf(t2, sizeof(t2), "I-Sync: %s 0x%08x", reason, addr);
    const char *txts[] = {t1, t2, NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);

    s->buf_len = 0;
}

static void handle_trigger(struct srd_decoder_inst *di, etmv3_priv *s,
                           uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Trigger event", "Trigger", NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void handle_branch(struct srd_decoder_inst *di, etmv3_priv *s,
                          uint64_t ss, uint64_t es)
{
    /* Check if branch packet is complete */
    if (s->buf[s->buf_len - 1] & 0x80)
        return;  /* Not complete yet */

    uint32_t addr;
    int addrlen;
    parse_varint(s->buf, s->buf_len, &addr, &addrlen);

    int addr_bits = 7 * addrlen;
    int have_exc_info = 0;

    if (s->branch_enc_alt) {
        addr_bits -= 1;
        if (addrlen >= 2 && (addr & (1 << addr_bits))) {
            have_exc_info = 1;
            addr &= ~(1 << addr_bits);
        }
    } else {
        if (addrlen == 5 && (s->buf[4] & 0x40))
            have_exc_info = 1;
    }

    /* CPU state change detection */
    if (addrlen == 5) {
        if ((s->buf[4] & 0xB8) == 0x08)
            s->cpu_state = CPU_ARM;
        else if ((s->buf[4] & 0xB0) == 0x10)
            s->cpu_state = CPU_THUMB;
        else if ((s->buf[4] & 0xA0) == 0x20)
            s->cpu_state = CPU_JAZELLE;
    }

    /* Shift address based on CPU state */
    if (s->cpu_state == CPU_ARM) {
        addr = (addr & 0xFFFFFFFE) << 1;
        addr_bits += 1;
    } else if (s->cpu_state == CPU_THUMB) {
        addr = addr & 0xFFFFFFFE;
    } else if (s->cpu_state == CPU_JAZELLE) {
        addr = (addr & 0xFFFFFFFE) >> 1;
        addr_bits -= 1;
    }

    /* Fill in from previous address if not full */
    if (addrlen < 5) {
        addr |= s->last_branch & (0xFFFFFFFF << addr_bits);
    }

    s->last_branch = addr;
    s->current_pc = addr;

    char txt[256] = {0};
    const char *state_strs[] = {"arm", "thumb", "jazelle"};
    (void)state_strs;
    int ann_idx = ANN_BRANCH;

    /* Exception info (simplified) */
    if (have_exc_info) {
        ann_idx = ANN_EXCEPTION;
        /* Parse exception info if available */
        if (addrlen < s->buf_len) {
            uint32_t excv;
            int exclen;
            parse_varint(&s->buf[addrlen], s->buf_len - addrlen, &excv, &exclen);

            int exc = ((excv >> 1) & 0x0F) | ((excv >> 7) & 0x1F0);
            if (exc > 0) {
                if (exc < NUM_EXC_NAMES)
                    snprintf(txt + strlen(txt), sizeof(txt) - strlen(txt),
                             ", exception %s", exc_names[exc]);
                else
                    snprintf(txt + strlen(txt), sizeof(txt) - strlen(txt),
                             ", exception 0x%02x", exc);
            }
        }
    }

    char t1[256], t2[128];
    snprintf(t1, sizeof(t1), "Branch to 0x%08x%s", addr, txt);
    snprintf(t2, sizeof(t2), "B 0x%08x%s", addr, txt);
    const char *txts[] = {t1, t2, NULL};
    put_ann(di, s, ss, es, ann_idx, txts);

    s->buf_len = 0;
}

static void handle_p_header(struct srd_decoder_inst *di, etmv3_priv *s,
                            uint64_t ss, uint64_t es)
{
    if ((s->buf[0] & 0x83) == 0x80) {
        int n = (s->buf[0] >> 6) & 1;
        int e = (s->buf[0] >> 2) & 15;

        /* Output PC annotations for executed instructions */
        uint64_t tdelta = 1;
        if (e + n > 0)
            tdelta = (es - ss) / (e + n);

        for (int i = 0; i < e; i++) {
            uint64_t iss = ss + tdelta * i;
            uint64_t ies = ss + tdelta * (i + 1);
            char pc_str[64], pc_short[32], pc_tiny[16];
            snprintf(pc_str, sizeof(pc_str), "PC 0x%08x", s->current_pc);
            snprintf(pc_short, sizeof(pc_short), "0x%08x", s->current_pc);
            snprintf(pc_tiny, sizeof(pc_tiny), "%08x", s->current_pc);
            const char *pc_txts[] = {pc_str, pc_short, pc_tiny, NULL};
            put_ann(di, s, iss, ies, ANN_PC, pc_txts);

            /* Advance PC */
            if (s->cpu_state == CPU_THUMB)
                s->current_pc += 2;
            else
                s->current_pc += 4;
        }

        for (int i = 0; i < n; i++) {
            /* Not executed: still advance PC */
            if (s->cpu_state == CPU_THUMB)
                s->current_pc += 2;
            else
                s->current_pc += 4;
        }

        char t1[128], t2[64], t3[16];
        if (n) {
            snprintf(t1, sizeof(t1), "%d instructions executed, %d skipped due to condition codes", e, n);
            snprintf(t2, sizeof(t2), "%d ins exec, %d skipped", e, n);
            snprintf(t3, sizeof(t3), "%dE,%dN", e, n);
        } else {
            snprintf(t1, sizeof(t1), "%d instructions executed", e);
            snprintf(t2, sizeof(t2), "%d ins exec", e);
            snprintf(t3, sizeof(t3), "%dE", e);
        }
        const char *txts[] = {t1, t2, t3, NULL};
        put_ann(di, s, ss, es, ANN_EXECUTION, txts);
    } else if ((s->buf[0] & 0xF3) == 0x82) {
        int i1 = (s->buf[0] >> 3) & 1;
        int i2 = (s->buf[0] >> 2) & 1;

        /* Two-instruction packet */
        const char *txt_exec[] = {"executed", "skipped"};
        const char *txt_short[] = {"E", "S"};
        char t1[128], t2[64], t3[16];
        snprintf(t1, sizeof(t1), "Instruction 1 %s, instruction 2 %s",
                 txt_exec[i1], txt_exec[i2]);
        snprintf(t2, sizeof(t2), "I1 %s, I2 %s", txt_short[i1], txt_short[i2]);
        snprintf(t3, sizeof(t3), "%s,%s", txt_short[i1], txt_short[i2]);
        const char *txts[] = {t1, t2, t3, NULL};
        put_ann(di, s, ss, es, ANN_EXECUTION, txts);

        /* Advance PC */
        if (!i1) {
            if (s->cpu_state == CPU_THUMB) s->current_pc += 2;
            else s->current_pc += 4;
        }
        if (!i2) {
            if (s->cpu_state == CPU_THUMB) s->current_pc += 2;
            else s->current_pc += 4;
        }
    }

    s->buf_len = 0;
}

static void handle_ignore(struct srd_decoder_inst *di, etmv3_priv *s,
                          uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Ignore", NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void handle_data_suppressed(struct srd_decoder_inst *di, etmv3_priv *s,
                                   uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Data suppressed", NULL};
    put_ann(di, s, ss, es, ANN_DATA, txts);
    s->buf_len = 0;
}

static void handle_store_failed(struct srd_decoder_inst *di, etmv3_priv *s,
                                uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Store failed", NULL};
    put_ann(di, s, ss, es, ANN_DATA, txts);
    s->buf_len = 0;
}

static void handle_context_id(struct srd_decoder_inst *di, etmv3_priv *s,
                              uint64_t ss, uint64_t es)
{
    char t1[64];
    uint32_t cid = parse_uint(&s->buf[1], s->buf_len - 1);
    snprintf(t1, sizeof(t1), "Context ID: 0x%08x", cid);
    const char *txts[] = {t1, NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void handle_vmid(struct srd_decoder_inst *di, etmv3_priv *s,
                        uint64_t ss, uint64_t es)
{
    char t1[64];
    uint32_t vmid = parse_uint(&s->buf[1], s->buf_len - 1);
    snprintf(t1, sizeof(t1), "VMID: 0x%08x", vmid);
    const char *txts[] = {t1, NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void handle_timestamp(struct srd_decoder_inst *di, etmv3_priv *s,
                             uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Timestamp", NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void handle_cyclecount(struct srd_decoder_inst *di, etmv3_priv *s,
                              uint64_t ss, uint64_t es)
{
    uint32_t count;
    int clen;
    parse_varint(&s->buf[1], s->buf_len - 1, &count, &clen);
    char t1[64];
    snprintf(t1, sizeof(t1), "Cycle count: %u", count);
    const char *txts[] = {t1, NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void handle_value_not_traced(struct srd_decoder_inst *di, etmv3_priv *s,
                                    uint64_t ss, uint64_t es)
{
    const char *txts[] = {"Value not traced", NULL};
    put_ann(di, s, ss, es, ANN_DATA, txts);
    s->buf_len = 0;
}

static void handle_fallback(struct srd_decoder_inst *di, etmv3_priv *s,
                            uint64_t ss, uint64_t es)
{
    const char *ptype = get_packet_type(s->buf[0]);
    char t1[256];
    int pos = snprintf(t1, sizeof(t1), "Unhandled %s:", ptype);
    for (int i = 0; i < s->buf_len && pos < (int)sizeof(t1) - 8; i++)
        pos += snprintf(t1 + pos, sizeof(t1) - pos, " %02x", s->buf[i]);
    const char *txts[] = {t1, NULL};
    put_ann(di, s, ss, es, ANN_TRACE, txts);
    s->buf_len = 0;
}

static void process_buffer(struct srd_decoder_inst *di, etmv3_priv *s,
                           uint64_t ss, uint64_t es)
{
    if (s->buf_len == 0)
        return;

    const char *ptype = get_packet_type(s->buf[0]);

    if (strcmp(ptype, "a_sync") == 0) {
        handle_a_sync(di, s, ss, es);
    } else if (strcmp(ptype, "exception_exit") == 0) {
        handle_exception_exit(di, s, ss, es);
    } else if (strcmp(ptype, "exception_entry") == 0) {
        handle_exception_entry(di, s, ss, es);
    } else if (strcmp(ptype, "trigger") == 0) {
        handle_trigger(di, s, ss, es);
    } else if (strcmp(ptype, "ignore") == 0) {
        handle_ignore(di, s, ss, es);
    } else if (strcmp(ptype, "data_suppressed") == 0) {
        handle_data_suppressed(di, s, ss, es);
    } else if (strcmp(ptype, "store_failed") == 0) {
        handle_store_failed(di, s, ss, es);
    } else if (strcmp(ptype, "p_header") == 0) {
        handle_p_header(di, s, ss, es);
    } else if (strcmp(ptype, "branch") == 0) {
        handle_branch(di, s, ss, es);
    } else if (strcmp(ptype, "i_sync") == 0) {
        handle_i_sync(di, s, ss, es);
    } else if (strcmp(ptype, "context_id") == 0) {
        handle_context_id(di, s, ss, es);
    } else if (strcmp(ptype, "vmid") == 0) {
        handle_vmid(di, s, ss, es);
    } else if (strcmp(ptype, "timestamp") == 0) {
        handle_timestamp(di, s, ss, es);
    } else if (strcmp(ptype, "cyclecount") == 0) {
        handle_cyclecount(di, s, ss, es);
    } else if (strcmp(ptype, "value_not_traced") == 0) {
        handle_value_not_traced(di, s, ss, es);
    } else {
        handle_fallback(di, s, ss, es);
    }
}

static void arm_etmv3_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    etmv3_priv *s = (etmv3_priv *)c_decoder_get_private(di);
    if (!s)
        return;

    /* Only process DATA type from uart */
    if (strcmp(cmd, "DATA") != 0 || n_fields < 2)
        return;

    uint8_t byte_val = fields[0].u8;  /* byte value */
    /* fields[1].u8 = rxtx (0=RX, 1=TX), not used here */

    /* Reset packet if there is a long pause between bytes */
    s->byte_len = end_sample - start_sample;
    if (start_sample - s->prevsample > 16 * s->byte_len && s->prevsample != 0) {
        s->buf_len = 0;
    }
    s->prevsample = end_sample;

    /* Add byte to buffer */
    if (s->buf_len < 64)
        s->buf[s->buf_len++] = byte_val;

    /* Store start time of packet */
    if (s->buf_len == 1)
        s->startsample = start_sample;

    /* Keep separate buffer for sync detection */
    if (s->syncbuf_len < 5) {
        s->syncbuf[s->syncbuf_len++] = byte_val;
    } else {
        for (int i = 0; i < 4; i++)
            s->syncbuf[i] = s->syncbuf[i + 1];
        s->syncbuf[4] = byte_val;
    }

    /* Check for sync pattern: 0x00, 0x00, 0x00, 0x00, 0x80 */
    if (s->syncbuf_len >= 5 &&
        s->syncbuf[0] == 0x00 && s->syncbuf[1] == 0x00 &&
        s->syncbuf[2] == 0x00 && s->syncbuf[3] == 0x00 &&
        s->syncbuf[4] == 0x80) {
        /* Override buffer with sync pattern */
        s->buf[0] = 0x00; s->buf[1] = 0x00;
        s->buf[2] = 0x00; s->buf[3] = 0x00; s->buf[4] = 0x80;
        s->buf_len = 5;
    }

    /* Try to process the buffer */
    process_buffer(di, s, s->startsample, end_sample);
}

static void arm_etmv3_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(etmv3_priv)));
    }
    etmv3_priv *s = (etmv3_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(etmv3_priv));
    s->out_ann = -1;
    s->out_proto = -1;
    s->cpu_state = CPU_ARM;
}

static void arm_etmv3_start(struct srd_decoder_inst *di)
{
    etmv3_priv *s = (etmv3_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "arm_etmv3");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "arm_etmv3");

    const char *branch_enc = c_opt_str(di, "branch_enc", "alternative");
    s->branch_enc_alt = (strcmp(branch_enc, "alternative") == 0) ? 1 : 0;
}

static void arm_etmv3_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void arm_etmv3_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder arm_etmv3_c_decoder = {
    .id = "arm_etmv3_c",
    .name = "ARM ETMv3(C)",
    .longname = "ARM Embedded Trace Macroblock v3(C)",
    .desc = "ARM ETM v3 instruction trace protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = etmv3_options,
    .num_options = 4,
    .num_annotations = NUM_ANN,
    .ann_labels = etmv3_ann_labels,
    .num_annotation_rows = 8,
    .annotation_rows = etmv3_ann_rows,
    .inputs = etmv3_inputs,
    .num_inputs = 1,
    .outputs = etmv3_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = etmv3_tags,
    .num_tags = 1,
    .reset = arm_etmv3_reset,
    .start = arm_etmv3_start,
    .decode = arm_etmv3_decode,
    .destroy = arm_etmv3_destroy,
    .decode_upper = arm_etmv3_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    etmv3_options[0].def = g_variant_new_string("arm-none-eabi-objdump");
    etmv3_options[1].def = g_variant_new_string("-lSC");
    etmv3_options[2].def = g_variant_new_string("");

    etmv3_options[3].def = g_variant_new_string("alternative");
    GSList *enc_vals = NULL;
    enc_vals = g_slist_append(enc_vals, g_variant_new_string("alternative"));
    enc_vals = g_slist_append(enc_vals, g_variant_new_string("original"));
    etmv3_options[3].values = enc_vals;

    return &arm_etmv3_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}