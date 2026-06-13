#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* EJTAG instruction register values */
#define EJTAG_IR_IDCODE      0x01
#define EJTAG_IR_IMPCODE     0x03
#define EJTAG_IR_ADDRESS     0x08
#define EJTAG_IR_DATA        0x09
#define EJTAG_IR_CONTROL     0x0A
#define EJTAG_IR_ALL         0x0B
#define EJTAG_IR_EJTAGBOOT   0x0C
#define EJTAG_IR_NORMALBOOT  0x0D
#define EJTAG_IR_FASTDATA    0x0E
#define EJTAG_IR_TCBCONTROLA 0x10
#define EJTAG_IR_TCBCONTROLB 0x11
#define EJTAG_IR_TCBDATA     0x12
#define EJTAG_IR_TCBCONTROLC 0x13
#define EJTAG_IR_PCSAMPLE    0x14
#define EJTAG_IR_TCBCONTROLD 0x15
#define EJTAG_IR_TCBCONTROLE 0x16

/* EJTAG states */
enum ejtag_state {
    EJTAG_STATE_RESET = 0,
    EJTAG_STATE_DEVICE_ID = 1,
    EJTAG_STATE_IMPLEMENTATION = 2,
    EJTAG_STATE_DATA = 3,
    EJTAG_STATE_ADDRESS = 4,
    EJTAG_STATE_CONTROL = 5,
    EJTAG_STATE_FASTDATA = 6,
    EJTAG_STATE_PC_SAMPLE = 7,
    EJTAG_STATE_BYPASS = 8,
};

/* Control register bit fields */
#define EJTAG_CTRL_ROCC    (1U << 31)
#define EJTAG_CTRL_PSZ     (3U << 29)
#define EJTAG_CTRL_VPED    (1U << 23)
#define EJTAG_CTRL_DOZE    (1U << 22)
#define EJTAG_CTRL_HALT    (1U << 21)
#define EJTAG_CTRL_PER_RST (1U << 20)
#define EJTAG_CTRL_PRNW    (1U << 19)
#define EJTAG_CTRL_PRACC   (1U << 18)
#define EJTAG_CTRL_PR_RST  (1U << 16)
#define EJTAG_CTRL_PROB_EN (1U << 15)
#define EJTAG_CTRL_PROB_TRAP (1U << 14)
#define EJTAG_CTRL_ISA_ON_DEBUG (1U << 13)
#define EJTAG_CTRL_EJTAG_BRK (1U << 12)
#define EJTAG_CTRL_DM      (1U << 3)

enum {
    ANN_INSTRUCTION = 0,
    ANN_REG_RESET,
    ANN_REG_DEVICE_ID,
    ANN_REG_IMPLEMENTATION,
    ANN_REG_DATA,
    ANN_REG_ADDRESS,
    ANN_REG_CONTROL,
    ANN_REG_FASTDATA,
    ANN_REG_PC_SAMPLE,
    ANN_REG_BYPASS,
    ANN_CONTROL_FIELD_IN,
    ANN_CONTROL_FIELD_OUT,
    ANN_PRACC,
    NUM_ANN,
};

/* EJTAG instruction names */
static const struct {
    uint8_t code;
    const char *name;
    const char *desc;
} ejtag_insn_map[] = {
    { 0x00, "Free",        "Boundary scan" },
    { 0x01, "IDCODE",      "Select Device Identification register" },
    { 0x02, "Free",        "Boundary scan" },
    { 0x03, "IMPCODE",     "Select Implementation register" },
    { 0x08, "ADDRESS",     "Select Address register" },
    { 0x09, "DATA",        "Select Data register" },
    { 0x0A, "CONTROL",     "Select EJTAG Control register" },
    { 0x0B, "ALL",         "Select Address, Data and Control registers" },
    { 0x0C, "EJTAGBOOT",   "Fetch code from debug exception vector after reset" },
    { 0x0D, "NORMALBOOT",  "Execute the reset handler after reset" },
    { 0x0E, "FASTDATA",    "Select Data and Fastdata registers" },
    { 0x0F, "Reserved",    "Reserved" },
    { 0x10, "TCBCONTROLA", "Select TCBTraceControl register" },
    { 0x11, "TCBCONTROLB", "Select trace control block register B" },
    { 0x12, "TCBDATA",     "Access registers specified by TCBCONTROLB" },
    { 0x13, "TCBCONTROLC", "Select trace control block register C" },
    { 0x14, "PCSAMPLE",    "Select PCsample register" },
    { 0x15, "TCBCONTROLD", "Select trace control block register D" },
    { 0x16, "TCBCONTROLE", "Select trace control block register E" },
    { 0x17, "FDC",         "Select Fast Debug Channel" },
    { 0x1C, "Free",        "Boundary scan" },
    { 0xFF, NULL, NULL },
};

/* EJTAG register names */
static const char *ejtag_reg_names[] = {
    "RESET", "DEVICE_ID", "IMPLEMENTATION", "DATA",
    "ADDRESS", "CONTROL", "FASTDATA", "PC_SAMPLE", "BYPASS",
};

/* Control register field descriptions */
typedef struct {
    int start_bit;
    int end_bit;
    const char *name;
    const char *read_desc[4];
    int read_desc_count;
    const char *write_desc[4];
    int write_desc_count;
} ejtag_ctrl_field;

static const ejtag_ctrl_field ejtag_ctrl_fields[] = {
    { 31, 31, "Rocc",
      {"No reset occurred", "Reset occurred"}, 2,
      {"Acknowledge reset", "No effect"}, 2 },
    { 30, 29, "Psz",
      {"Access: byte", "Access: halfword", "Access: word", "Access: triple"}, 4,
      {NULL}, 0 },
    { 23, 23, "VPED",
      {"VPE disabled", "VPE enabled"}, 2,
      {NULL}, 0 },
    { 22, 22, "Doze",
      {"Processor not in low-power mode", "Processor in low-power mode"}, 2,
      {NULL}, 0 },
    { 21, 21, "Halt",
      {"Internal system bus clock running", "Internal system bus clock stopped"}, 2,
      {NULL}, 0 },
    { 20, 20, "Per Rst",
      {"No peripheral reset applied", "Peripheral reset applied"}, 2,
      {"Deassert peripheral reset", "Assert peripheral reset"}, 2 },
    { 19, 19, "PRn W",
      {"Read processor access", "Write processor access"}, 2,
      {NULL}, 0 },
    { 18, 18, "Pr Acc",
      {"No pending processor access", "Pending processor access"}, 2,
      {"Finish processor access", "Don't finish processor access"}, 2 },
    { 16, 16, "Pr Rst",
      {"No processor reset applied", "Processor reset applied"}, 2,
      {"Deassert processor reset", "Assert system reset"}, 2 },
    { 15, 15, "Prob En",
      {"Probe will not serve processor accesses", "Probe will service processor accesses"}, 2,
      {NULL}, 0 },
    { 14, 14, "Prob Trap",
      {"Default location", "DMSEG fetch"}, 2,
      {"Set to default location", "Set to DMSEG fetch"}, 2 },
    { 13, 13, "ISA On Debug",
      {"MIPS32/MIPS64 ISA", "microMIPS ISA"}, 2,
      {"Set to MIPS32/MIPS64 ISA", "Set to microMIPS ISA"}, 2 },
    { 12, 12, "EJTAG Brk",
      {"No pending debug interrupt", "Pending debug interrupt"}, 2,
      {"No effect", "Request debug interrupt"}, 2 },
    { 3, 3, "DM",
      {"Not in debug mode", "In debug mode"}, 2,
      {NULL}, 0 },
};

#define NUM_CTRL_FIELDS (sizeof(ejtag_ctrl_fields) / sizeof(ejtag_ctrl_fields[0]))

typedef struct {
    uint32_t data_in;
    uint32_t data_out;
    uint64_t ss_in;
    uint64_t es_in;
    uint64_t ss_out;
    uint64_t es_out;
    int has_in;
    int has_out;
} ejtag_last_data;

typedef struct {
    uint32_t address_in;
    uint32_t address_out;
    uint32_t data_in;
    uint32_t data_out;
    int write;
    int has_in;
    int has_out;
    uint64_t ss;
    uint64_t es;
} ejtag_pracc_state;

typedef struct {
    int out_ann;
    enum ejtag_state state;
    ejtag_last_data last_data;
    ejtag_pracc_state pracc;
    uint64_t ss;
    uint64_t es;
} jtag_ejtag_state;

static const char *jtag_ejtag_inputs[] = {"jtag", NULL};
static const char *jtag_ejtag_tags[] = {"Debug/trace", NULL};

static const char *jtag_ejtag_ann_labels[][3] = {
    {"", "instruction", "Instruction"},
    {"", "reset", "RESET"},
    {"", "device_id", "DEVICE_ID"},
    {"", "implementation", "IMPLEMENTATION"},
    {"", "data", "DATA"},
    {"", "address", "ADDRESS"},
    {"", "control", "CONTROL"},
    {"", "fastdata", "FASTDATA"},
    {"", "pc_sample", "PC_SAMPLE"},
    {"", "bypass", "BYPASS"},
    {"", "control_field_in", "Control field in"},
    {"", "control_field_out", "Control field out"},
    {"", "pracc", "PrAcc"},
};

static const int jtag_ejtag_row_instructions_classes[] = {ANN_INSTRUCTION, -1};
static const int jtag_ejtag_row_regs_classes[] = {
    ANN_REG_RESET, ANN_REG_DEVICE_ID, ANN_REG_IMPLEMENTATION,
    ANN_REG_DATA, ANN_REG_ADDRESS, ANN_REG_CONTROL,
    ANN_REG_FASTDATA, ANN_REG_PC_SAMPLE, ANN_REG_BYPASS, -1
};
static const int jtag_ejtag_row_ctrl_in_classes[] = {ANN_CONTROL_FIELD_IN, -1};
static const int jtag_ejtag_row_ctrl_out_classes[] = {ANN_CONTROL_FIELD_OUT, -1};
static const int jtag_ejtag_row_pracc_classes[] = {ANN_PRACC, -1};

static const struct srd_c_ann_row jtag_ejtag_ann_rows[] = {
    {"instructions", "Instructions", jtag_ejtag_row_instructions_classes, 1},
    {"regs", "Registers", jtag_ejtag_row_regs_classes, 9},
    {"control_fields_in", "Control fields in", jtag_ejtag_row_ctrl_in_classes, 1},
    {"control_fields_out", "Control fields out", jtag_ejtag_row_ctrl_out_classes, 1},
    {"pracc", "PrAcc", jtag_ejtag_row_pracc_classes, 1},
};

static uint32_t bytes_to_uint32(const c_field *fields, int n_fields)
{
    uint32_t val = 0;
    for (uint64_t i = 0; i < (uint64_t)n_fields && i < 4; i++)
        val |= ((uint32_t)fields[i].u8) << (i * 8);
    return val;
}

static const char *ejtag_find_insn_name(uint8_t code)
{
    for (int i = 0; ejtag_insn_map[i].name != NULL; i++) {
        if (ejtag_insn_map[i].code == code)
            return ejtag_insn_map[i].name;
    }
    return NULL;
}

static const char *ejtag_find_insn_desc(uint8_t code)
{
    for (int i = 0; ejtag_insn_map[i].name != NULL; i++) {
        if (ejtag_insn_map[i].code == code)
            return ejtag_insn_map[i].desc;
    }
    return NULL;
}

static enum ejtag_state ejtag_select_state(uint8_t ir_val)
{
    switch (ir_val) {
    case EJTAG_IR_IDCODE:  return EJTAG_STATE_DEVICE_ID;
    case EJTAG_IR_IMPCODE: return EJTAG_STATE_IMPLEMENTATION;
    case EJTAG_IR_DATA:    return EJTAG_STATE_DATA;
    case EJTAG_IR_ADDRESS: return EJTAG_STATE_ADDRESS;
    case EJTAG_IR_CONTROL: return EJTAG_STATE_CONTROL;
    case EJTAG_IR_FASTDATA:return EJTAG_STATE_FASTDATA;
    default:               return EJTAG_STATE_RESET;
    }
}

static void ejtag_parse_control_reg(struct srd_decoder_inst *di, jtag_ejtag_state *s,
                                     int ann, uint32_t ctrl_val)
{
    int is_write = (ann == ANN_CONTROL_FIELD_IN);

    for (int f = 0; f < (int)NUM_CTRL_FIELDS; f++) {
        const ejtag_ctrl_field *field = &ejtag_ctrl_fields[f];
        
        uint32_t mask = 0;
        for (int b = field->end_bit; b <= field->start_bit; b++)
            mask |= (1U << b);
        int value = (ctrl_val & mask) >> field->end_bit;

        const char *desc = NULL;
        if (is_write && field->write_desc_count > 0) {
            if (value < field->write_desc_count && field->write_desc[value])
                desc = field->write_desc[value];
        } else if (!is_write && field->read_desc_count > 0) {
            if (value < field->read_desc_count && field->read_desc[value])
                desc = field->read_desc[value];
        }

        char buf[256];
        if (desc) {
            snprintf(buf, sizeof(buf), "%s: %s", field->name, desc);
        } else {
            snprintf(buf, sizeof(buf), "%s: %d", field->name, value);
        }
        c_put(di, s->ss, s->es, s->out_ann, ann, buf);
    }
}

static void ejtag_parse_pracc(struct srd_decoder_inst *di, jtag_ejtag_state *s)
{
    uint32_t ctrl_in = s->last_data.data_in;
    uint32_t ctrl_out = s->last_data.data_out;

    /* Check if JTAG master acknowledges a pending PrAcc */
    if ((ctrl_in & EJTAG_CTRL_PRACC) != 0 || (ctrl_out & EJTAG_CTRL_PRACC) == 0)
        return;

    int pracc_write = (ctrl_out & EJTAG_CTRL_PRNW) != 0;
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "PrAcc: %s",
                    pracc_write ? "Store" : "Load/Fetch");

    if (pracc_write) {
        if (s->pracc.address_out != 0 || s->pracc.has_out)
            pos += snprintf(buf + pos, sizeof(buf) - pos, ", A: 0x%08X", s->pracc.address_out);
        if (s->pracc.data_out != 0 || s->pracc.has_out)
            pos += snprintf(buf + pos, sizeof(buf) - pos, ", D: 0x%08X", s->pracc.data_out);
    } else {
        if (s->pracc.address_out != 0 || s->pracc.has_out)
            pos += snprintf(buf + pos, sizeof(buf) - pos, ", A: 0x%08X", s->pracc.address_out);
        if (s->pracc.data_in != 0 || s->pracc.has_in)
            pos += snprintf(buf + pos, sizeof(buf) - pos, ", D: 0x%08X", s->pracc.data_in);
    }

    /* Reset pracc state */
    memset(&s->pracc, 0, sizeof(s->pracc));

    c_put(di, s->ss, s->es, s->out_ann, ANN_PRACC, buf);
}

static void jtag_ejtag_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    jtag_ejtag_state *s = (jtag_ejtag_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "IR TDI") == 0) {
        uint8_t ir_val = (n_fields > 0) ? fields[0].u8 : 0;
        const char *name = ejtag_find_insn_name(ir_val);
        const char *desc = ejtag_find_insn_desc(ir_val);

        char buf[256];
        if (name) {
            snprintf(buf, sizeof(buf), "%s: %s (0x%02X)", name, desc ? desc : "", ir_val);
            c_put(di, s->ss, s->es, s->out_ann, ANN_INSTRUCTION, buf);
        } else {
            snprintf(buf, sizeof(buf), "0x%02X", ir_val);
            c_put(di, s->ss, s->es, s->out_ann, ANN_INSTRUCTION, buf);
        }
        s->state = ejtag_select_state(ir_val);
        return;
    }

    if (strcmp(cmd, "DR TDI") == 0) {
        uint32_t value = bytes_to_uint32(fields, n_fields);
        s->last_data.data_in = value;
        s->last_data.ss_in = start_sample;
        s->last_data.es_in = end_sample;
        s->last_data.has_in = 1;

        s->pracc.ss = start_sample;
        s->pracc.es = end_sample;

        if (s->state == EJTAG_STATE_ADDRESS) {
            s->pracc.address_in = value;
        } else if (s->state == EJTAG_STATE_DATA) {
            s->pracc.data_in = value;
            s->pracc.has_in = 1;
        } else if (s->state == EJTAG_STATE_FASTDATA) {
            /* FASTDATA: 33 bits, bit 32 is SPrAcc, bits 31:0 are data */
            char buf[64];
            snprintf(buf, sizeof(buf), "FASTDATA IN: 0x%08X", value);
            c_put(di, s->ss, s->es, s->out_ann, ANN_CONTROL_FIELD_IN, buf);
        }
        return;
    }

    if (strcmp(cmd, "DR TDO") == 0) {
        uint32_t value = bytes_to_uint32(fields, n_fields);
        s->last_data.data_out = value;
        s->last_data.ss_out = start_sample;
        s->last_data.es_out = end_sample;
        s->last_data.has_out = 1;

        if (s->state == EJTAG_STATE_ADDRESS) {
            s->pracc.address_out = value;
        } else if (s->state == EJTAG_STATE_DATA) {
            s->pracc.data_out = value;
            s->pracc.has_out = 1;
        } else if (s->state == EJTAG_STATE_FASTDATA) {
            char buf[64];
            snprintf(buf, sizeof(buf), "FASTDATA OUT: 0x%08X", value);
            c_put(di, s->ss, s->es, s->out_ann, ANN_CONTROL_FIELD_OUT, buf);
        }
        return;
    }

    if (strcmp(cmd, "NEW STATE") == 0) {
        /* On UPDATE-DR, process the register data */
        if (s->state == EJTAG_STATE_RESET)
            return;

        if (s->state >= EJTAG_STATE_RESET && s->state <= EJTAG_STATE_BYPASS) {
            int ann_idx = ANN_REG_RESET + s->state;
            const char *reg_name = ejtag_reg_names[s->state];
            char buf[64];
            snprintf(buf, sizeof(buf), "%s", reg_name);
            c_put(di, s->ss, s->es, s->out_ann, ann_idx, buf);

            /* For data/address registers, show the value */
            if (s->state == EJTAG_STATE_DEVICE_ID && s->last_data.has_out) {
                snprintf(buf, sizeof(buf), "IDCODE: 0x%08X", s->last_data.data_out);
                c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DEVICE_ID, buf);
            } else if (s->state == EJTAG_STATE_IMPLEMENTATION && s->last_data.has_out) {
                snprintf(buf, sizeof(buf), "IMPCODE: 0x%08X", s->last_data.data_out);
                c_put(di, s->ss, s->es, s->out_ann, ANN_REG_IMPLEMENTATION, buf);
            } else if (s->state == EJTAG_STATE_ADDRESS) {
                if (s->last_data.has_in) {
                    snprintf(buf, sizeof(buf), "ADDRESS IN: 0x%08X", s->last_data.data_in);
                    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_ADDRESS, buf);
                }
                if (s->last_data.has_out) {
                    snprintf(buf, sizeof(buf), "ADDRESS OUT: 0x%08X", s->last_data.data_out);
                    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_ADDRESS, buf);
                }
            } else if (s->state == EJTAG_STATE_DATA) {
                if (s->last_data.has_in) {
                    snprintf(buf, sizeof(buf), "DATA IN: 0x%08X", s->last_data.data_in);
                    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, buf);
                }
                if (s->last_data.has_out) {
                    snprintf(buf, sizeof(buf), "DATA OUT: 0x%08X", s->last_data.data_out);
                    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, buf);
                }
            } else if (s->state == EJTAG_STATE_CONTROL) {
                /* Parse control register fields */
                if (s->last_data.has_in) {
                    ejtag_parse_control_reg(di, s, ANN_CONTROL_FIELD_IN, s->last_data.data_in);
                }
                if (s->last_data.has_out) {
                    ejtag_parse_control_reg(di, s, ANN_CONTROL_FIELD_OUT, s->last_data.data_out);
                }
                ejtag_parse_pracc(di, s);
            }
        }

        /* Reset last data */
        memset(&s->last_data, 0, sizeof(s->last_data));
        return;
    }
}

static void jtag_ejtag_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(jtag_ejtag_state)));
    }
    jtag_ejtag_state *s = (jtag_ejtag_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(jtag_ejtag_state));
    s->state = EJTAG_STATE_RESET;
}

static void jtag_ejtag_start(struct srd_decoder_inst *di)
{
    jtag_ejtag_state *s = (jtag_ejtag_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "jtag_ejtag");
}

static void jtag_ejtag_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void jtag_ejtag_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder jtag_ejtag_c_decoder = {
    .id = "jtag_ejtag_c",
    .name = "JTAG/EJTAG(C)",
    .longname = "JTAG / MIPS EJTAG (C)",
    .desc = "MIPS EJTAG debug protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = jtag_ejtag_ann_labels,
    .num_annotation_rows = 5,
    .annotation_rows = jtag_ejtag_ann_rows,
    .inputs = jtag_ejtag_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = jtag_ejtag_tags,
    .num_tags = 1,
    .reset = jtag_ejtag_reset,
    .start = jtag_ejtag_start,
    .decode = jtag_ejtag_decode,
    .destroy = jtag_ejtag_destroy,
    .decode_upper = jtag_ejtag_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &jtag_ejtag_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}