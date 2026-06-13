#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_REGISTER = 0,
    ANN_FIELD,
    ANN_PTR_BYTE,
    ANN_SLAVE_ADDR,
    ANN_DATA_BYTE,
    ANN_WARNING,
    NUM_ANN,
};

enum ad5593r_state {
    AD5593R_IDLE,
    AD5593R_GET_SLAVE_ADDR,
    AD5593R_GET_POINTER_BYTE,
    AD5593R_GET_DATA_HIGH,
    AD5593R_GET_DATA_LOW,
};

/* CONFIG_MODE_BITS_MAP: opcode -> register name */
static const char *config_mode_bits_map[] = {
    "NOP",         /* 0b0000 */
    NULL,          /* 0b0001 - undefined */
    "ADC_SEQ",     /* 0b0010 */
    "GEN_CTRL_REG",/* 0b0011 */
    "ADC_CONFIG",  /* 0b0100 */
    "DAC_CONFIG",  /* 0b0101 */
    "PULLDWN_CONFIG",/* 0b0110 */
    "LDAC_MODE",   /* 0b0111 */
    "GPIO_CONFIG", /* 0b1000 */
    "GPIO_OUTPUT", /* 0b1001 */
    "GPIO_INPUT",  /* 0b1010 */
    "PD_REF_CTRL", /* 0b1011 */
    "GPIO_OPENDRAIN_CONFIG", /* 0b1100 */
    "IO_TS_CONFIG",/* 0b1101 */
    NULL,          /* 0b1110 - undefined */
    "SW_RESET",    /* 0b1111 */
};

/* REG_SEL_RD_MAP */
static const char *reg_sel_rd_map[] = {
    "NOP", NULL, "ADC_SEQ", "GEN_CTRL_REG",
    "ADC_CONFIG", "DAC_CONFIG", "PULLDWN_CONFIG", "LDAC_MODE",
    "GPIO_CONFIG", "GPIO_OUTPUT", "GPIO_INPUT", "PD_REF_CTRL",
    "GPIO_OPENDRAIN_CONFIG", "IO_TS_CONFIG",
};

typedef struct {
    enum ad5593r_state state;
    int io_operation_type; /* 0=write, 1=read */
    char databyte_register[64];
    uint8_t data_high;
    uint8_t data_low;
    
    uint64_t ss_block, es_block;
    uint64_t ss, es;
    uint64_t bits_ss_msb;
    uint64_t bits_es_bit1;
    uint64_t bits_es_bit0;
    int out_ann;
    double vref;
} ad5593r_state;

static const char *ad5593r_inputs[] = {"i2c", NULL};
static const char *ad5593r_tags[] = {"IC", "Analog/digital", NULL};

static struct srd_decoder_option ad5593r_options[] = {
    {"Vref", "dec_ad5593r_opt_vref", "Reference voltage (V)", NULL, NULL},
};

static const char *ad5593r_ann_labels[][3] = {
    {"", "register", "Register"},
    {"", "field", "Field"},
    {"", "ptr_byte", "Pointer Byte"},
    {"", "slave_addr", "Slave Address"},
    {"", "data_byte", "Data Byte"},
    {"", "warning", "Warning"},
};

static const int ad5593r_row_packet_classes[] = {ANN_PTR_BYTE, ANN_SLAVE_ADDR, ANN_WARNING, ANN_DATA_BYTE, -1};
static const int ad5593r_row_registers_classes[] = {ANN_REGISTER, -1};
static const int ad5593r_row_fields_classes[] = {ANN_FIELD, -1};

static const struct srd_c_ann_row ad5593r_ann_rows[] = {
    {"packet", "Packets", ad5593r_row_packet_classes, 4},
    {"registers", "Registers", ad5593r_row_registers_classes, 1},
    {"fields", "Fields", ad5593r_row_fields_classes, 1},
};

static void ad5593r_putx(struct srd_decoder_inst *di, ad5593r_state *s, int cls, const char *text)
{
    c_put(di, s->ss, s->es, s->out_ann, cls, text);
}

static void ad5593r_putb(struct srd_decoder_inst *di, ad5593r_state *s, int cls, const char *text)
{
    c_put(di, s->ss_block, s->es_block, s->out_ann, cls, text);
}

static const char *bit_indices_str(uint16_t val)
{
    static char buf[64];
    int pos = 0;
    int first = 1;
    for (int i = 0; i < 16 && val; i++) {
        if (val & 1) {
            if (!first) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%d", i);
            first = 0;
        }
        val >>= 1;
    }
    if (first) return "NONE";
    return buf;
}

static void ad5593r_decode_field(struct srd_decoder_inst *di, ad5593r_state *s,
    const char *name, uint16_t val)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %u", name, val);
    ad5593r_putx(di, s, ANN_FIELD, buf);
}

static void ad5593r_decode_field_hex(struct srd_decoder_inst *di, ad5593r_state *s,
    const char *name, uint16_t val)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: 0x%02X", name, val);
    ad5593r_putx(di, s, ANN_FIELD, buf);
}

static void ad5593r_decode_field_str(struct srd_decoder_inst *di, ad5593r_state *s,
    const char *name, const char *str)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s", name, str);
    ad5593r_putx(di, s, ANN_FIELD, buf);
}

static void ad5593r_handle_pointer_byte(struct srd_decoder_inst *di, ad5593r_state *s, uint8_t ptr_byte)
{
    uint8_t opcode = (ptr_byte >> 4) & 0x0F;
    uint8_t low_nibble = ptr_byte & 0x0F;

    c_put(di, s->ss, s->es, s->out_ann, ANN_PTR_BYTE, "Pointer Byte", "Ptr Byte");

    /* Determine register from opcode */
    const char *reg_name = NULL;
    if (opcode < 16)
        reg_name = config_mode_bits_map[opcode];

    if (reg_name) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_REGISTER, reg_name);
    }

    /* Decode pointer byte fields based on specific pointer byte type */
    switch (ptr_byte & 0xF0) {
    case 0x00: /* CONFIG_MODE_POINTER */
        ad5593r_decode_field_str(di, s, "CONFIG_MODE_BITS",
            (opcode < 16 && config_mode_bits_map[opcode]) ? config_mode_bits_map[opcode] : "UNKNOWN");
        ad5593r_decode_field_hex(di, s, "CONFIG_MODE_SEL", opcode);
        if (opcode < 16 && config_mode_bits_map[opcode])
            snprintf(s->databyte_register, sizeof(s->databyte_register), "%s", config_mode_bits_map[opcode]);
        else
            s->databyte_register[0] = '\0';
        break;
    case 0x10: /* DAC_WR_POINTER */
        ad5593r_decode_field(di, s, "DAC_CH_SEL_WR", low_nibble);
        ad5593r_decode_field_hex(di, s, "DAC_WR_SEL", opcode);
        snprintf(s->databyte_register, sizeof(s->databyte_register), "DAC_WR");
        break;
    case 0x40: /* ADC_RD_POINTER */
        ad5593r_decode_field_str(di, s, "RESERVED", "");
        ad5593r_decode_field_hex(di, s, "ADC_RD_SEL", opcode);
        snprintf(s->databyte_register, sizeof(s->databyte_register), "ADC_RESULT");
        break;
    case 0x50: /* DAC_RD_POINTER */
        ad5593r_decode_field(di, s, "DAC_CH_SEL_RD", low_nibble);
        ad5593r_decode_field_hex(di, s, "DAC_RD_SEL", opcode);
        snprintf(s->databyte_register, sizeof(s->databyte_register), "DAC_DATA_RD");
        break;
    case 0x60: /* GPIO_RD_POINTER */
        ad5593r_decode_field_str(di, s, "RESERVED", "");
        ad5593r_decode_field_hex(di, s, "GPIO_RD_SEL", opcode);
        if (s->io_operation_type == 0)
            snprintf(s->databyte_register, sizeof(s->databyte_register), "GPIO_INPUT");
        else
            snprintf(s->databyte_register, sizeof(s->databyte_register), "GPIO_OUTPUT");
        break;
    case 0x70: /* REG_RD_POINTER */
        if (low_nibble < 14 && reg_sel_rd_map[low_nibble])
            ad5593r_decode_field_str(di, s, "DAC_CH_SEL_WR", reg_sel_rd_map[low_nibble]);
        else
            ad5593r_decode_field_str(di, s, "DAC_CH_SEL_WR", "UNKNOWN");
        ad5593r_decode_field_hex(di, s, "REG_RD_SEL", opcode);
        if (low_nibble < 14 && reg_sel_rd_map[low_nibble])
            snprintf(s->databyte_register, sizeof(s->databyte_register), "%s", reg_sel_rd_map[low_nibble]);
        else
            s->databyte_register[0] = '\0';
        break;
    default:
        s->databyte_register[0] = '\0';
        break;
    }
}

static void ad5593r_handle_data_bytes(struct srd_decoder_inst *di, ad5593r_state *s, uint16_t data16)
{
    const char *reg = s->databyte_register;
    if (!reg || reg[0] == '\0')
        return;

    c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_REGISTER, reg);

    if (strcmp(reg, "NOOP") == 0) {
        ad5593r_decode_field(di, s, "No operation", data16 & 0x7FF);
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "ADC_SEQ") == 0) {
        ad5593r_decode_field_str(di, s, "ADC channels", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "Temperature Indicator", (data16 & 0x100) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "Repeat", (data16 & 0x200) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "GEN_CTRL_REG") == 0) {
        ad5593r_decode_field_str(di, s, "RESERVED", "");
        ad5593r_decode_field_str(di, s, "DAC_RANGE", (data16 & 0x10) ? "0V to 2xVref" : "0V to Vref");
        ad5593r_decode_field_str(di, s, "ADC_RANGE", (data16 & 0x20) ? "0V to 2xVref" : "0V to Vref");
        ad5593r_decode_field_str(di, s, "ALL_DAC", (data16 & 0x40) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "IO_LOCK", (data16 & 0x80) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "ADC_BUF_EN", (data16 & 0x100) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "ADC_BUF_PRECH", (data16 & 0x200) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "ADC_CONFIG") == 0) {
        ad5593r_decode_field_str(di, s, "ADC input pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "DAC_CONFIG") == 0) {
        ad5593r_decode_field_str(di, s, "DAC output pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "PULLDWN_CONFIG") == 0) {
        ad5593r_decode_field_str(di, s, "Weak-pulldown output pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "LDAC_MODE") == 0) {
        ad5593r_decode_field_hex(di, s, "LDAC_MODE", data16 & 0x3);
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "GPIO_CONFIG") == 0) {
        ad5593r_decode_field_str(di, s, "GPIO output pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "GPIO_OUTPUT") == 0) {
        ad5593r_decode_field_str(di, s, "GPIO high pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "GPIO_INPUT") == 0) {
        ad5593r_decode_field_str(di, s, "GPIO input pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "PD_REF_CTRL") == 0) {
        ad5593r_decode_field_str(di, s, "DAC power-down pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
        ad5593r_decode_field_str(di, s, "EN_REF", (data16 & 0x200) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "PD_ALL", (data16 & 0x400) ? "Enabled" : "Disabled");
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "GPIO_OPENDRAIN_CONFIG") == 0) {
        ad5593r_decode_field_str(di, s, "GPIO open-drain pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "IO_TS_CONFIG") == 0) {
        ad5593r_decode_field_str(di, s, "Three-state output pins", bit_indices_str(data16 & 0xFF));
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "SW_RESET") == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%03X", data16 & 0x7FF);
        ad5593r_decode_field_str(di, s, "Reset command", buf);
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "DAC_WR") == 0) {
        char buf[64];
        double voltage = (data16 & 0xFFF) * s->vref / 4095.0;
        snprintf(buf, sizeof(buf), "DAC data: %u (%.4f V)", data16 & 0xFFF, voltage);
        ad5593r_putb(di, s, ANN_FIELD, buf);
        ad5593r_decode_field_str(di, s, "RESERVED", "");
    } else if (strcmp(reg, "DAC_DATA_RD") == 0) {
        char buf[64];
        double voltage = (data16 & 0xFFF) * s->vref / 4095.0;
        int addr = (data16 >> 12) & 0x7;
        snprintf(buf, sizeof(buf), "DAC_DATA: %u (%.4f V)", data16 & 0xFFF, voltage);
        ad5593r_putb(di, s, ANN_FIELD, buf);
        char buf2[32];
        snprintf(buf2, sizeof(buf2), "DAC_ADDR: DAC%d", addr);
        ad5593r_putb(di, s, ANN_FIELD, buf2);
    } else if (strcmp(reg, "ADC_RESULT") == 0) {
        char buf[64];
        double voltage = (data16 & 0xFFF) * s->vref / 4095.0;
        int addr = (data16 >> 12) & 0x7;
        snprintf(buf, sizeof(buf), "ADC_DATA: %u (%.4f V)", data16 & 0xFFF, voltage);
        ad5593r_putb(di, s, ANN_FIELD, buf);
        char buf2[32];
        snprintf(buf2, sizeof(buf2), "ADC_ADDR: ADC%d", addr);
        ad5593r_putb(di, s, ANN_FIELD, buf2);
    }
}

static void ad5593r_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ad5593r_state *s = (ad5593r_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "BITS") == 0) {
        if (n_fields > 0 && fields[0].bytes.len >= 140) {
            const unsigned char *bdata = (const unsigned char *)fields[0].bytes.data;
            if (bdata[0] == 0x02) {
                uint64_t msb_ss = 0, bit1_es = 0, bit0_es = 0;
                for(int b=0; b<8; b++) {
                    msb_ss  |= ((uint64_t)bdata[5  + 0*17 + b]) << (b*8);
                    bit1_es |= ((uint64_t)bdata[13 + 6*17 + b]) << (b*8);
                    bit0_es |= ((uint64_t)bdata[13 + 7*17 + b]) << (b*8);
                }
                s->bits_ss_msb = msb_ss;
                s->bits_es_bit1 = bit1_es;
                s->bits_es_bit0 = bit0_es;
            }
        }
        return;
    }

    if (strcmp(cmd, "STOP") == 0) {
        s->state = AD5593R_IDLE;
        return;
    }

    switch (s->state) {
    case AD5593R_IDLE:
        if (strcmp(cmd, "START") == 0)
            s->state = AD5593R_GET_SLAVE_ADDR;
        break;
    case AD5593R_GET_SLAVE_ADDR:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            s->ss = s->bits_ss_msb;
            s->es = s->bits_es_bit1;
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr != 0x10 && addr != 0x11) {
                c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING,
                    "I\xC2\xB2" "C slave is not compatible.");
            } else {
                c_put(di, s->ss, s->es, s->out_ann, ANN_SLAVE_ADDR,
                    "I2C Slave address", "I2C Slave");
            }
            s->io_operation_type = 0;
            s->state = AD5593R_GET_POINTER_BYTE;
        } else if (strcmp(cmd, "ADDRESS READ") == 0) {
            s->ss = s->bits_ss_msb;
            s->es = s->bits_es_bit1;
            uint8_t addr = (n_fields > 0) ? fields[0].u8 : 0;
            if (addr != 0x10 && addr != 0x11) {
                c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING,
                    "I\xC2\xB2" "C slave is not compatible.");
            } else {
                c_put(di, s->ss, s->es, s->out_ann, ANN_SLAVE_ADDR,
                    "I2C Slave address", "I2C Slave");
            }
            s->io_operation_type = 1;
            s->state = AD5593R_GET_DATA_HIGH;
        }
        break;
    case AD5593R_GET_POINTER_BYTE:
        if (strcmp(cmd, "DATA WRITE") == 0 || strcmp(cmd, "DATA READ") == 0) {
            s->ss = s->bits_ss_msb;
            s->es = s->bits_es_bit0;
            uint8_t ptr_byte = (n_fields > 0) ? fields[0].u8 : 0;
            ad5593r_handle_pointer_byte(di, s, ptr_byte);
            s->state = AD5593R_GET_DATA_HIGH;
        }
        break;
    case AD5593R_GET_DATA_HIGH:
        if (strcmp(cmd, "DATA WRITE") == 0 || strcmp(cmd, "DATA READ") == 0) {
            s->data_high = (n_fields > 0) ? fields[0].u8 : 0;
            s->ss_block = s->bits_ss_msb;
            s->state = AD5593R_GET_DATA_LOW;
        }
        break;
    case AD5593R_GET_DATA_LOW:
        if (strcmp(cmd, "DATA WRITE") == 0 || strcmp(cmd, "DATA READ") == 0) {
            s->data_low = (n_fields > 0) ? fields[0].u8 : 0;
            s->es_block = s->bits_es_bit0;
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA_BYTE, "Data Bytes");
            uint16_t data16 = ((uint16_t)s->data_high << 8) | s->data_low;
            ad5593r_handle_data_bytes(di, s, data16);
            s->state = AD5593R_GET_DATA_HIGH;
        }
        break;
    }
}

static void ad5593r_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ad5593r_state)));
    }
    ad5593r_state *s = (ad5593r_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ad5593r_state));
    s->state = AD5593R_IDLE;
    s->vref = 2.5;
}

static void ad5593r_start(struct srd_decoder_inst *di)
{
    ad5593r_state *s = (ad5593r_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ad5593r");
    s->vref = c_opt_dbl(di, "Vref", 2.5);
}

static void ad5593r_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ad5593r_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ad5593r_c_decoder = {
    .id = "ad5593r_c",
    .name = "AD5593R(C)",
    .longname = "Analog Devices AD5593R (C)",
    .desc = "Analog Devices AD5593R 12-bit configurable ADC/DAC. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ad5593r_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ad5593r_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ad5593r_ann_rows,
    .inputs = ad5593r_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ad5593r_tags,
    .num_tags = 2,
    .reset = ad5593r_reset,
    .start = ad5593r_start,
    .decode = ad5593r_decode,
    .destroy = ad5593r_destroy,
    .decode_upper = ad5593r_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    ad5593r_options[0].def = g_variant_new_double(2.5);
    return &ad5593r_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}