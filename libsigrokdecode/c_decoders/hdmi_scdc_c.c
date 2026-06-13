#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_ADDRESS = 0,
    ANN_REGISTER = 1,
    ANN_FIELDS = 2,
    ANN_DEBUG = 3,
    NUM_ANN,
};

enum hdmi_scdc_state {
    SCDC_IDLE,
    SCDC_GET_SLAVE_ADDR,
    SCDC_READ_REGISTER,
    SCDC_WRITE_REGISTER,
    SCDC_GET_OFFSET,
    SCDC_OFFSET_RECEIVED,
    SCDC_READ_REGISTER_WOFFSET,
    SCDC_WRITE_REGISTER_WOFFSET,
};

typedef struct {
    enum hdmi_scdc_state state;
    int reg;
    int offset;
    int protocol;
    uint8_t databytes[16];
    int databytes_len;
    int err_det_lower;
    uint64_t block_s, block_e;
    
    int out_ann;
    int verbosity;  /* 0=short, 1=long, 2=debug */
    uint64_t ss;
    uint64_t es;
} hdmi_scdc_state;

/* SCDC register definitions */
typedef struct {
    uint8_t offset;
    const char *name;
} scdc_reg_def;

static const scdc_reg_def scdc_regs[] = {
    {0x01, "Sink version"},
    {0x02, "Source version"},
    {0x10, "Update_0"},
    {0x11, "Update_1"},
    {0x20, "TMDS_Config"},
    {0x21, "Scrambler status"},
    {0x30, "Config_0"},
    {0x40, "Status_Flags_0"},
    {0x41, "Status_Flags_1"},
    {0x50, "Err_Det_0_L"},
    {0x51, "Err_Det_0_H"},
    {0x52, "Err_Det_1_L"},
    {0x53, "Err_Det_1_H"},
    {0x54, "Err_Det_2_L"},
    {0x55, "Err_Det_2_H"},
    {0x56, "Err_Det_Checksum"},
    {0, NULL},
};

/* Field interpretation for each register */
typedef struct {
    uint8_t mask;
    uint8_t value;
    const char *short_text;
    const char *long_text;
} scdc_field_interp;

/* Register 0x01 - Sink version */
static const scdc_field_interp reg_0x01_fields[] = {
    {0xFF, 0x01, "Always 0x01 for HDMI2.0 compliant sinks", ""},
    {0, 0, NULL, NULL},
};

/* Register 0x02 - Source version */
static const scdc_field_interp reg_0x02_fields[] = {
    {0xFF, 0x01, "Source version = 1", " - The Source is supporting HDMI2.0 SCDC registers"},
    {0xFF, 0x00, "Source version = 0", " - The Source is supporting HDMI2.0 SCDC registers"},
    {0, 0, NULL, NULL},
};

/* Register 0x10 - Update_0 */
static const scdc_field_interp reg_0x10_fields[] = {
    {0x01, 0x01, "Status_Update=1", " - Indicating a change in the Status Registers"},
    {0x01, 0x00, "Status_Update=0", " - No change in the Status Register"},
    {0x02, 0x02, "CED_Update=1", " - Indicating a change in the Character Error Detection Registers"},
    {0x02, 0x00, "CED_Update=0", " - No change in the Character Error Detection Register"},
    {0x04, 0x04, "RR_Test=1", " - Generate test Read Request"},
    {0x04, 0x00, "RR_Test=0", " - No test Read Request"},
    {0, 0, NULL, NULL},
};

/* Register 0x11 - Update_1 */
static const scdc_field_interp reg_0x11_fields[] = {
    {0xFF, 0x00, "Reserved to 0x00 in HDMI2.0b", ""},
    {0, 0, NULL, NULL},
};

/* Register 0x20 - TMDS_Config */
static const scdc_field_interp reg_0x20_fields[] = {
    {0x01, 0x01, "Scrambling Enable = ENABLED", ""},
    {0x01, 0x00, "Scrambling Enable = DISABLED", ""},
    {0x02, 0x02, "TMDS_Bit_Clock_Ratio = 1/40", ""},
    {0x02, 0x00, "TMDS_Bit_Clock_Ratio = 1/10", ""},
    {0, 0, NULL, NULL},
};

/* Register 0x21 - Scrambler status */
static const scdc_field_interp reg_0x21_fields[] = {
    {0x01, 0x01, "Scrambling_Status = 1", " - Scrambled control code detected by the Sink"},
    {0x01, 0x00, "Scrambling_Status = 0", " - No scrambled control code detected by the Sink"},
    {0, 0, NULL, NULL},
};

/* Register 0x30 - Config_0 */
static const scdc_field_interp reg_0x30_fields[] = {
    {0x01, 0x01, "RR_Enable = 1", " - Source is supporting Read Request"},
    {0x01, 0x00, "RR_Enable = 0", " - Source only supports polling the Update Flags"},
    {0, 0, NULL, NULL},
};

/* Register 0x40 - Status_Flags_0 */
static const scdc_field_interp reg_0x40_fields[] = {
    {0x01, 0x01, "Clock_Detected = 1", " - Sink detected a valid clock signal."},
    {0x01, 0x00, "Clock_Detected = 0", " - No valid clock signal detected by the Sink."},
    {0x02, 0x02, "CH0_Locked = 1", " - Sink is successfully decoding data on HDMI Channel 0."},
    {0x02, 0x00, "CH0_Locked = 0", " - Sink is not able to decode data on HDMI Channel 0."},
    {0x04, 0x04, "CH1_Locked = 1", " - Sink is successfully decoding data on HDMI Channel 1."},
    {0x04, 0x00, "CH1_Locked = 0", " - Sink is not able to decode data on HDMI Channel 1."},
    {0x08, 0x08, "CH2_Locked = 1", " - Sink is successfully decoding data on HDMI Channel 2."},
    {0x08, 0x00, "CH2_Locked = 0", " - Sink is not able to decode data on HDMI Channel 2."},
    {0, 0, NULL, NULL},
};

/* Register 0x41 - Status_Flags_1 */
static const scdc_field_interp reg_0x41_fields[] = {
    {0xFF, 0x00, "Reserved to 0x00 in HDMI2.0b", ""},
    {0, 0, NULL, NULL},
};

/* Field lookup table by register offset */
typedef struct {
    uint8_t offset;
    const scdc_field_interp *fields;
} scdc_reg_fields_entry;

static const scdc_reg_fields_entry scdc_reg_fields[] = {
    {0x01, reg_0x01_fields},
    {0x02, reg_0x02_fields},
    {0x10, reg_0x10_fields},
    {0x11, reg_0x11_fields},
    {0x20, reg_0x20_fields},
    {0x21, reg_0x21_fields},
    {0x30, reg_0x30_fields},
    {0x40, reg_0x40_fields},
    {0x41, reg_0x41_fields},
    {0, NULL},
};

static const char *hdmi_scdc_lookup_reg_name(int offset)
{
    for (int i = 0; scdc_regs[i].name != NULL; i++) {
        if (scdc_regs[i].offset == offset)
            return scdc_regs[i].name;
    }
    return NULL;
}

static const scdc_field_interp *hdmi_scdc_lookup_reg_fields(int offset)
{
    for (int i = 0; scdc_reg_fields[i].fields != NULL; i++) {
        if (scdc_reg_fields[i].offset == offset)
            return scdc_reg_fields[i].fields;
    }
    return NULL;
}

static void hdmi_scdc_handle_scdc(struct srd_decoder_inst *di, hdmi_scdc_state *s)
{
    uint8_t reg_val = s->databytes[s->databytes_len - 1];
    char messages[512] = "";
    int pos = 0;

    const scdc_field_interp *fields = hdmi_scdc_lookup_reg_fields(s->offset);
    if (fields) {
        for (int i = 0; fields[i].short_text != NULL; i++) {
            uint8_t masked = reg_val & fields[i].mask;
            if (masked == fields[i].value) {
                if (pos > 0)
                    pos += snprintf(messages + pos, sizeof(messages) - pos, " | ");
                if (s->verbosity >= 1 && fields[i].long_text[0] != '\0')
                    pos += snprintf(messages + pos, sizeof(messages) - pos,
                                    "%s%s", fields[i].short_text, fields[i].long_text);
                else
                    pos += snprintf(messages + pos, sizeof(messages) - pos,
                                    "%s", fields[i].short_text);
            }
        }
    }

    if (pos > 0) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELDS, messages);
    }

    /* Handle CED registers (0x50-0x56) */
    if (s->offset >= 0x50 && s->offset <= 0x55) {
        char ced_msg[256] = "";
        int ced_pos = 0;

        if (s->offset == 0x50 || s->offset == 0x52 || s->offset == 0x54) {
            /* First byte of 2-byte CED register */
            s->err_det_lower = reg_val;
            s->block_s = s->ss;
        } else if (s->offset == 0x51 || s->offset == 0x53 || s->offset == 0x55) {
            /* Second byte of 2-byte CED register */
            int error_counter = s->err_det_lower + ((0x7F & reg_val) << 8);
            int channel = (s->offset - 0x51) / 2;
            ced_pos += snprintf(ced_msg + ced_pos, sizeof(ced_msg) - ced_pos,
                                "Channel %d Error Counter = %d", channel, error_counter);
            ced_pos += snprintf(ced_msg + ced_pos, sizeof(ced_msg) - ced_pos,
                                " | Ch%d_Valid = %d", channel, reg_val >> 7);
            c_put(di, s->block_s, s->es, s->out_ann, ANN_FIELDS, ced_msg);
        }

        /* Auto-increment offset for next CED register */
        s->offset++;
    } else if (s->offset == 0x56) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELDS,
                  "Checksum of Character Error Detection registers");
        s->offset++;
    }
}

static const char *hdmi_scdc_inputs[] = {"i2c", NULL};
static const char *hdmi_scdc_outputs[] = {"scdc", NULL};
static const char *hdmi_scdc_tags[] = {"Embedded/industrial", NULL};

static struct srd_decoder_option hdmi_scdc_options[] = {
    {"verbosity", "dec_hdmi_scdc_opt_verbosity", "Verbosity", NULL, NULL},
};

static const char *hdmi_scdc_ann_labels[][3] = {
    {"", "address", "I²C address"},
    {"", "register", "Register name and offset"},
    {"", "fields", "Readable register interpretation"},
    {"", "debug", "Debug messages"},
};

static const int hdmi_scdc_row_scdc_classes[] = {ANN_ADDRESS, ANN_REGISTER, ANN_FIELDS};
static const int hdmi_scdc_row_debug_classes[] = {ANN_DEBUG};
static const struct srd_c_ann_row hdmi_scdc_ann_rows[] = {
    {"scdc", "SCDC", hdmi_scdc_row_scdc_classes, 3},
    {"debug", "Debug", hdmi_scdc_row_debug_classes, 1},
};

static void hdmi_scdc_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    hdmi_scdc_state *s = (hdmi_scdc_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (s->verbosity == 2) {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "%d %s", s->state, cmd);
        c_put(di, s->ss, s->es, s->out_ann, ANN_DEBUG, dbg);
    }

    if (strcmp(cmd, "STOP") == 0) {
        memset(s, 0, sizeof(hdmi_scdc_state));
        s->state = SCDC_IDLE;
        /* Re-read verbosity option */
        const char *verb = c_opt_str(di, "verbosity", "short");
        if (verb && strcmp(verb, "long") == 0) s->verbosity = 1;
        else if (verb && strcmp(verb, "debug") == 0) s->verbosity = 2;
        else s->verbosity = 0;
        return;
    }

    switch (s->state) {
    case SCDC_IDLE:
        if (strcmp(cmd, "START") == 0)
            s->state = SCDC_GET_SLAVE_ADDR;
        break;
    case SCDC_GET_SLAVE_ADDR:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr == 0x54) { /* 7-bit address */
                c_put(di, s->ss, s->es, s->out_ann, ANN_ADDRESS,
                          "SCDC write - Address : 0xA8");
                s->protocol = 1;
                s->state = SCDC_GET_OFFSET;
            }
        } else if (strcmp(cmd, "ADDRESS READ") == 0) {
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr == 0x54) { /* 7-bit address */
                c_put(di, s->ss, s->es, s->out_ann, ANN_ADDRESS,
                          "SCDC read - Address : 0xA9");
                s->protocol = 1;
                s->state = SCDC_READ_REGISTER;
            }
        }
        break;
    case SCDC_GET_OFFSET:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            if (s->protocol) {
                s->offset = (n_fields > 0) ? fields[0].u8 : 0;
                const char *reg_name = hdmi_scdc_lookup_reg_name(s->offset);
                char buf[256];
                if (reg_name)
                    snprintf(buf, sizeof(buf), "Register: %s (0x%02x)", reg_name, s->offset);
                else
                    snprintf(buf, sizeof(buf), "Unknown Register (0x%02x)", s->offset);
                c_put(di, s->ss, s->es, s->out_ann, ANN_REGISTER, buf);
                s->state = SCDC_OFFSET_RECEIVED;
            }
        }
        break;
    case SCDC_OFFSET_RECEIVED:
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = SCDC_GET_SLAVE_ADDR;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            s->databytes[s->databytes_len++] = (n_fields > 0) ? fields[0].u8 : 0;
            s->state = SCDC_WRITE_REGISTER;
            hdmi_scdc_handle_scdc(di, s);
        }
        break;
    case SCDC_READ_REGISTER:
    case SCDC_WRITE_REGISTER:
        if (strcmp(cmd, "DATA READ") == 0 || strcmp(cmd, "DATA WRITE") == 0) {
            s->databytes[s->databytes_len++] = (n_fields > 0) ? fields[0].u8 : 0;
            hdmi_scdc_handle_scdc(di, s);
        } else if (strcmp(cmd, "STOP") == 0 || strcmp(cmd, "START REPEAT") == 0) {
            memset(s, 0, sizeof(hdmi_scdc_state));
            s->state = SCDC_IDLE;
            const char *verb = c_opt_str(di, "verbosity", "short");
            if (verb && strcmp(verb, "long") == 0) s->verbosity = 1;
            else if (verb && strcmp(verb, "debug") == 0) s->verbosity = 2;
            else s->verbosity = 0;
        }
        break;
    default:
        break;
    }
}

static void hdmi_scdc_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(hdmi_scdc_state)));
    }
    hdmi_scdc_state *s = (hdmi_scdc_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(hdmi_scdc_state));
    s->state = SCDC_IDLE;
    const char *verb = c_opt_str(di, "verbosity", "short");
    if (verb && strcmp(verb, "long") == 0) s->verbosity = 1;
    else if (verb && strcmp(verb, "debug") == 0) s->verbosity = 2;
    else s->verbosity = 0;
}

static void hdmi_scdc_start(struct srd_decoder_inst *di)
{
    hdmi_scdc_state *s = (hdmi_scdc_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "hdmi_scdc");
    const char *verb = c_opt_str(di, "verbosity", "short");
    if (verb && strcmp(verb, "long") == 0) s->verbosity = 1;
    else if (verb && strcmp(verb, "debug") == 0) s->verbosity = 2;
    else s->verbosity = 0;
}

static void hdmi_scdc_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void hdmi_scdc_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder hdmi_scdc_c_decoder = {
    .id = "hdmi_scdc_c",
    .name = "HDMI_SCDC(C)",
    .longname = "Status and Control Data Channel (C)",
    .desc = "Status and Control Data Channel: SCDC for HDMI2.0 (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = hdmi_scdc_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = hdmi_scdc_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = hdmi_scdc_ann_rows,
    .inputs = hdmi_scdc_inputs,
    .num_inputs = 1,
    .outputs = hdmi_scdc_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = hdmi_scdc_tags,
    .num_tags = 1,
    .reset = hdmi_scdc_reset,
    .start = hdmi_scdc_start,
    .decode = hdmi_scdc_decode,
    .destroy = hdmi_scdc_destroy,
    .decode_upper = hdmi_scdc_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    hdmi_scdc_options[0].def = g_variant_new_string("short");
    GSList *verb_vals = NULL;
    verb_vals = g_slist_append(verb_vals, g_variant_new_string("short"));
    verb_vals = g_slist_append(verb_vals, g_variant_new_string("long"));
    verb_vals = g_slist_append(verb_vals, g_variant_new_string("debug"));
    hdmi_scdc_options[0].values = verb_vals;

    return &hdmi_scdc_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}