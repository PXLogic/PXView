#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CMD = 0,
    ANN_TX,
    ANN_REG,
    ANN_RX,
    ANN_WARN,
    NUM_ANN,
};

#define NRF24_MAX_CMD_BYTES 64

typedef struct {
    int out_ann;
    int chip_type; /* 0 = nrf24l01, 1 = xn297 */

    int first;
    int cs_was_released;
    char cmd[32];
    int dat;
    int min_bytes;
    int max_bytes;

    uint8_t mosi_bytes[NRF24_MAX_CMD_BYTES];
    uint8_t miso_bytes[NRF24_MAX_CMD_BYTES];
    int num_bytes;
    uint64_t mb_ss;
    uint64_t mb_es;
    uint64_t cmd_ss;
    uint64_t cmd_es;
} nrf24l01_state;

static const char *nrf24l01_inputs[] = {"spi", NULL};
static const char *nrf24l01_tags[] = {"IC", "Wireless/RF", NULL};

static struct srd_decoder_option nrf24l01_options[] = {
    {"chip", "dec_nrf24l01_opt_chip", "Chip type", NULL, NULL},
};

static const char *nrf24l01_ann_labels[][3] = {
    {"", "cmd", "Commands sent to the device"},
    {"", "tx-data", "Payload sent to the device"},
    {"", "register", "Registers read from the device"},
    {"", "rx-data", "Payload read from the device"},
    {"", "warning", "Warnings"},
};

static const int nrf24l01_row_commands_classes[] = {ANN_CMD, ANN_TX, -1};
static const int nrf24l01_row_responses_classes[] = {ANN_REG, ANN_RX, -1};
static const int nrf24l01_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row nrf24l01_ann_rows[] = {
    {"commands", "Commands", nrf24l01_row_commands_classes, 2},
    {"responses", "Responses", nrf24l01_row_responses_classes, 2},
    {"warnings", "Warnings", nrf24l01_row_warnings_classes, 1},
};

/* Register table */
typedef struct {
    uint8_t addr;
    const char *name;
    int size;
} nrf24l01_reg_entry;

static const nrf24l01_reg_entry nrf24l01_regs[] = {
    {0x00, "CONFIG", 1}, {0x01, "EN_AA", 1}, {0x02, "EN_RXADDR", 1},
    {0x03, "SETUP_AW", 1}, {0x04, "SETUP_RETR", 1}, {0x05, "RF_CH", 1},
    {0x06, "RF_SETUP", 1}, {0x07, "STATUS", 1}, {0x08, "OBSERVE_TX", 1},
    {0x09, "RPD", 1}, {0x0a, "RX_ADDR_P0", 5}, {0x0b, "RX_ADDR_P1", 5},
    {0x0c, "RX_ADDR_P2", 1}, {0x0d, "RX_ADDR_P3", 1}, {0x0e, "RX_ADDR_P4", 1},
    {0x0f, "RX_ADDR_P5", 1}, {0x10, "TX_ADDR", 5}, {0x11, "RX_PW_P0", 1},
    {0x12, "RX_PW_P1", 1}, {0x13, "RX_PW_P2", 1}, {0x14, "RX_PW_P3", 1},
    {0x15, "RX_PW_P4", 1}, {0x16, "RX_PW_P5", 1}, {0x17, "FIFO_STATUS", 1},
    {0x1c, "DYNPD", 1}, {0x1d, "FEATURE", 1},
    {0xFF, NULL, 0} /* sentinel */
};

static const nrf24l01_reg_entry xn297_regs[] = {
    {0x19, "DEMOD_CAL", 1}, {0x1a, "RF_CAL2", 6}, {0x1b, "DEM_CAL2", 3},
    {0x1e, "RF_CAL", 3}, {0x1f, "BB_CAL", 5},
    {0xFF, NULL, 0} /* sentinel */
};

static int nrf24l01_get_reg_size(nrf24l01_state *s, uint8_t addr)
{
    for (int i = 0; nrf24l01_regs[i].name != NULL; i++) {
        if (nrf24l01_regs[i].addr == addr)
            return nrf24l01_regs[i].size;
    }
    if (s->chip_type == 1) {
        for (int i = 0; xn297_regs[i].name != NULL; i++) {
            if (xn297_regs[i].addr == addr)
                return xn297_regs[i].size;
        }
    }
    return 1;
}

static const char *nrf24l01_get_reg_name(nrf24l01_state *s, uint8_t addr)
{
    for (int i = 0; nrf24l01_regs[i].name != NULL; i++) {
        if (nrf24l01_regs[i].addr == addr)
            return nrf24l01_regs[i].name;
    }
    if (s->chip_type == 1) {
        for (int i = 0; xn297_regs[i].name != NULL; i++) {
            if (xn297_regs[i].addr == addr)
                return xn297_regs[i].name;
        }
    }
    return "unknown register";
}

static void nrf24l01_next(nrf24l01_state *s)
{
    s->first = 1;
    s->cmd[0] = '\0';
    s->dat = -1;
    s->min_bytes = 0;
    s->max_bytes = 0;
    s->num_bytes = 0;
    s->mb_ss = 0;
    s->mb_es = 0;
}

static int nrf24l01_parse_command(nrf24l01_state *s, uint8_t b)
{
    int buflen = (s->chip_type == 1) ? 64 : 32;

    if ((b & 0xe0) == 0x00) {
        snprintf(s->cmd, sizeof(s->cmd), "R_REGISTER");
        s->dat = b & 0x1f;
        int m = nrf24l01_get_reg_size(s, s->dat);
        s->min_bytes = 1;
        s->max_bytes = m;
        return 0;
    }
    if ((b & 0xe0) == 0x20) {
        snprintf(s->cmd, sizeof(s->cmd), "W_REGISTER");
        s->dat = b & 0x1f;
        int m = nrf24l01_get_reg_size(s, s->dat);
        s->min_bytes = 1;
        s->max_bytes = m;
        return 0;
    }
    if (b == 0x50) {
        snprintf(s->cmd, sizeof(s->cmd), "ACTIVATE");
        s->dat = -1; s->min_bytes = 1; s->max_bytes = 1;
        return 0;
    }
    if (b == 0x61) {
        snprintf(s->cmd, sizeof(s->cmd), "R_RX_PAYLOAD");
        s->dat = -1; s->min_bytes = 1; s->max_bytes = buflen;
        return 0;
    }
    if (b == 0x60) {
        snprintf(s->cmd, sizeof(s->cmd), "R_RX_PL_WID");
        s->dat = -1; s->min_bytes = 1; s->max_bytes = 1;
        return 0;
    }
    if (b == 0xA0) {
        snprintf(s->cmd, sizeof(s->cmd), "W_TX_PAYLOAD");
        s->dat = -1; s->min_bytes = 1; s->max_bytes = buflen;
        return 0;
    }
    if (b == 0xB0) {
        snprintf(s->cmd, sizeof(s->cmd), "W_TX_PAYLOAD_NOACK");
        s->dat = -1; s->min_bytes = 1; s->max_bytes = buflen;
        return 0;
    }
    if ((b & 0xF8) == 0xA8) {
        snprintf(s->cmd, sizeof(s->cmd), "W_ACK_PAYLOAD");
        s->dat = b & 0x07; s->min_bytes = 1; s->max_bytes = buflen;
        return 0;
    }
    if (b == 0xE1) {
        snprintf(s->cmd, sizeof(s->cmd), "FLUSH_TX");
        s->dat = -1; s->min_bytes = 0; s->max_bytes = 0;
        return 0;
    }
    if (b == 0xE2) {
        snprintf(s->cmd, sizeof(s->cmd), "FLUSH_RX");
        s->dat = -1; s->min_bytes = 0; s->max_bytes = 0;
        return 0;
    }
    if (b == 0xE3) {
        snprintf(s->cmd, sizeof(s->cmd), "REUSE_TX_PL");
        s->dat = -1; s->min_bytes = 0; s->max_bytes = 0;
        return 0;
    }
    if (b == 0xFF) {
        snprintf(s->cmd, sizeof(s->cmd), "NOP");
        s->dat = -1; s->min_bytes = 0; s->max_bytes = 0;
        return 0;
    }

    /* xn297 specific commands */
    if (s->chip_type == 1) {
        if (b == 0xFD) {
            snprintf(s->cmd, sizeof(s->cmd), "CE_FSPI_ON");
            s->dat = -1; s->min_bytes = 1; s->max_bytes = 1;
            return 0;
        }
        if (b == 0xFC) {
            snprintf(s->cmd, sizeof(s->cmd), "CE_FSPI_OFF");
            s->dat = -1; s->min_bytes = 1; s->max_bytes = 1;
            return 0;
        }
        if (b == 0x53) {
            snprintf(s->cmd, sizeof(s->cmd), "RST_FSPI");
            s->dat = -1; s->min_bytes = 1; s->max_bytes = 1;
            return 0;
        }
    }

    return -1;
}

static void nrf24l01_format_command(nrf24l01_state *s, char *buf, int bufsize)
{
    if (strcmp(s->cmd, "R_REGISTER") == 0) {
        const char *reg = nrf24l01_get_reg_name(s, s->dat);
        snprintf(buf, bufsize, "Cmd R_REGISTER \"%s\"", reg);
    } else {
        snprintf(buf, bufsize, "Cmd %s", s->cmd);
    }
}

static void nrf24l01_decode_mb_data(struct srd_decoder_inst *di, nrf24l01_state *s,
    uint64_t ss, uint64_t es, int ann, const uint8_t *data, int len,
    const char *label, int always_hex)
{
    char data_str[512];
    int pos = 0;
    for (int i = 0; i < len && pos < (int)sizeof(data_str) - 4; i++) {
        if (always_hex) {
            pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%02X", data[i]);
        } else {
            if (data[i] >= 32 && data[i] <= 126) {
                pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%c", data[i]);
            } else {
                pos += snprintf(data_str + pos, sizeof(data_str) - pos, "\\x%02X", data[i]);
            }
        }
    }

    char long_str[1024], short_str[768];
    snprintf(long_str, sizeof(long_str), "%s = \"{$}\"", label);
    snprintf(short_str, sizeof(short_str), "@%s", data_str);
    c_put(di, ss, es, s->out_ann, ann, long_str, short_str);
}

static void nrf24l01_decode_register(struct srd_decoder_inst *di, nrf24l01_state *s,
    uint64_t ss, uint64_t es, int ann, int regid, const uint8_t *data, int n_fields)
{
    char label[256];
    if (regid >= 0) {
        const char *name = nrf24l01_get_reg_name(s, (uint8_t)regid);
        if (strcmp(s->cmd, "W_REGISTER") == 0 && ann == ANN_CMD) {
            char cmd_str[128];
            nrf24l01_format_command(s, cmd_str, sizeof(cmd_str));
            snprintf(label, sizeof(label), "%s: %s", cmd_str, name);
        } else {
            snprintf(label, sizeof(label), "Reg %s", name);
        }
    } else {
        /* STATUS register */
        snprintf(label, sizeof(label), "Reg STATUS");
    }

    /* Multi byte registers come LSByte first, reverse for display */
    uint8_t rev[NRF24_MAX_CMD_BYTES];
    for (int i = 0; i < n_fields && i < NRF24_MAX_CMD_BYTES; i++)
        rev[i] = data[n_fields - 1 - i];

    nrf24l01_decode_mb_data(di, s, ss, es, ann, rev, n_fields, label, 1);
}

static void nrf24l01_finish_command(struct srd_decoder_inst *di, nrf24l01_state *s)
{
    uint64_t ss = s->mb_ss;
    uint64_t es = s->mb_es;

    if (strcmp(s->cmd, "R_REGISTER") == 0) {
        nrf24l01_decode_register(di, s, ss, es, ANN_REG, s->dat, s->miso_bytes, s->num_bytes);
    } else if (strcmp(s->cmd, "W_REGISTER") == 0) {
        nrf24l01_decode_register(di, s, ss, es, ANN_CMD, s->dat, s->mosi_bytes, s->num_bytes);
    } else if (strcmp(s->cmd, "R_RX_PAYLOAD") == 0) {
        uint8_t rev[NRF24_MAX_CMD_BYTES];
        for (int i = 0; i < s->num_bytes && i < NRF24_MAX_CMD_BYTES; i++)
            rev[i] = s->miso_bytes[s->num_bytes - 1 - i];
        nrf24l01_decode_mb_data(di, s, ss, es, ANN_RX, rev, s->num_bytes, "RX payload", 1);
    } else if (strcmp(s->cmd, "W_TX_PAYLOAD") == 0 || strcmp(s->cmd, "W_TX_PAYLOAD_NOACK") == 0) {
        uint8_t rev[NRF24_MAX_CMD_BYTES];
        for (int i = 0; i < s->num_bytes && i < NRF24_MAX_CMD_BYTES; i++)
            rev[i] = s->mosi_bytes[s->num_bytes - 1 - i];
        nrf24l01_decode_mb_data(di, s, ss, es, ANN_TX, rev, s->num_bytes, "TX payload", 1);
    } else if (strcmp(s->cmd, "W_ACK_PAYLOAD") == 0) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "ACK payload for pipe %d", s->dat);
        uint8_t rev[NRF24_MAX_CMD_BYTES];
        for (int i = 0; i < s->num_bytes && i < NRF24_MAX_CMD_BYTES; i++)
            rev[i] = s->mosi_bytes[s->num_bytes - 1 - i];
        nrf24l01_decode_mb_data(di, s, ss, es, ANN_TX, rev, s->num_bytes, lbl, 1);
    } else if (strcmp(s->cmd, "R_RX_PL_WID") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Payload width = %d", s->miso_bytes[0]);
        c_put(di, ss, es, s->out_ann, ANN_REG, buf);
    } else if (strcmp(s->cmd, "ACTIVATE") == 0) {
        if (s->mosi_bytes[0] == 0x8c)
            snprintf(s->cmd, sizeof(s->cmd), "DEACTIVATE");
        else if (s->mosi_bytes[0] != 0x73)
            c_put(di, ss, es, s->out_ann, ANN_WARN, "wrong data for \"ACTIVATE\" command");
        char buf[256];
        nrf24l01_format_command(s, buf, sizeof(buf));
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_CMD, buf);
    } else if (strcmp(s->cmd, "RST_FSPI") == 0) {
        if (s->mosi_bytes[0] == 0x5a)
            snprintf(s->cmd, sizeof(s->cmd), "RST_FSPI_HOLD");
        else if (s->mosi_bytes[0] == 0xa5)
            snprintf(s->cmd, sizeof(s->cmd), "RST_FSPI_RELS");
        else
            c_put(di, ss, es, s->out_ann, ANN_WARN, "wrong data for \"RST_FSPI\" command");
        char buf[256];
        nrf24l01_format_command(s, buf, sizeof(buf));
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_CMD, buf);
    }
}

static void nrf24l01_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    nrf24l01_state *s = (nrf24l01_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        if (n_fields >= 2) {
            int old_val = fields[0].u8;
            int new_val = fields[1].u8;
            if (old_val == 0xFF && new_val == 1) {
                /* First CS release */
                s->cs_was_released = 1;
            } else if (old_val == 0 && new_val == 1) {
                /* CS rising edge (deassert) - process command */
                if (s->cmd[0] != '\0') {
                    if (s->num_bytes < s->min_bytes) {
                        c_put(di, start_sample, start_sample, s->out_ann, ANN_WARN,
                                  "missing data bytes");
                    } else if (s->num_bytes > 0) {
                        nrf24l01_finish_command(di, s);
                    }
                }
                nrf24l01_next(s);
                s->cs_was_released = 1;
            }
        }
    } else if (strcmp(cmd, "TRANSFER") == 0) {
        if (s->cmd[0] != '\0') {
            if (s->num_bytes < s->min_bytes) {
                c_put(di, start_sample, start_sample, s->out_ann, ANN_WARN,
                          "missing data bytes");
            } else if (s->num_bytes > 0) {
                nrf24l01_finish_command(di, s);
            }
        }
        nrf24l01_next(s);
        s->cs_was_released = 1;
    } else if (strcmp(cmd, "DATA") == 0 && s->cs_was_released && n_fields >= 17) {
        
        
        uint64_t mosi_val = 0, miso_val = 0;
        for (int i = 0; i < 8; i++)
            mosi_val |= ((uint64_t)fields[1 + i].u8 << (8 * i));
        for (int i = 0; i < 8; i++)
            miso_val |= ((uint64_t)fields[9 + i].u8 << (8 * i));
        uint8_t mosi = (uint8_t)mosi_val;
        uint8_t miso = (uint8_t)miso_val;

        if (s->first) {
            s->first = 0;
            /* First MOSI byte is the command */
            int ret = nrf24l01_parse_command(s, mosi);
            if (ret < 0) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN,
                          "unknown command");
                s->cmd[0] = '\0';
            } else {
                s->cmd_ss = start_sample;
                s->cmd_es = end_sample;
                if (strcmp(s->cmd, "W_REGISTER") == 0 ||
                    strcmp(s->cmd, "ACTIVATE") == 0 ||
                    strcmp(s->cmd, "RST_FSPI") == 0) {
                    s->mb_ss = start_sample;
                } else {
                    char buf[256];
                    nrf24l01_format_command(s, buf, sizeof(buf));
                    c_put(di, start_sample, end_sample, s->out_ann, ANN_CMD, buf);
                }
            }
            /* First MISO byte is always STATUS */
            nrf24l01_decode_register(di, s, start_sample, end_sample, ANN_REG, -1, &miso, 1);
        } else {
            if (s->cmd[0] == '\0' || s->num_bytes >= s->max_bytes) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN,
                          "excess byte");
            } else {
                if (s->num_bytes == 0 || s->mb_ss == 0)
                    s->mb_ss = start_sample;
                s->mb_es = end_sample;
                if (s->num_bytes < NRF24_MAX_CMD_BYTES) {
                    s->mosi_bytes[s->num_bytes] = mosi;
                    s->miso_bytes[s->num_bytes] = miso;
                    s->num_bytes++;
                }
            }
        }
    }
}

static void nrf24l01_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(nrf24l01_state)));
    }
    nrf24l01_state *s = (nrf24l01_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(nrf24l01_state));
    nrf24l01_next(s);
    s->cs_was_released = 0;
}

static void nrf24l01_start(struct srd_decoder_inst *di)
{
    nrf24l01_state *s = (nrf24l01_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "nrf24l01");

    const char *chip = c_opt_str(di, "chip", "nrf24l01");
    s->chip_type = (chip && strcmp(chip, "xn297") == 0) ? 1 : 0;
}

static void nrf24l01_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void nrf24l01_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder nrf24l01_c_decoder = {
    .id = "nrf24l01_c",
    .name = "nRF24L01(+)(C)",
    .longname = "Nordic Semiconductor nRF24L01(+) (C)",
    .desc = "2.4GHz RF transceiver chip. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = nrf24l01_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = nrf24l01_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = nrf24l01_ann_rows,
    .inputs = nrf24l01_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = nrf24l01_tags,
    .num_tags = 2,
    .reset = nrf24l01_reset,
    .start = nrf24l01_start,
    .decode = nrf24l01_decode,
    .destroy = nrf24l01_destroy,
    .decode_upper = nrf24l01_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    nrf24l01_options[0].def = g_variant_new_string("nrf24l01");
    GSList *chip_vals = NULL;
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("nrf24l01"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("xn297"));
    nrf24l01_options[0].values = chip_vals;

    return &nrf24l01_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}