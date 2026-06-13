#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* STM32 JTAG IR values for the Cortex-M3 TAP (4 bits, IR[3:0]) */
#define STM32_IR_BYPASS  0x0F  /* 1111 */
#define STM32_IR_IDCODE  0x0E  /* 1110 */
#define STM32_IR_DPACC   0x0A  /* 1010 */
#define STM32_IR_APACC   0x0B  /* 1011 */
#define STM32_IR_ABORT   0x08  /* 1000 */

/* BS TAP IR (5 bits, IR[8:4]) */
#define STM32_BS_IR_BYPASS 0x1F  /* 11111 */

/* ACK values (3 bits) */
#define STM32_ACK_WAIT      0x01  /* 001 */
#define STM32_ACK_OK_FAULT  0x02  /* 010 */

/* ARM Ltd JEDEC ID */
#define ARM_JEDEC_CONT_CODE 0x04
#define ARM_JEDEC_ID_CODE   0x3B

/* STM32F1 JTAG ID codes */
static const struct {
    uint32_t idcode;
    const char *name;
} stm32f1_idcode_map[] = {
    { 0x06412041, "Low-density device, rev. A" },
    { 0x06410041, "Medium-density device, rev. A" },
    { 0x16410041, "Medium-density device, rev. B/Z/Y" },
    { 0x06414041, "High-density device, rev. A/Z/Y" },
    { 0x06430041, "XL-density device, rev. A" },
    { 0x06418041, "Connectivity-line device, rev. A/Z" },
    { 0, NULL },
};

/* DP register names (addressed via A[3:2]) */
static const char *dp_reg_names[] = {
    "Reserved",       /* 00 */
    "DP CTRL/STAT",   /* 01 */
    "DP SELECT",      /* 10 */
    "DP RDBUFF",      /* 11 */
};

/* APB-AP register names */
static const char *apb_ap_reg_name(uint8_t addr)
{
    switch (addr) {
    case 0x00: return "CSW";
    case 0x04: return "TAR";
    case 0x0c: return "DRW";
    case 0x10: return "BD0";
    case 0x14: return "BD1";
    case 0x18: return "BD2";
    case 0x1c: return "BD3";
    case 0xfc: return "IDR";
    default:   return NULL;
    }
}

enum {
    ANN_ITEM = 0,
    ANN_FIELD,
    ANN_COMMAND,
    ANN_WARNING,
    NUM_ANN,
};

enum stm32_state {
    STM32_STATE_IDLE,
    STM32_STATE_BYPASS,
    STM32_STATE_IDCODE,
    STM32_STATE_DPACC,
    STM32_STATE_APACC,
    STM32_STATE_ABORT,
    STM32_STATE_UNKNOWN,
};

typedef struct {
    int out_ann;
    enum stm32_state state;
    uint64_t ss;
    uint64_t es;
} jtag_stm32_state;

static const char *jtag_stm32_inputs[] = {"jtag", NULL};
static const char *jtag_stm32_tags[] = {"Debug/trace", NULL};

static const char *jtag_stm32_ann_labels[][3] = {
    {"", "item", "Item"},
    {"", "field", "Field"},
    {"", "command", "Command"},
    {"", "warning", "Warning"},
};

static const int jtag_stm32_row_items_classes[] = {ANN_ITEM, -1};
static const int jtag_stm32_row_fields_classes[] = {ANN_FIELD, -1};
static const int jtag_stm32_row_commands_classes[] = {ANN_COMMAND, -1};
static const int jtag_stm32_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row jtag_stm32_ann_rows[] = {
    {"items", "Items", jtag_stm32_row_items_classes, 1},
    {"fields", "Fields", jtag_stm32_row_fields_classes, 1},
    {"commands", "Commands", jtag_stm32_row_commands_classes, 1},
    {"warnings", "Warnings", jtag_stm32_row_warnings_classes, 1},
};

static uint64_t bytes_to_uint64(const c_field *fields, int n_fields)
{
    uint64_t val = 0;
    for (uint64_t i = 0; i < (uint64_t)n_fields && i < 8; i++)
        val |= ((uint64_t)fields[i].u8) << (i * 8);
    return val;
}

static const char *stm32f1_find_idcode(uint32_t idcode)
{
    for (int i = 0; stm32f1_idcode_map[i].name != NULL; i++) {
        if (stm32f1_idcode_map[i].idcode == idcode)
            return stm32f1_idcode_map[i].name;
    }
    return NULL;
}

static void stm32_handle_idcode(struct srd_decoder_inst *di, jtag_stm32_state *s,
                                 const c_field *fields, int n_fields)
{
    if (n_fields < 4)
        return;

    uint32_t idcode = (uint32_t)bytes_to_uint64(fields, n_fields);
    int version = (idcode >> 28) & 0x0F;
    uint16_t part = (idcode >> 12) & 0xFFFF;
    int cont_code = (idcode >> 8) & 0x0F;
    int identity = (idcode >> 1) & 0x7F;

    const char *ver_str = (version == 0x3) ? "JTAG-DP" :
                          (version == 0x2) ? "SW-DP" : "UNKNOWN";
    const char *part_str = (part == 0xba00) ? "JTAG-DP" :
                           (part == 0xba10) ? "SW-DP" : "UNKNOWN";
    const char *manuf_str = ((cont_code + 1) == ARM_JEDEC_CONT_CODE &&
                             identity == ARM_JEDEC_ID_CODE) ? "ARM Ltd." : "UNKNOWN";

    char buf[256];
    snprintf(buf, sizeof(buf), "Continuation code: 0x%x", cont_code);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "Identity code: 0x%x", identity);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "Manufacturer: %s", manuf_str);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "Part: %s", part_str);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    snprintf(buf, sizeof(buf), "Version: %s", ver_str);
    c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

    /* Check if it's a known STM32F1 device */
    const char *dev_name = stm32f1_find_idcode(idcode);
    if (dev_name) {
        snprintf(buf, sizeof(buf), "IDCODE: 0x%08X (%s: %s/%s)", idcode, manuf_str, part_str, ver_str);
        c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
        snprintf(buf, sizeof(buf), "STM32F1: %s", dev_name);
        c_put(di, s->ss, s->es, s->out_ann, ANN_ITEM, buf);
    } else {
        snprintf(buf, sizeof(buf), "IDCODE: 0x%08X (%s: %s/%s)", idcode, manuf_str, part_str, ver_str);
        c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
    }
}

static void stm32_handle_bypass(struct srd_decoder_inst *di, jtag_stm32_state *s,
                                 const c_field *fields, int n_fields)
{
    uint64_t val = bytes_to_uint64(fields, n_fields);
    char buf[32];
    snprintf(buf, sizeof(buf), "BYPASS: %llu", (unsigned long long)val);
    c_put(di, s->ss, s->es, s->out_ann, ANN_ITEM, buf);
}

static void stm32_handle_dpacc_apacc(struct srd_decoder_inst *di, jtag_stm32_state *s,
                                      const char *cmd, const c_field *fields,
                                      int n_fields, int is_apacc)
{
    if (n_fields < 5)
        return;

    /* 35 bits: DATA[34:3] + A[2:1] + RnW[0] for TDI,
       or DATA[34:3] + ACK[2:0] for TDO */
    

    if (strcmp(cmd, "DR TDI") == 0) {
        /* TDI: DATA[31:0] in bits[34:3], A[3:2] in bits[2:1], RnW in bit[0] */
        uint64_t val = bytes_to_uint64(fields, n_fields);
        /* Extract from the 35-bit value */
        uint64_t data_val = (val >> 3) & 0xFFFFFFFF;
        int a = (val >> 1) & 0x03;
        int rnw = val & 0x01;
        const char *rw_str = rnw ? "Read request" : "Write request";

        char buf[256];
        if (is_apacc) {
            const char *ap_reg = apb_ap_reg_name(a * 4);
            if (ap_reg)
                snprintf(buf, sizeof(buf), "New transaction: DATA: 0x%08X, A: %s, RnW: %s",
                         (uint32_t)data_val, ap_reg, rw_str);
            else
                snprintf(buf, sizeof(buf), "New transaction: DATA: 0x%08X, A: 0x%02X, RnW: %s",
                         (uint32_t)data_val, a * 4, rw_str);
        } else {
            snprintf(buf, sizeof(buf), "New transaction: DATA: 0x%08X, A: %s, RnW: %s",
                     (uint32_t)data_val, dp_reg_names[a], rw_str);
        }
        c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
    } else if (strcmp(cmd, "DR TDO") == 0) {
        /* TDO: DATA[31:0] in bits[34:3], ACK[2:0] in bits[2:0] */
        uint64_t val = bytes_to_uint64(fields, n_fields);
        uint64_t data_val = (val >> 3) & 0xFFFFFFFF;
        int ack = val & 0x07;

        const char *ack_str;
        switch (ack) {
        case STM32_ACK_WAIT:     ack_str = "WAIT"; break;
        case STM32_ACK_OK_FAULT: ack_str = "OK/FAULT"; break;
        default:                 ack_str = "Reserved"; break;
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "Previous transaction result: DATA: 0x%08X, ACK: %s",
                 (uint32_t)data_val, ack_str);
        c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
    }
}

static void stm32_handle_abort(struct srd_decoder_inst *di, jtag_stm32_state *s,
                                const c_field *fields, int n_fields)
{
    if (n_fields < 5)
        return;

    uint64_t val = bytes_to_uint64(fields, n_fields);
    int dapabort = val & 0x01;

    char buf[256];
    const char *a = dapabort ? "" : "No ";
    snprintf(buf, sizeof(buf), "DAPABORT = %d: %sDAP abort generated", dapabort, a);
    c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);

    /* Warn if reserved bits are non-zero */
    if ((val & ~0x01ULL) != 0) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING,
                  "WARNING: DAPABORT[31:1] reserved!");
    }
}

static void jtag_stm32_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    jtag_stm32_state *s = (jtag_stm32_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "IR TDI") == 0) {
        /* STM32 has 9-bit IR: 5 bits for BS TAP + 4 bits for M3 TAP */
        if (n_fields < 2)
            return;

        uint16_t ir_full = (uint16_t)bytes_to_uint64(fields, n_fields);
        int m3_ir = ir_full & 0x0F;        /* IR[3:0] - Cortex-M3 TAP */
        int bs_ir = (ir_full >> 4) & 0x1F;  /* IR[8:4] - BS TAP */

        /* Decode BS TAP instruction */
        const char *bs_ir_name = (bs_ir == STM32_BS_IR_BYPASS) ? "BYPASS" : "UNKNOWN";
        char buf[64];
        snprintf(buf, sizeof(buf), "IR (BS TAP): %s", bs_ir_name);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

        /* Decode M3 TAP instruction */
        const char *m3_ir_name;
        switch (m3_ir) {
        case STM32_IR_BYPASS: m3_ir_name = "BYPASS"; s->state = STM32_STATE_BYPASS; break;
        case STM32_IR_IDCODE: m3_ir_name = "IDCODE"; s->state = STM32_STATE_IDCODE; break;
        case STM32_IR_DPACC:  m3_ir_name = "DPACC";  s->state = STM32_STATE_DPACC; break;
        case STM32_IR_APACC:  m3_ir_name = "APACC";  s->state = STM32_STATE_APACC; break;
        case STM32_IR_ABORT:  m3_ir_name = "ABORT";  s->state = STM32_STATE_ABORT; break;
        default:              m3_ir_name = "UNKNOWN"; s->state = STM32_STATE_UNKNOWN; break;
        }

        snprintf(buf, sizeof(buf), "IR (M3 TAP): %s", m3_ir_name);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

        snprintf(buf, sizeof(buf), "IR: %s", m3_ir_name);
        c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
        return;
    }

    if (strcmp(cmd, "NEW STATE") == 0) {
        s->state = STM32_STATE_IDLE;
        return;
    }

    /* Handle DR data based on current state */
    switch (s->state) {
    case STM32_STATE_BYPASS:
        if (strcmp(cmd, "DR TDI") == 0) {
            stm32_handle_bypass(di, s, fields, n_fields);
            s->state = STM32_STATE_IDLE;
        }
        break;

    case STM32_STATE_IDCODE:
        if (strcmp(cmd, "DR TDO") == 0) {
            stm32_handle_idcode(di, s, fields, n_fields);
            s->state = STM32_STATE_IDLE;
        }
        break;

    case STM32_STATE_DPACC:
        if (strcmp(cmd, "DR TDI") == 0 || strcmp(cmd, "DR TDO") == 0) {
            stm32_handle_dpacc_apacc(di, s, cmd, fields, n_fields, 0);
            if (strcmp(cmd, "DR TDO") == 0)
                s->state = STM32_STATE_IDLE;
        }
        break;

    case STM32_STATE_APACC:
        if (strcmp(cmd, "DR TDI") == 0 || strcmp(cmd, "DR TDO") == 0) {
            stm32_handle_dpacc_apacc(di, s, cmd, fields, n_fields, 1);
            if (strcmp(cmd, "DR TDO") == 0)
                s->state = STM32_STATE_IDLE;
        }
        break;

    case STM32_STATE_ABORT:
        if (strcmp(cmd, "DR TDO") == 0) {
            stm32_handle_abort(di, s, fields, n_fields);
            s->state = STM32_STATE_IDLE;
        }
        break;

    case STM32_STATE_UNKNOWN:
        if (strcmp(cmd, "DR TDO") == 0) {
            char buf[64];
            uint64_t val = bytes_to_uint64(fields, n_fields);
            snprintf(buf, sizeof(buf), "Unknown instruction: 0x%llX", (unsigned long long)val);
            c_put(di, s->ss, s->es, s->out_ann, ANN_COMMAND, buf);
            s->state = STM32_STATE_IDLE;
        }
        break;

    default:
        break;
    }
}

static void jtag_stm32_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(jtag_stm32_state)));
    }
    jtag_stm32_state *s = (jtag_stm32_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(jtag_stm32_state));
    s->state = STM32_STATE_IDLE;
}

static void jtag_stm32_start(struct srd_decoder_inst *di)
{
    jtag_stm32_state *s = (jtag_stm32_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "jtag_stm32");
}

static void jtag_stm32_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void jtag_stm32_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder jtag_stm32_c_decoder = {
    .id = "jtag_stm32_c",
    .name = "JTAG/STM32(C)",
    .longname = "JTAG / ST STM32 (C)",
    .desc = "ST STM32-specific JTAG protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = jtag_stm32_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = jtag_stm32_ann_rows,
    .inputs = jtag_stm32_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = jtag_stm32_tags,
    .num_tags = 1,
    .reset = jtag_stm32_reset,
    .start = jtag_stm32_start,
    .decode = jtag_stm32_decode,
    .destroy = jtag_stm32_destroy,
    .decode_upper = jtag_stm32_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &jtag_stm32_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}