#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* AVR JTAG instruction register values (4 bits) */
#define AVR_IR_IDCODE   0x03  /* 0011 */
#define AVR_IR_PDICOM   0x07  /* 0111 */
#define AVR_IR_BYPASS   0x0F  /* 1111 */

/* AVR IDCODE manufacturer (JEDEC) */
#define AVR_JEDEC_ATMEL 0x1F

/* AVR device ID codes */
static const struct {
    uint16_t part;
    const char *name;
} avr_idcode_map[] = {
    { 0x9642, "ATXMega64A3U" },
    { 0x9742, "ATXMega128A3U" },
    { 0x9744, "ATXMega192A3U" },
    { 0x9842, "ATXMega256A3U" },
    { 0, NULL },
};

/* PDI opcodes */
#define PDI_OP_LDS    0
#define PDI_OP_LD     1
#define PDI_OP_STS    2
#define PDI_OP_ST     3
#define PDI_OP_LDCS   4
#define PDI_OP_STCS   5
#define PDI_OP_REPEAT 6
#define PDI_OP_KEY    7

enum {
    ANN_ITEM = 0,
    ANN_FIELD,
    ANN_COMMAND,
    ANN_WARNING,
    ANN_DATA_IN,
    ANN_PARITY_IN_OK,
    ANN_PARITY_IN_ERR,
    ANN_DATA_OUT,
    ANN_PARITY_OUT_OK,
    ANN_PARITY_OUT_ERR,
    ANN_BREAK,
    ANN_OPCODE,
    ANN_DATA_PROG,
    ANN_DATA_DEV,
    ANN_PDI_BREAK,
    ANN_ENABLE,
    ANN_DISABLE,
    ANN_CMD_DATA,
    NUM_ANN,
};

enum avr_state {
    AVR_STATE_IDLE,
    AVR_STATE_BYPASS,
    AVR_STATE_IDCODE,
    AVR_STATE_PDICOM,
};

/* PDI control register names */

/* PDI pointer format names */

typedef struct {
    int out_ann;
    enum avr_state state;

    /* PDI decoder state */
    int pdi_opcode;
    int pdi_rep_count;
    int pdi_wr_counts[8];
    int pdi_rd_counts[8];
    int pdi_num_wr;
    int pdi_num_rd;
    int pdi_data_count;
    int pdi_data_bytes[8];
    int pdi_data_idx;
    uint64_t pdi_data_ss;
    uint64_t pdi_cmd_ss;

    /* Current bit data */
    uint64_t ss;
    uint64_t es;
} jtag_avr_state;

static const char *jtag_avr_inputs[] = {"jtag", NULL};
static const char *jtag_avr_tags[] = {"Debug/trace", NULL};

static const char *jtag_avr_ann_labels[][3] = {
    {"", "item", "Item"},
    {"", "field", "Field"},
    {"", "command", "Command"},
    {"", "warning", "Warning"},
    {"", "data-in", "PDI data in"},
    {"", "parity-in-ok", "Parity OK"},
    {"", "parity-in-err", "Parity error"},
    {"", "data-out", "PDI data out"},
    {"", "parity-out-ok", "Parity OK"},
    {"", "parity-out-err", "Parity error"},
    {"", "break", "BREAK condition"},
    {"", "opcode", "Instruction opcode"},
    {"", "data-prog", "Programmer data"},
    {"", "data-dev", "Device data"},
    {"", "pdi-break", "BREAK at PDI level"},
    {"", "enable", "Enable PDI"},
    {"", "disable", "Disable PDI"},
    {"", "cmd-data", "PDI command with data"},
};

static const int jtag_avr_row_items_classes[] = {ANN_ITEM, -1};
static const int jtag_avr_row_fields_classes[] = {ANN_FIELD, -1};
static const int jtag_avr_row_commands_classes[] = {ANN_COMMAND, -1};
static const int jtag_avr_row_warnings_classes[] = {ANN_WARNING, -1};
static const int jtag_avr_row_data_in_classes[] = {ANN_DATA_IN, ANN_PARITY_IN_OK, ANN_PARITY_IN_ERR, -1};
static const int jtag_avr_row_data_out_classes[] = {ANN_DATA_OUT, ANN_PARITY_OUT_OK, ANN_PARITY_OUT_ERR, -1};
static const int jtag_avr_row_break_classes[] = {ANN_BREAK, -1};
static const int jtag_avr_row_pdi_fields_classes[] = {ANN_PDI_BREAK, -1};
static const int jtag_avr_row_pdi_prog_classes[] = {ANN_OPCODE, ANN_DATA_PROG, -1};
static const int jtag_avr_row_pdi_dev_classes[] = {ANN_DATA_DEV, -1};
static const int jtag_avr_row_pdi_cmds_classes[] = {ANN_ENABLE, ANN_DISABLE, ANN_CMD_DATA, -1};

static const struct srd_c_ann_row jtag_avr_ann_rows[] = {
    {"items", "Items", jtag_avr_row_items_classes, 1},
    {"fields", "Fields", jtag_avr_row_fields_classes, 1},
    {"commands", "Commands", jtag_avr_row_commands_classes, 1},
    {"warnings", "Warnings", jtag_avr_row_warnings_classes, 1},
    {"data_in", "PDI Data (In)", jtag_avr_row_data_in_classes, 3},
    {"data_out", "PDI Data (Out)", jtag_avr_row_data_out_classes, 3},
    {"data_fields", "PDI Data Fields", jtag_avr_row_break_classes, 1},
    {"pdi_fields", "PDI Fields", jtag_avr_row_pdi_fields_classes, 1},
    {"pdi_prog", "PDI Programmer In", jtag_avr_row_pdi_prog_classes, 2},
    {"pdi_dev", "PDI Device Out", jtag_avr_row_pdi_dev_classes, 1},
    {"pdi_cmds", "PDI Commands", jtag_avr_row_pdi_cmds_classes, 3},
};

static const char *avr_find_part(uint16_t part)
{
    for (int i = 0; avr_idcode_map[i].name != NULL; i++) {
        if (avr_idcode_map[i].part == part)
            return avr_idcode_map[i].name;
    }
    return NULL;
}

static uint64_t bytes_to_uint64(const c_field *fields, int n_fields)
{
    uint64_t val = 0;
    for (uint64_t i = 0; i < (uint64_t)n_fields && i < 8; i++)
        val |= ((uint64_t)fields[i].u8) << (i * 8);
    return val;
}

static void jtag_avr_handle_idcode(struct srd_decoder_inst *di, jtag_avr_state *s,
                                    const c_field *fields, int n_fields)
{
    if (n_fields < 4)
        return;

    uint32_t idcode = (uint32_t)bytes_to_uint64(fields, n_fields);
    int version = (idcode >> 28) & 0x0F;
    uint16_t part = (idcode >> 12) & 0xFFFF;
    int manufacturer = (idcode >> 1) & 0x7FF;

    const char *manuf_str = (manufacturer == AVR_JEDEC_ATMEL) ? "Atmel" : "INVALID";
    const char *part_str = avr_find_part(part);
    char part_buf[16];
    if (!part_str) {
        snprintf(part_buf, sizeof(part_buf), "0x%04X", part);
        part_str = part_buf;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "Manufacturer: %s", manuf_str);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "Part: %s", part_str);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "Version: %d", version);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "IDCODE: 0x%08X (%s: %s@r%d)",
             idcode, manuf_str, part_str, version);
    c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
}

static void jtag_avr_handle_bypass(struct srd_decoder_inst *di, jtag_avr_state *s,
                                    const c_field *fields, int n_fields)
{
    char buf[32];
    uint64_t val = bytes_to_uint64(fields, n_fields);
    snprintf(buf, sizeof(buf), "BYPASS: %llu", (unsigned long long)val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_ITEM, buf);
}

/* PDI instruction handling */
static void pdi_clear_insn(jtag_avr_state *s)
{
    s->pdi_opcode = -1;
    s->pdi_data_count = 0;
    s->pdi_data_idx = 0;
    s->pdi_num_wr = 0;
    s->pdi_num_rd = 0;
}

static void pdi_setup_insn(jtag_avr_state *s, int opcode, int args)
{
    int sizeA = (args & 0x0C) >> 2;
    int sizeB = args & 0x03;
    int addr = args & 0x0F;
    (void)addr;
    int widthAddr = sizeA + 1;
    int widthData = sizeB + 1;

    s->pdi_opcode = opcode;
    s->pdi_data_idx = 0;

    switch (opcode) {
    case PDI_OP_LDS:
        s->pdi_wr_counts[0] = widthAddr;
        s->pdi_num_wr = 1;
        s->pdi_rd_counts[0] = widthData;
        s->pdi_num_rd = 1;
        s->pdi_data_count = widthAddr;
        break;
    case PDI_OP_LD:
        s->pdi_wr_counts[0] = 0;
        s->pdi_num_wr = 0;
        s->pdi_rd_counts[0] = widthData;
        s->pdi_num_rd = 1;
        if (s->pdi_rep_count > 0) {
            for (int i = 1; i <= s->pdi_rep_count && i < 7; i++)
                s->pdi_rd_counts[i] = widthData;
            s->pdi_num_rd = s->pdi_rep_count + 1;
            s->pdi_rep_count = 0;
        }
        s->pdi_data_count = 0;
        break;
    case PDI_OP_STS:
        s->pdi_wr_counts[0] = widthAddr;
        s->pdi_wr_counts[1] = widthData;
        s->pdi_num_wr = 2;
        s->pdi_num_rd = 0;
        s->pdi_data_count = widthAddr;
        break;
    case PDI_OP_ST:
        s->pdi_wr_counts[0] = widthData;
        s->pdi_num_wr = 1;
        s->pdi_num_rd = 0;
        if (s->pdi_rep_count > 0) {
            for (int i = 1; i <= s->pdi_rep_count && i < 7; i++)
                s->pdi_wr_counts[i] = widthData;
            s->pdi_num_wr = s->pdi_rep_count + 1;
            s->pdi_rep_count = 0;
        }
        s->pdi_data_count = widthData;
        break;
    case PDI_OP_LDCS:
        s->pdi_num_wr = 0;
        s->pdi_rd_counts[0] = 1;
        s->pdi_num_rd = 1;
        s->pdi_data_count = 0;
        break;
    case PDI_OP_STCS:
        s->pdi_wr_counts[0] = 1;
        s->pdi_num_wr = 1;
        s->pdi_num_rd = 0;
        s->pdi_data_count = 1;
        break;
    case PDI_OP_REPEAT:
        s->pdi_wr_counts[0] = widthData;
        s->pdi_num_wr = 1;
        s->pdi_num_rd = 0;
        s->pdi_data_count = widthData;
        break;
    case PDI_OP_KEY:
        s->pdi_wr_counts[0] = 8;
        s->pdi_num_wr = 1;
        s->pdi_num_rd = 0;
        s->pdi_data_count = 8;
        break;
    default:
        s->pdi_data_count = 0;
        break;
    }
}

static const char *pdi_opcode_name(int opcode)
{
    switch (opcode) {
    case PDI_OP_LDS:    return "LDS";
    case PDI_OP_LD:     return "LD";
    case PDI_OP_STS:    return "STS";
    case PDI_OP_ST:     return "ST";
    case PDI_OP_LDCS:   return "LDCS";
    case PDI_OP_STCS:   return "STCS";
    case PDI_OP_REPEAT: return "REPEAT";
    case PDI_OP_KEY:    return "KEY";
    default:            return "???";
    }
}

static void pdi_handle_data_in(struct srd_decoder_inst *di, jtag_avr_state *s,
                                const c_field *fields, int n_fields)
{
    if (s->pdi_opcode < 0)
        return;

    if (s->pdi_data_count > 0 && s->pdi_data_idx < 8) {
        s->pdi_data_bytes[s->pdi_data_idx++] = (n_fields > 0) ? fields[0].u8 : 0;
        s->pdi_data_count--;
        if (s->pdi_data_count > 0)
            return;

        /* All data bytes received for this chunk */
        char hex_buf[32];
        int idx = s->pdi_data_idx;
        /* Build hex string (reversed for LSB-first) */
        uint64_t val = 0;
        for (int i = 0; i < idx && i < 8; i++)
            val |= ((uint64_t)s->pdi_data_bytes[i]) << (i * 8);
        snprintf(hex_buf, sizeof(hex_buf), "0x%llX", (unsigned long long)val);

        char buf[64];
        snprintf(buf, sizeof(buf), "Data: %s", hex_buf);
        c_put(di, s->ss, s->es, s->out_ann, ANN_DATA_PROG, buf);

        /* Check if we need more data */
        if (s->pdi_num_wr > 1) {
            /* Move to next write count */
            for (int i = 1; i < s->pdi_num_wr; i++)
                s->pdi_wr_counts[i - 1] = s->pdi_wr_counts[i];
            s->pdi_num_wr--;
            s->pdi_data_count = s->pdi_wr_counts[0];
            s->pdi_data_idx = 0;
            return;
        }

        /* Instruction complete */
        if (s->pdi_opcode == PDI_OP_REPEAT) {
            s->pdi_rep_count = (int)val;
        }

        char cmd_buf[64];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s", pdi_opcode_name(s->pdi_opcode));
        c_put(di, s->pdi_cmd_ss, s->es, s->out_ann, ANN_CMD_DATA, cmd_buf);

        int save_rep = s->pdi_rep_count;
        pdi_clear_insn(s);
        s->pdi_rep_count = save_rep;
    }
}

static void pdi_handle_data_out(struct srd_decoder_inst *di, jtag_avr_state *s,
                                 const c_field *fields, int n_fields)
{
    if (s->pdi_opcode < 0)
        return;

    if (s->pdi_data_count > 0 && s->pdi_data_idx < 8) {
        s->pdi_data_bytes[s->pdi_data_idx++] = (n_fields > 0) ? fields[0].u8 : 0;
        s->pdi_data_count--;
        if (s->pdi_data_count > 0)
            return;

        char hex_buf[32];
        int idx = s->pdi_data_idx;
        uint64_t val = 0;
        for (int i = 0; i < idx && i < 8; i++)
            val |= ((uint64_t)s->pdi_data_bytes[i]) << (i * 8);
        snprintf(hex_buf, sizeof(hex_buf), "0x%llX", (unsigned long long)val);

        char buf[64];
        snprintf(buf, sizeof(buf), "Data: %s", hex_buf);
        c_put(di, s->ss, s->es, s->out_ann, ANN_DATA_DEV, buf);

        if (s->pdi_num_rd > 1) {
            for (int i = 1; i < s->pdi_num_rd; i++)
                s->pdi_rd_counts[i - 1] = s->pdi_rd_counts[i];
            s->pdi_num_rd--;
            s->pdi_data_count = s->pdi_rd_counts[0];
            s->pdi_data_idx = 0;
            return;
        }

        char cmd_buf[64];
        snprintf(cmd_buf, sizeof(cmd_buf), "%s", pdi_opcode_name(s->pdi_opcode));
        c_put(di, s->pdi_cmd_ss, s->es, s->out_ann, ANN_CMD_DATA, cmd_buf);

        int save_rep = s->pdi_rep_count;
        pdi_clear_insn(s);
        s->pdi_rep_count = save_rep;
    }
}

static void jtag_avr_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    jtag_avr_state *s = (jtag_avr_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "IR TDI") == 0) {
        /* Decode instruction register */
        uint8_t ir_val = (n_fields > 0) ? fields[0].u8 : 0;
        ir_val &= 0x0F; /* 4-bit IR */

        if (ir_val == AVR_IR_BYPASS) {
            s->state = AVR_STATE_BYPASS;
            c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "IR: BYPASS");
        } else if (ir_val == AVR_IR_IDCODE) {
            s->state = AVR_STATE_IDCODE;
            c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "IR: IDCODE");
        } else if (ir_val == AVR_IR_PDICOM) {
            s->state = AVR_STATE_PDICOM;
            c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "IR: PDICOM");
            c_put(di, s->ss, s->es, s->out_ann, ANN_ENABLE, "Enable PDI");
        } else {
            s->state = AVR_STATE_IDLE;
            char buf[32];
            snprintf(buf, sizeof(buf), "IR: UNKNOWN (0x%X)", ir_val);
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING, buf);
        }
        return;
    }

    if (strcmp(cmd, "NEW STATE") == 0) {
        /* On state transition back to idle, disable PDI if active */
        if (s->state == AVR_STATE_PDICOM) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_DISABLE, "Disable PDI");
        }
        s->state = AVR_STATE_IDLE;
        return;
    }

    if (s->state == AVR_STATE_BYPASS) {
        if (strcmp(cmd, "DR TDI") == 0) {
            jtag_avr_handle_bypass(di, s, fields, n_fields);
            s->state = AVR_STATE_IDLE;
        }
    } else if (s->state == AVR_STATE_IDCODE) {
        if (strcmp(cmd, "DR TDO") == 0) {
            jtag_avr_handle_idcode(di, s, fields, n_fields);
            s->state = AVR_STATE_IDLE;
        }
    } else if (s->state == AVR_STATE_PDICOM) {
        if (strcmp(cmd, "DR TDI") == 0) {
            /* PDI input data */
            if (s->pdi_opcode < 0 && n_fields > 0) {
                /* First byte is opcode */
                uint8_t byte_val = fields[0].u8;
                int opcode = (byte_val & 0xE0) >> 5;
                int args = byte_val & 0x1F;
                s->pdi_cmd_ss = start_sample;

                char buf[64];
                snprintf(buf, sizeof(buf), "Opcode: %s", pdi_opcode_name(opcode));
                c_put(di, s->ss, s->es, s->out_ann, ANN_OPCODE, buf);

                pdi_setup_insn(s, opcode, args);

                /* If no data expected, command is complete */
                if (s->pdi_data_count == 0 && s->pdi_num_wr == 0 && s->pdi_num_rd == 0) {
                    snprintf(buf, sizeof(buf), "%s", pdi_opcode_name(opcode));
                    c_put(di, s->pdi_cmd_ss, s->es, s->out_ann, ANN_CMD_DATA, buf);
                    int save_rep = s->pdi_rep_count;
                    pdi_clear_insn(s);
                    s->pdi_rep_count = save_rep;
                } else if (s->pdi_data_count > 0) {
                    /* Process remaining data bytes if any */
                    if (n_fields > 1) {
                        /* Multi-byte in single call - process byte by byte */
                        for (uint64_t i = 1; i < (uint64_t)n_fields && s->pdi_data_count > 0; i++) {
                            if (s->pdi_data_idx < 8)
                                s->pdi_data_bytes[s->pdi_data_idx++] = fields[i].u8;
                            s->pdi_data_count--;
                        }
                        if (s->pdi_data_count == 0) {
                            char hex_buf[32];
                            int idx = s->pdi_data_idx;
                            uint64_t val = 0;
                            for (int i = 0; i < idx && i < 8; i++)
                                val |= ((uint64_t)s->pdi_data_bytes[i]) << (i * 8);
                            snprintf(hex_buf, sizeof(hex_buf), "0x%llX", (unsigned long long)val);

                            char dbuf[64];
                            snprintf(dbuf, sizeof(dbuf), "Data: %s", hex_buf);
                            c_put(di, s->ss, s->es, s->out_ann, ANN_DATA_PROG, dbuf);

                            if (s->pdi_opcode == PDI_OP_REPEAT)
                                s->pdi_rep_count = (int)val;

                            char cmd_buf[64];
                            snprintf(cmd_buf, sizeof(cmd_buf), "%s", pdi_opcode_name(s->pdi_opcode));
                            c_put(di, s->pdi_cmd_ss, s->es, s->out_ann, ANN_CMD_DATA, cmd_buf);

                            int save_rep = s->pdi_rep_count;
                            pdi_clear_insn(s);
                            s->pdi_rep_count = save_rep;
                        }
                    }
                }
            } else if (s->pdi_opcode >= 0) {
                pdi_handle_data_in(di, s, fields, n_fields);
            }
        } else if (strcmp(cmd, "DR TDO") == 0) {
            /* PDI output data */
            c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, "PDICOM");
            if (s->pdi_opcode >= 0) {
                pdi_handle_data_out(di, s, fields, n_fields);
            }
        }
    }
}

static void jtag_avr_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(jtag_avr_state)));
    }
    jtag_avr_state *s = (jtag_avr_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(jtag_avr_state));
    s->state = AVR_STATE_IDLE;
    s->pdi_opcode = -1;
}

static void jtag_avr_start(struct srd_decoder_inst *di)
{
    jtag_avr_state *s = (jtag_avr_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "jtag_avr");
}

static void jtag_avr_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void jtag_avr_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder jtag_avr_c_decoder = {
    .id = "jtag_avr_c",
    .name = "JTAG/AVR(C)",
    .longname = "JTAG / Atmel AVR PDI (C)",
    .desc = "Atmel AVR PDI JTAG protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = jtag_avr_ann_labels,
    .num_annotation_rows = 11,
    .annotation_rows = jtag_avr_ann_rows,
    .inputs = jtag_avr_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = jtag_avr_tags,
    .num_tags = 1,
    .reset = jtag_avr_reset,
    .start = jtag_avr_start,
    .decode = jtag_avr_decode,
    .destroy = jtag_avr_destroy,
    .decode_upper = jtag_avr_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &jtag_avr_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}