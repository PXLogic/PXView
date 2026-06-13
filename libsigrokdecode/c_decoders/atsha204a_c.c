#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_WADDR = 0,
    ANN_COUNT,
    ANN_OPCODE,
    ANN_PARAM1,
    ANN_PARAM2,
    ANN_DATA,
    ANN_CRC,
    ANN_STATUS,
    ANN_WARNING,
    NUM_ANN,
};

enum atsha204a_state {
    ATSHA204A_IDLE,
    ATSHA204A_GET_SLAVE_ADDR,
    ATSHA204A_READ_REGS,
    ATSHA204A_WRITE_REGS,
};

#define ATSHA204A_MAX_BYTES 256

#define WORD_ADDR_RESET   0x00
#define WORD_ADDR_SLEEP   0x01
#define WORD_ADDR_IDLE    0x02
#define WORD_ADDR_COMMAND 0x03

#define OPCODE_PAUSE      0x01
#define OPCODE_READ       0x02
#define OPCODE_MAC        0x08
#define OPCODE_HMAC       0x11
#define OPCODE_WRITE      0x12
#define OPCODE_GEN_DIG    0x15
#define OPCODE_NONCE      0x16
#define OPCODE_LOCK       0x17
#define OPCODE_RANDOM     0x1b
#define OPCODE_DERIVE_KEY 0x1c
#define OPCODE_UPDATE_EXTRA 0x20
#define OPCODE_COUNTER    0x24
#define OPCODE_CHECK_MAC  0x28
#define OPCODE_DEV_REV    0x30
#define OPCODE_GEN_KEY    0x40
#define OPCODE_SIGN       0x41
#define OPCODE_ECDH       0x43
#define OPCODE_VERIFY     0x45
#define OPCODE_PRIVWRITE  0x46
#define OPCODE_SHA        0x47

typedef struct {
    uint64_t ss;
    uint64_t es;
    uint8_t val;
} atsha204a_byte_entry;

typedef struct {
    enum atsha204a_state state;
    int waddr;
    int opcode;
    
    uint64_t ss_block, es_block;
    uint64_t ss, es;
    atsha204a_byte_entry bytes[ATSHA204A_MAX_BYTES];
    int num_bytes;
    int out_ann;
} atsha204a_state;

static const char *word_addr_str[] = {"RESET", "SLEEP", "IDLE", "COMMAND"};

static const char *opcodes_str[] = {
    NULL, "Pause", "Read", NULL, NULL, NULL, NULL, NULL,
    "MAC", NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, "HMAC", "Write", NULL, NULL, "GenDig", "Nonce", "Lock",
    NULL, NULL, NULL, "Random", "DeriveKey", NULL, NULL, NULL,
    "UpdateExtra", NULL, NULL, NULL, "Counter", NULL, NULL, NULL,
    NULL, "GenKey", "Sign", NULL, "ECDH", NULL, "Verify", "PrivWrite",
    "SHA",
};

static const char *zones_str[] = {"CONFIG", "OTP", "DATA"};

static const char *status_str(uint8_t status)
{
    switch (status) {
    case 0x00: return "Command success";
    case 0x01: return "Checkmac failure";
    case 0x03: return "Parse error";
    case 0x0f: return "Execution error";
    case 0x11: return "Ready";
    case 0xff: return "CRC / communications error";
    default: return "Unknown status";
    }
}

static const char *opcode_name(int op)
{
    if (op >= 0 && op < (int)(sizeof(opcodes_str) / sizeof(opcodes_str[0])) && opcodes_str[op])
        return opcodes_str[op];
    return "Unknown";
}

static const char *atsha204a_inputs[] = {"i2c", NULL};
static const char *atsha204a_tags[] = {"Security/crypto", "IC", "Memory", NULL};

static const char *atsha204a_ann_labels[][3] = {
    {"", "waddr", "Word address"},
    {"", "count", "Count"},
    {"", "opcode", "Opcode"},
    {"", "param1", "Param1"},
    {"", "param2", "Param2"},
    {"", "data", "Data"},
    {"", "crc", "CRC"},
    {"", "status", "Status"},
    {"", "warning", "Warning"},
};

static const int atsha204a_row_frame_classes[] = {ANN_WADDR, ANN_COUNT, ANN_OPCODE, ANN_PARAM1, ANN_PARAM2, ANN_DATA, ANN_CRC, -1};
static const int atsha204a_row_status_classes[] = {ANN_STATUS, -1};
static const int atsha204a_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row atsha204a_ann_rows[] = {
    {"frame", "Frame", atsha204a_row_frame_classes, 7},
    {"status", "Status", atsha204a_row_status_classes, 1},
    {"warnings", "Warnings", atsha204a_row_warnings_classes, 1},
};



static void atsha204a_puty(struct srd_decoder_inst *di, atsha204a_state *s, int start_idx, int end_idx, int cls, const char *text)
{
    if (start_idx < 0 || end_idx < 0 || start_idx >= s->num_bytes || end_idx >= s->num_bytes)
        return;
    c_put(di, s->bytes[start_idx].ss, s->bytes[end_idx].es, s->out_ann, cls, text);
}

static void atsha204a_put_warning(struct srd_decoder_inst *di, atsha204a_state *s, uint64_t ss, uint64_t es, const char *msg)
{
    char buf[512];
    snprintf(buf, sizeof(buf), "Warning: %s", msg);
    c_put(di, ss, es, s->out_ann, ANN_WARNING, buf);
}

static void atsha204a_put_data_hex(struct srd_decoder_inst *di, atsha204a_state *s,
    int start_idx, int end_idx, const char *label)
{
    if (start_idx < 0 || end_idx < 0 || start_idx >= s->num_bytes || end_idx >= s->num_bytes)
        return;
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s: ", label);
    for (int i = start_idx; i <= end_idx && pos < (int)sizeof(buf) - 10; i++)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", s->bytes[i].val);
    c_put(di, s->bytes[start_idx].ss, s->bytes[end_idx].es, s->out_ann, ANN_DATA, buf);
}

static void atsha204a_output_tx_bytes(struct srd_decoder_inst *di, atsha204a_state *s)
{
    int n = s->num_bytes;
    if (n < 1)
        return;

    /* Word address */
    s->waddr = s->bytes[0].val;
    char buf[256];
    if (s->waddr >= 0 && s->waddr <= 3) {
        snprintf(buf, sizeof(buf), "Word addr: %s", word_addr_str[s->waddr]);
    } else {
        snprintf(buf, sizeof(buf), "Word addr: 0x%02X", s->waddr);
    }
    c_put(di, s->bytes[0].ss, s->bytes[0].es, s->out_ann, ANN_WADDR, buf);

    if (s->waddr != WORD_ADDR_COMMAND)
        return;

    if (n < 2) return;

    /* Count */
    int count = s->bytes[1].val;
    snprintf(buf, sizeof(buf), "Count: %d", count);
    c_put(di, s->bytes[1].ss, s->bytes[1].es, s->out_ann, ANN_COUNT, buf);

    if (n - 1 != count) {
        snprintf(buf, sizeof(buf), "Invalid frame length: Got %d, expecting %d", n - 1, count);
        atsha204a_put_warning(di, s, s->bytes[0].ss, s->bytes[n - 1].es, buf);
        return;
    }

    if (n < 3) return;

    /* Opcode */
    s->opcode = s->bytes[2].val;
    snprintf(buf, sizeof(buf), "Opcode: %s", opcode_name(s->opcode));
    c_put(di, s->bytes[2].ss, s->bytes[2].es, s->out_ann, ANN_OPCODE, buf);

    if (n < 4) return;

    /* Param1 */
    int p1 = s->bytes[3].val;
    int op = s->opcode;
    if (op == OPCODE_CHECK_MAC || op == OPCODE_COUNTER || op == OPCODE_DEV_REV ||
        op == OPCODE_ECDH || op == OPCODE_GEN_KEY || op == OPCODE_HMAC ||
        op == OPCODE_MAC || op == OPCODE_NONCE || op == OPCODE_RANDOM ||
        op == OPCODE_SHA || op == OPCODE_SIGN || op == OPCODE_VERIFY) {
        snprintf(buf, sizeof(buf), "Mode: %02X", p1);
    } else if (op == OPCODE_DERIVE_KEY) {
        snprintf(buf, sizeof(buf), "Random: %s", p1 ? "Yes" : "No");
    } else if (op == OPCODE_PRIVWRITE) {
        snprintf(buf, sizeof(buf), "Encrypted: %s", (p1 & 0x40) ? "Yes" : "No");
    } else if (op == OPCODE_GEN_DIG) {
        snprintf(buf, sizeof(buf), "Zone: %s", (p1 < 3) ? zones_str[p1] : "Unknown");
    } else if (op == OPCODE_LOCK) {
        snprintf(buf, sizeof(buf), "Zone: %s, Summary: %s",
                 (p1 & 0x01) ? "DATA/OTP" : "CONFIG",
                 (p1 & 0x80) ? "Ignored" : "Used");
    } else if (op == OPCODE_PAUSE) {
        snprintf(buf, sizeof(buf), "Selector: %02X", p1);
    } else if (op == OPCODE_READ) {
        snprintf(buf, sizeof(buf), "Zone: %s, Length: %s",
                 (p1 & 0x03) < 3 ? zones_str[p1 & 0x03] : "Unknown",
                 (p1 & 0x80) ? "32 bytes" : "4 bytes");
    } else if (op == OPCODE_WRITE) {
        snprintf(buf, sizeof(buf), "Zone: %s, Encrypted: %s, Length: %s",
                 (p1 & 0x03) < 3 ? zones_str[p1 & 0x03] : "Unknown",
                 (p1 & 0x40) ? "Yes" : "No",
                 (p1 & 0x80) ? "32 bytes" : "4 bytes");
    } else {
        snprintf(buf, sizeof(buf), "Param1: %02X", p1);
    }
    c_put(di, s->bytes[3].ss, s->bytes[3].es, s->out_ann, ANN_PARAM1, buf);

    if (n < 6) return;

    /* Param2 */
    if (op == OPCODE_DERIVE_KEY) {
        snprintf(buf, sizeof(buf), "TargetKey: %02x %02x", s->bytes[5].val, s->bytes[4].val);
    } else if (op == OPCODE_COUNTER || op == OPCODE_ECDH || op == OPCODE_GEN_KEY ||
               op == OPCODE_PRIVWRITE || op == OPCODE_SIGN || op == OPCODE_VERIFY) {
        snprintf(buf, sizeof(buf), "KeyID: %02x %02x", s->bytes[5].val, s->bytes[4].val);
    } else if (op == OPCODE_NONCE || op == OPCODE_PAUSE || op == OPCODE_RANDOM) {
        snprintf(buf, sizeof(buf), "Zero: %02x %02x", s->bytes[5].val, s->bytes[4].val);
    } else if (op == OPCODE_HMAC || op == OPCODE_MAC || op == OPCODE_CHECK_MAC || op == OPCODE_GEN_DIG) {
        snprintf(buf, sizeof(buf), "SlotID: %02x %02x", s->bytes[5].val, s->bytes[4].val);
    } else if (op == OPCODE_LOCK) {
        snprintf(buf, sizeof(buf), "Summary: %02x %02x", s->bytes[5].val, s->bytes[4].val);
    } else if (op == OPCODE_READ || op == OPCODE_WRITE) {
        snprintf(buf, sizeof(buf), "Address: %02x %02x", s->bytes[5].val, s->bytes[4].val);
    } else if (op == OPCODE_UPDATE_EXTRA) {
        snprintf(buf, sizeof(buf), "NewValue: %02x", s->bytes[4].val);
    } else {
        snprintf(buf, sizeof(buf), "-");
    }
    atsha204a_puty(di, s, 4, 5, ANN_PARAM2, buf);

    /* Data (bytes 6 to n-3) */
    if (n > 7) {
        int data_start = 6;
        int data_end = n - 3;
        if (op == OPCODE_CHECK_MAC && data_end >= data_start + 76) {
            atsha204a_put_data_hex(di, s, data_start, data_start + 31, "ClientChal");
            atsha204a_put_data_hex(di, s, data_start + 32, data_start + 63, "ClientResp");
            atsha204a_put_data_hex(di, s, data_start + 64, data_start + 76, "OtherData");
        } else if (op == OPCODE_DERIVE_KEY) {
            atsha204a_put_data_hex(di, s, data_start, data_end, "MAC");
        } else if (op == OPCODE_ECDH && data_end >= data_start + 63) {
            atsha204a_put_data_hex(di, s, data_start, data_start + 31, "Pub X");
            atsha204a_put_data_hex(di, s, data_start + 32, data_start + 63, "Pub Y");
        } else if (op == OPCODE_GEN_DIG || op == OPCODE_GEN_KEY) {
            atsha204a_put_data_hex(di, s, data_start, data_end, "OtherData");
        } else if (op == OPCODE_MAC) {
            atsha204a_put_data_hex(di, s, data_start, data_end, "Challenge");
        } else if (op == OPCODE_PRIVWRITE) {
            if (data_end - data_start + 1 > 36) {
                atsha204a_put_data_hex(di, s, data_start, data_end - 32, "Value");
                atsha204a_put_data_hex(di, s, data_end - 31, data_end, "MAC");
            } else {
                atsha204a_put_data_hex(di, s, data_start, data_end, "Value");
            }
        } else if (op == OPCODE_VERIFY) {
            if (data_end >= data_start + 63) {
                atsha204a_put_data_hex(di, s, data_start, data_start + 31, "ECDSA R");
                atsha204a_put_data_hex(di, s, data_start + 32, data_start + 63, "ECDSA S");
                if (data_end >= data_start + 82) {
                    atsha204a_put_data_hex(di, s, data_start + 64, data_start + 82, "OtherData");
                }
                if (data_end >= data_start + 127) {
                    atsha204a_put_data_hex(di, s, data_start + 64, data_start + 95, "Pub X");
                    atsha204a_put_data_hex(di, s, data_start + 96, data_start + 127, "Pub Y");
                }
            }
        } else if (op == OPCODE_WRITE) {
            if (data_end - data_start + 1 > 32) {
                atsha204a_put_data_hex(di, s, data_start, data_end - 32, "Value");
                atsha204a_put_data_hex(di, s, data_end - 31, data_end, "MAC");
            } else {
                atsha204a_put_data_hex(di, s, data_start, data_end, "Value");
            }
        } else {
            atsha204a_put_data_hex(di, s, data_start, data_end, "Data");
        }
    }

    /* CRC */
    if (n >= 2) {
        snprintf(buf, sizeof(buf), "CRC: %02X %02X", s->bytes[n - 2].val, s->bytes[n - 1].val);
        atsha204a_puty(di, s, n - 2, n - 1, ANN_CRC, buf);
    }
}

static void atsha204a_output_rx_bytes(struct srd_decoder_inst *di, atsha204a_state *s)
{
    int n = s->num_bytes;
    if (n < 1)
        return;

    char buf[256];

    /* Count */
    int count = s->bytes[0].val;
    snprintf(buf, sizeof(buf), "Count: %d", count);
    c_put(di, s->bytes[0].ss, s->bytes[0].es, s->out_ann, ANN_COUNT, buf);

    if (s->waddr == WORD_ADDR_RESET) {
        /* Response to reset */
        if (n >= 2) {
            atsha204a_put_data_hex(di, s, 1, 1, "Data");
            if (n >= 4) {
                snprintf(buf, sizeof(buf), "CRC: %02X %02X", s->bytes[2].val, s->bytes[3].val);
                atsha204a_puty(di, s, 2, 3, ANN_CRC, buf);
                /* Status */
                c_put(di, s->bytes[0].ss, s->bytes[n - 1].es, s->out_ann, ANN_STATUS,
                    status_str(s->bytes[1].val));
            }
        }
    } else if (s->waddr == WORD_ADDR_COMMAND) {
        if (count == 4) {
            /* Status / Error response */
            if (n >= 2) {
                atsha204a_put_data_hex(di, s, 1, 1, "Data");
                if (n >= 4) {
                    snprintf(buf, sizeof(buf), "CRC: %02X %02X", s->bytes[2].val, s->bytes[3].val);
                    atsha204a_puty(di, s, 2, 3, ANN_CRC, buf);
                    c_put(di, s->bytes[0].ss, s->bytes[n - 1].es, s->out_ann, ANN_STATUS,
                        status_str(s->bytes[1].val));
                }
            }
        } else {
            /* Data response */
            if (n >= 3) {
                atsha204a_put_data_hex(di, s, 1, n - 3, "Data");
                snprintf(buf, sizeof(buf), "CRC: %02X %02X", s->bytes[n - 2].val, s->bytes[n - 1].val);
                atsha204a_puty(di, s, n - 2, n - 1, ANN_CRC, buf);
            }
        }
    }
}

static void atsha204a_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    atsha204a_state *s = (atsha204a_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "BITS") == 0)
        return;

    switch (s->state) {
    case ATSHA204A_IDLE:
        if (strcmp(cmd, "START") == 0) {
            s->state = ATSHA204A_GET_SLAVE_ADDR;
            s->ss_block = start_sample;
            s->num_bytes = 0;
        }
        break;
    case ATSHA204A_GET_SLAVE_ADDR:
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            s->state = ATSHA204A_READ_REGS;
            s->num_bytes = 0;
        } else if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            s->state = ATSHA204A_WRITE_REGS;
            s->num_bytes = 0;
        }
        break;
    case ATSHA204A_READ_REGS:
        if (strcmp(cmd, "DATA READ") == 0) {
            uint8_t b = (n_fields > 0) ? fields[0].u8 : 0;
            if (s->num_bytes < ATSHA204A_MAX_BYTES) {
                s->bytes[s->num_bytes].ss = start_sample;
                s->bytes[s->num_bytes].es = end_sample;
                s->bytes[s->num_bytes].val = b;
                s->num_bytes++;
            }
        } else if (strcmp(cmd, "STOP") == 0) {
            s->es_block = end_sample;
            s->opcode = -1;
            if (s->num_bytes > 0)
                atsha204a_output_rx_bytes(di, s);
            s->num_bytes = 0;
            s->waddr = -1;
            s->state = ATSHA204A_IDLE;
        }
        break;
    case ATSHA204A_WRITE_REGS:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            uint8_t b = (n_fields > 0) ? fields[0].u8 : 0;
            if (s->num_bytes < ATSHA204A_MAX_BYTES) {
                s->bytes[s->num_bytes].ss = start_sample;
                s->bytes[s->num_bytes].es = end_sample;
                s->bytes[s->num_bytes].val = b;
                s->num_bytes++;
            }
        } else if (strcmp(cmd, "STOP") == 0) {
            s->es_block = end_sample;
            atsha204a_output_tx_bytes(di, s);
            s->num_bytes = 0;
            s->state = ATSHA204A_IDLE;
        }
        break;
    }

    if (strcmp(cmd, "STOP") == 0) {
        s->state = ATSHA204A_IDLE;
    }
}

static void atsha204a_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(atsha204a_state)));
    }
    atsha204a_state *s = (atsha204a_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(atsha204a_state));
    s->state = ATSHA204A_IDLE;
    s->waddr = -1;
    s->opcode = -1;
}

static void atsha204a_start(struct srd_decoder_inst *di)
{
    atsha204a_state *s = (atsha204a_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "atsha204a");
}

static void atsha204a_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void atsha204a_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder atsha204a_c_decoder = {
    .id = "atsha204a_c",
    .name = "ATSHA204A(C)",
    .longname = "Microchip ATSHA204A (C)",
    .desc = "Microchip ATSHA204A family crypto authentication protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = atsha204a_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = atsha204a_ann_rows,
    .inputs = atsha204a_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = atsha204a_tags,
    .num_tags = 3,
    .reset = atsha204a_reset,
    .start = atsha204a_start,
    .decode = atsha204a_decode,
    .destroy = atsha204a_destroy,
    .decode_upper = atsha204a_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &atsha204a_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}