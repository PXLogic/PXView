#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CMD = 0,
    ANN_REG_WR,
    ANN_REG_RD,
    ANN_TX,
    ANN_RX,
    ANN_RESP,
    ANN_WARN,
    NUM_ANN,
};

#define NRF905_MAX_BYTES 64

typedef struct {
    int out_ann;
    int cs_asserted;
    uint64_t cmd_ss;
    uint64_t cmd_es;

    uint8_t mosi_bytes[NRF905_MAX_BYTES];
    uint64_t mosi_ss[NRF905_MAX_BYTES];
    uint64_t mosi_es[NRF905_MAX_BYTES];
    uint8_t miso_bytes[NRF905_MAX_BYTES];
    uint64_t miso_ss[NRF905_MAX_BYTES];
    uint64_t miso_es[NRF905_MAX_BYTES];
    int num_bytes;
} nrf905_state;

/* Configuration register field definitions */
typedef struct {
    const char *name;
    int stbit;
    int nbits;
    const char *opts[8]; /* max 8 options, NULL-terminated */
} nrf905_reg_field;

static const nrf905_reg_field cfg_reg_0[] = {
    {"CH_NO", 7, 8, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_1[] = {
    {"AUTO_RETRAN", 5, 1, {"No retransmission", "Retransmission of data packet", NULL}},
    {"RX_RED_PWR", 4, 1, {"Normal operation", "Reduced power", NULL}},
    {"PA_PWR", 3, 2, {"-10 dBm", "-2 dBm", "+6 dBm", "+10 dBm", NULL}},
    {"HFREQ_PLL", 1, 1, {"433 MHz", "868 / 915 MHz", NULL}},
    {"CH_NO_8", 0, 1, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_2[] = {
    {"TX_AFW (TX addr width)", 6, 3, {NULL}},
    {"RX_AFW (RX addr width)", 2, 3, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_3[] = {
    {"RW_PW (RX payload width)", 5, 6, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_4[] = {
    {"TX_PW (TX payload width)", 5, 6, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_5[] = {
    {"RX_ADDR_0", 7, 8, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_6[] = {
    {"RX_ADDR_1", 7, 8, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_7[] = {
    {"RX_ADDR_2", 7, 8, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_8[] = {
    {"RX_ADDR_3", 7, 8, {NULL}},
    {NULL}
};

static const nrf905_reg_field cfg_reg_9[] = {
    {"CRC_MODE", 7, 1, {"8 CRC check bit", "16 CRC check bit", NULL}},
    {"CRC_EN", 6, 1, {"Disabled", "Enabled", NULL}},
    {"XOR", 5, 3, {"4 MHz", "8 MHz", "12 MHz", "16 MHz", "20 MHz", NULL}},
    {"UP_CLK_EN", 2, 1, {"No external clock signal avail.", "External clock signal enabled", NULL}},
    {"UP_CLK_FREQ", 1, 2, {"4 MHz", "2 MHz", "1 MHz", "500 kHz", NULL}},
    {NULL}
};

static const nrf905_reg_field chn_cfg[] = {
    {"PA_PWR", 3, 2, {"-10 dBm", "-2 dBm", "+6 dBm", "+10 dBm", NULL}},
    {"HFREQ_PLL", 1, 1, {"433 MHz", "868 / 915 MHz", NULL}},
    {NULL}
};

static const nrf905_reg_field stat_reg[] = {
    {"AM", 7, 1, {NULL}},
    {"DR", 5, 1, {NULL}},
    {NULL}
};

static const nrf905_reg_field *cfg_regs[] = {
    cfg_reg_0, cfg_reg_1, cfg_reg_2, cfg_reg_3, cfg_reg_4,
    cfg_reg_5, cfg_reg_6, cfg_reg_7, cfg_reg_8, cfg_reg_9
};

static const char *nrf905_inputs[] = {"spi", NULL};
static const char *nrf905_tags[] = {"IC", "Wireless/RF", NULL};

static const char *nrf905_ann_labels[][3] = {
    {"", "cmd", "Command sent to the device"},
    {"", "reg-write", "Config register written to the device"},
    {"", "reg-read", "Config register read from the device"},
    {"", "tx-data", "Payload sent to the device"},
    {"", "rx-data", "Payload read from the device"},
    {"", "resp", "Response to commands received from the device"},
    {"", "warning", "Warning"},
};

static const int nrf905_row_commands_classes[] = {ANN_CMD, -1};
static const int nrf905_row_responses_classes[] = {ANN_RESP, -1};
static const int nrf905_row_registers_classes[] = {ANN_REG_WR, ANN_REG_RD, -1};
static const int nrf905_row_tx_classes[] = {ANN_TX, -1};
static const int nrf905_row_rx_classes[] = {ANN_RX, -1};
static const int nrf905_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row nrf905_ann_rows[] = {
    {"commands", "Commands", nrf905_row_commands_classes, 1},
    {"responses", "Responses", nrf905_row_responses_classes, 1},
    {"registers", "Registers", nrf905_row_registers_classes, 2},
    {"tx", "Transmitted data", nrf905_row_tx_classes, 1},
    {"rx", "Received data", nrf905_row_rx_classes, 1},
    {"warnings", "Warnings", nrf905_row_warnings_classes, 1},
};

static int nrf905_extract_bits(uint8_t byte, int start_bit, int num_bits)
{
    int begin = 7 - start_bit;
    int end = begin + num_bits;
    if (begin < 0 || end > 8)
        return 0;
    return (byte >> (8 - end)) & ((1 << num_bits) - 1);
}

static void nrf905_extract_vars(struct srd_decoder_inst *di, nrf905_state *s,
    const nrf905_reg_field *fields, uint8_t reg_value,
    uint64_t ss, uint64_t es, int ann)
{
    char buf[512];
    int pos = 0;
    for (int i = 0; fields[i].name != NULL; i++) {
        int val = nrf905_extract_bits(reg_value, fields[i].stbit, fields[i].nbits);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s = %d", fields[i].name, val);
        if (fields[i].opts[0] != NULL && val < 8 && fields[i].opts[val] != NULL) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " (%s)", fields[i].opts[val]);
        }
        if (fields[i + 1].name != NULL) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | ");
        }
    }
    c_put(di, ss, es, s->out_ann, ann, buf);
}

static void nrf905_parse_config_register(struct srd_decoder_inst *di, nrf905_state *s,
    int addr, uint8_t value, uint64_t ss, uint64_t es, int is_write)
{
    if (addr < 0 || addr > 9) {
        c_put(di, ss, es, s->out_ann, ANN_WARN, "Invalid reg. addr");
        return;
    }
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "CFG_REG[0x%X] -> ", addr);
    char buf[512];
    int pos = snprintf(buf, sizeof(buf), "%s", prefix);
    const nrf905_reg_field *fields = cfg_regs[addr];

    for (int i = 0; fields[i].name != NULL; i++) {
        int val = nrf905_extract_bits(value, fields[i].stbit, fields[i].nbits);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s = %d", fields[i].name, val);
        if (fields[i].opts[0] != NULL && val < 8 && fields[i].opts[val] != NULL) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " (%s)", fields[i].opts[val]);
        }
        if (fields[i + 1].name != NULL) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | ");
        }
    }

    int ann = is_write ? ANN_REG_WR : ANN_REG_RD;
    c_put(di, ss, es, s->out_ann, ann, buf);
}

static void nrf905_dump_cmd_bytes(struct srd_decoder_inst *di, nrf905_state *s,
    const char *prefix, int is_mosi, int ann)
{
    int start = 1; /* skip command byte */
    if (start >= s->num_bytes)
        return;

    char data_str[256];
    int pos = 0;
    uint64_t ss = s->mosi_ss[start];
    uint64_t es = s->mosi_es[s->num_bytes - 1];

    for (int i = start; i < s->num_bytes && pos < (int)sizeof(data_str) - 4; i++) {
        uint8_t b = is_mosi ? s->mosi_bytes[i] : s->miso_bytes[i];
        pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%02X ", b);
        if (is_mosi)
            es = s->mosi_es[i];
        else
            es = s->miso_es[i];
    }

    char long_str[768], short_str[512];
    snprintf(long_str, sizeof(long_str), "%s{$}", prefix);
    snprintf(short_str, sizeof(short_str), "@%s", data_str);
    c_put(di, ss, es, s->out_ann, ann, long_str, short_str);
}

static void nrf905_handle_stat(struct srd_decoder_inst *di, nrf905_state *s)
{
    if (s->num_bytes < 1)
        return;
    nrf905_extract_vars(di, s, stat_reg, s->miso_bytes[0],
                        s->miso_ss[0], s->miso_es[0], ANN_REG_RD);
}

static void nrf905_handle_WC(struct srd_decoder_inst *di, nrf905_state *s)
{
    int start_addr = s->mosi_bytes[0] & 0x0F;
    if (start_addr > 9)
        return;
    for (int i = 1; i < s->num_bytes; i++) {
        int reg_addr = start_addr + (i - 1);
        if (reg_addr <= 9) {
            nrf905_parse_config_register(di, s, reg_addr, s->mosi_bytes[i],
                                         s->mosi_ss[i], s->mosi_es[i], 1);
        }
    }
}

static void nrf905_handle_RC(struct srd_decoder_inst *di, nrf905_state *s)
{
    int start_addr = s->mosi_bytes[0] & 0x0F;
    if (start_addr > 9)
        return;
    for (int i = 1; i < s->num_bytes; i++) {
        int reg_addr = start_addr + (i - 1);
        if (reg_addr <= 9) {
            nrf905_parse_config_register(di, s, reg_addr, s->miso_bytes[i],
                                         s->miso_ss[i], s->miso_es[i], 0);
        }
    }
}

static void nrf905_handle_WTP(struct srd_decoder_inst *di, nrf905_state *s)
{
    nrf905_dump_cmd_bytes(di, s, "Write TX payload.: ", 1, ANN_TX);
}

static void nrf905_handle_RTP(struct srd_decoder_inst *di, nrf905_state *s)
{
    nrf905_dump_cmd_bytes(di, s, "Read TX payload: ", 0, ANN_RESP);
}

static void nrf905_handle_WTA(struct srd_decoder_inst *di, nrf905_state *s)
{
    nrf905_dump_cmd_bytes(di, s, "Write TX addr: ", 1, ANN_REG_WR);
}

static void nrf905_handle_RTA(struct srd_decoder_inst *di, nrf905_state *s)
{
    nrf905_dump_cmd_bytes(di, s, "Read TX addr: ", 0, ANN_RESP);
}

static void nrf905_handle_RRP(struct srd_decoder_inst *di, nrf905_state *s)
{
    nrf905_dump_cmd_bytes(di, s, "Read RX payload: ", 0, ANN_RX);
}

static void nrf905_handle_CC(struct srd_decoder_inst *di, nrf905_state *s)
{
    if (s->num_bytes < 2)
        return;
    uint8_t cmd_byte = s->mosi_bytes[0];
    uint8_t dta = s->mosi_bytes[1];
    int channel = ((cmd_byte & 0x01) << 8) + dta;

    char buf[512];
    int pos = 0;
    /* Extract CHN_CFG fields from command byte */
    for (int i = 0; chn_cfg[i].name != NULL; i++) {
        int val = nrf905_extract_bits(cmd_byte, chn_cfg[i].stbit, chn_cfg[i].nbits);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s = %d", chn_cfg[i].name, val);
        if (chn_cfg[i].opts[0] != NULL && val < 8 && chn_cfg[i].opts[val] != NULL) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " (%s)", chn_cfg[i].opts[val]);
        }
        if (chn_cfg[i + 1].name != NULL) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " | ");
        }
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "| CHN = %d", channel);
    c_put(di, s->mosi_ss[0], s->mosi_es[1], s->out_ann, ANN_REG_WR, buf);
}

static void nrf905_process_cmd(struct srd_decoder_inst *di, nrf905_state *s)
{
    if (s->num_bytes < 1)
        return;

    uint8_t cmd = s->mosi_bytes[0];
    const char *cmd_name = NULL;

    if ((cmd & 0xF0) == 0x00) {
        cmd_name = "CMD: W_CONFIG (WC)";
    } else if ((cmd & 0xF0) == 0x10) {
        cmd_name = "CMD: R_CONFIG (RC)";
    } else if (cmd == 0x20) {
        cmd_name = "CMD: W_TX_PAYLOAD (WTP)";
    } else if (cmd == 0x21) {
        cmd_name = "CMD: R_TX_PAYLOAD (RTP)";
    } else if (cmd == 0x22) {
        cmd_name = "CMD: W_TX_ADDRESS (WTA)";
    } else if (cmd == 0x23) {
        cmd_name = "CMD: R_TX_ADDRESS (RTA)";
    } else if (cmd == 0x24) {
        cmd_name = "CMD: R_RX_PAYLOAD (RRP)";
    } else if ((cmd & 0xF0) == 0x80) {
        cmd_name = "CMD: CHANNEL_CONFIG (CC)";
    }

    /* Report command name */
    if (cmd_name) {
        c_put(di, s->cmd_ss, s->cmd_es, s->out_ann, ANN_CMD, cmd_name);
    }

    /* Handle status byte */
    nrf905_handle_stat(di, s);

    /* Handle command */
    if ((cmd & 0xF0) == 0x00) {
        nrf905_handle_WC(di, s);
    } else if ((cmd & 0xF0) == 0x10) {
        nrf905_handle_RC(di, s);
    } else if (cmd == 0x20) {
        nrf905_handle_WTP(di, s);
    } else if (cmd == 0x21) {
        nrf905_handle_RTP(di, s);
    } else if (cmd == 0x22) {
        nrf905_handle_WTA(di, s);
    } else if (cmd == 0x23) {
        nrf905_handle_RTA(di, s);
    } else if (cmd == 0x24) {
        nrf905_handle_RRP(di, s);
    } else if ((cmd & 0xF0) == 0x80) {
        nrf905_handle_CC(di, s);
    }
}

static void nrf905_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    nrf905_state *s = (nrf905_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        if (n_fields >= 2) {
            int old_val = fields[0].u8;
            int new_val = fields[1].u8;
            if (old_val == 0xFF && new_val == 0) {
                /* First CS assert */
                s->cs_asserted = 1;
                s->cmd_ss = start_sample;
                s->num_bytes = 0;
            } else if (old_val == 1 && new_val == 0) {
                /* CS assert (falling edge) */
                s->cs_asserted = 1;
                s->cmd_ss = start_sample;
                s->num_bytes = 0;
            } else if (old_val == 0 && new_val == 1) {
                /* CS deassert (rising edge) - process command */
                s->cmd_es = start_sample;
                if (s->num_bytes > 0) {
                    nrf905_process_cmd(di, s);
                }
                s->cs_asserted = 0;
                s->num_bytes = 0;
            } else if (old_val == 0xFF && new_val == 1) {
                /* First CS release (no data) */
                s->cs_asserted = 0;
            }
        }
    } else if (strcmp(cmd, "DATA") == 0 && s->cs_asserted && n_fields >= 17) {
        
        
        uint64_t mosi_val = 0, miso_val = 0;
        for (int i = 0; i < 8; i++)
            mosi_val |= ((uint64_t)fields[1 + i].u8 << (8 * i));
        for (int i = 0; i < 8; i++)
            miso_val |= ((uint64_t)fields[9 + i].u8 << (8 * i));

        if (s->num_bytes < NRF905_MAX_BYTES) {
            s->mosi_bytes[s->num_bytes] = (uint8_t)mosi_val;
            s->mosi_ss[s->num_bytes] = start_sample;
            s->mosi_es[s->num_bytes] = end_sample;
            s->miso_bytes[s->num_bytes] = (uint8_t)miso_val;
            s->miso_ss[s->num_bytes] = start_sample;
            s->miso_es[s->num_bytes] = end_sample;
            s->num_bytes++;
        }
    }
}

static void nrf905_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(nrf905_state)));
    }
    nrf905_state *s = (nrf905_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(nrf905_state));
}

static void nrf905_start(struct srd_decoder_inst *di)
{
    nrf905_state *s = (nrf905_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "nrf905");
}

static void nrf905_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void nrf905_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder nrf905_c_decoder = {
    .id = "nrf905_c",
    .name = "nRF905(C)",
    .longname = "Nordic Semiconductor nRF905 (C)",
    .desc = "433/868/933MHz transceiver chip. (C implementation)",
    .license = "mit",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = nrf905_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = nrf905_ann_rows,
    .inputs = nrf905_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = nrf905_tags,
    .num_tags = 2,
    .reset = nrf905_reset,
    .start = nrf905_start,
    .decode = nrf905_decode,
    .destroy = nrf905_destroy,
    .decode_upper = nrf905_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &nrf905_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}