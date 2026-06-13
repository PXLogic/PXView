#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_REGISTER = 0,
    ANN_VALUE = 1,
    ANN_WARNING = 2,
    NUM_ANN,
};

enum tca6408a_state {
    TCA6408A_IDLE,
    TCA6408A_GET_SLAVE_ADDR,
    TCA6408A_GET_REG_ADDR,
    TCA6408A_WRITE_IO_REGS,
    TCA6408A_READ_IO_REGS,
    TCA6408A_READ_IO_REGS2,
};

typedef struct {
    enum tca6408a_state state;
    int reg;
    int chip;
    
    uint64_t logic_output_es;
    uint8_t logic_value;
    int out_ann;
    int out_logic;
    uint64_t ss;
    uint64_t es;
} tca6408a_state;

static const char *tca6408a_inputs[] = {"i2c", NULL};
static const char *tca6408a_tags[] = {"Embedded/industrial", "IC", NULL};

static const char *tca6408a_ann_labels[][3] = {
    {"", "register", "Register type"},
    {"", "value", "Register value"},
    {"", "warning", "Warning"},
};

static const int tca6408a_row_regs_classes[] = {ANN_REGISTER, ANN_VALUE};
static const int tca6408a_row_warnings_classes[] = {ANN_WARNING};
static const struct srd_c_ann_row tca6408a_ann_rows[] = {
    {"regs", "Registers", tca6408a_row_regs_classes, 2},
    {"warnings", "Warnings", tca6408a_row_warnings_classes, 1},
};

static void tca6408a_put_logic_states(struct srd_decoder_inst *di, tca6408a_state *s)
{
    if (s->es > s->logic_output_es) {
        uint8_t logic_data[1];
        logic_data[0] = s->logic_value;
        c_put_logic(di, s->logic_output_es, s->es,
                            s->out_logic, 0xFF, logic_data, 8);
        s->logic_output_es = s->es;
    }
}

static void tca6408a_handle_reg(struct srd_decoder_inst *di, tca6408a_state *s,
    int reg, uint8_t b)
{
    char buf[64];
    switch (reg) {
    case 0x00:
        snprintf(buf, sizeof(buf), "State of inputs: %02X", b);
        c_put(di, s->ss, s->es, s->out_ann, ANN_VALUE, buf);
        break;
    case 0x01:
        tca6408a_put_logic_states(di, s);
        snprintf(buf, sizeof(buf), "Outputs set: %02X", b);
        c_put(di, s->ss, s->es, s->out_ann, ANN_VALUE, buf);
        s->logic_value = b;
        break;
    case 0x02:
        snprintf(buf, sizeof(buf), "Polarity inverted: %02X", b);
        c_put(di, s->ss, s->es, s->out_ann, ANN_VALUE, buf);
        break;
    case 0x03:
        snprintf(buf, sizeof(buf), "Configuration: %02X", b);
        c_put(di, s->ss, s->es, s->out_ann, ANN_VALUE, buf);
        break;
    }
}

static void tca6408a_handle_write_reg(struct srd_decoder_inst *di, tca6408a_state *s, int reg)
{
    switch (reg) {
    case 0: c_put(di, s->ss, s->es, s->out_ann, ANN_REGISTER, "Input port", "In", "I"); break;
    case 1: c_put(di, s->ss, s->es, s->out_ann, ANN_REGISTER, "Output port", "Out", "O"); break;
    case 2: c_put(di, s->ss, s->es, s->out_ann, ANN_REGISTER, "Polarity inversion register", "Pol", "P"); break;
    case 3: c_put(di, s->ss, s->es, s->out_ann, ANN_REGISTER, "Configuration register", "Conf", "C"); break;
    }
}

static void tca6408a_check_correct_chip(struct srd_decoder_inst *di, tca6408a_state *s, int addr)
{
    if (addr != 0x20 && addr != 0x21) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "Warning: I²C slave 0x%02X not a TCA6408A compatible chip.", addr);
        c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING, buf);
        s->state = TCA6408A_IDLE;
    }
}

static void tca6408a_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tca6408a_state *s = (tca6408a_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (s->state == TCA6408A_IDLE) {
        if (strcmp(cmd, "START") == 0)
            s->state = TCA6408A_GET_SLAVE_ADDR;
    } else if (s->state == TCA6408A_GET_SLAVE_ADDR) {
        /* Only process ADDRESS WRITE/READ commands, ignore BITS etc. */
        if (strcmp(cmd, "ADDRESS WRITE") == 0 || strcmp(cmd, "ADDRESS READ") == 0) {
            s->chip = (n_fields > 0) ? fields[0].u8 : 0;
            tca6408a_check_correct_chip(di, s, s->chip);
            if (s->state != TCA6408A_IDLE)
                s->state = TCA6408A_GET_REG_ADDR;
        }
    } else if (s->state == TCA6408A_GET_REG_ADDR) {
        if (strcmp(cmd, "ADDRESS READ") == 0 || strcmp(cmd, "ADDRESS WRITE") == 0) {
            tca6408a_check_correct_chip(di, s, (n_fields > 0) ? fields[0].u8 : 0);
            if (s->state == TCA6408A_IDLE)
                return;
        }
        if (strcmp(cmd, "DATA WRITE") != 0)
            return;
        s->reg = (n_fields > 0) ? fields[0].u8 : 0;
        tca6408a_handle_write_reg(di, s, s->reg);
        s->state = TCA6408A_WRITE_IO_REGS;
    } else if (s->state == TCA6408A_WRITE_IO_REGS) {
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = TCA6408A_READ_IO_REGS;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            tca6408a_handle_reg(di, s, s->reg, (n_fields > 0) ? fields[0].u8 : 0);
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = TCA6408A_IDLE;
            s->chip = -1;
        }
    } else if (s->state == TCA6408A_READ_IO_REGS) {
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            s->state = TCA6408A_READ_IO_REGS2;
            s->chip = (n_fields > 0) ? fields[0].u8 : 0;
        }
    } else if (s->state == TCA6408A_READ_IO_REGS2) {
        if (strcmp(cmd, "DATA READ") == 0) {
            tca6408a_handle_reg(di, s, s->reg, (n_fields > 0) ? fields[0].u8 : 0);
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = TCA6408A_IDLE;
        }
    }
}

static void tca6408a_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tca6408a_state)));
    }
    tca6408a_state *s = (tca6408a_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tca6408a_state));
    s->state = TCA6408A_IDLE;
    s->chip = -1;
}

static void tca6408a_start(struct srd_decoder_inst *di)
{
    tca6408a_state *s = (tca6408a_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tca6408a");
    s->out_logic = c_reg_out(di, SRD_OUTPUT_LOGIC, "tca6408a");
}

static void tca6408a_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tca6408a_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder tca6408a_c_decoder = {
    .id = "tca6408a_c",
    .name = "TCA6408A(C)",
    .longname = "Texas Instruments TCA6408A (C)",
    .desc = "Texas Instruments TCA6408A 8-bit I²C I/O expander. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = tca6408a_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = tca6408a_ann_rows,
    .inputs = tca6408a_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = tca6408a_tags,
    .num_tags = 2,
    .reset = tca6408a_reset,
    .start = tca6408a_start,
    .decode = tca6408a_decode,
    .destroy = tca6408a_destroy,
    .decode_upper = tca6408a_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &tca6408a_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}