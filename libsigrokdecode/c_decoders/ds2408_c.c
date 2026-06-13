#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* DS2408 8-channel addressable switch decoder */

enum {
    ANN_RESET_PRESENCE = 0,
    ANN_ROM,
    ANN_CMD,
    ANN_ADDRESS,
    ANN_DATA,
    ANN_PIO,
    ANN_STATUS,
    ANN_WARN,
    NUM_ANN,
};

/* Function command table */
struct ds2408_cmd {
    uint8_t code;
    const char *name;
};

static const struct ds2408_cmd ds2408_commands[] = {
    {0xf0, "Read PIO Registers"},
    {0xf5, "Channel Access Read"},
    {0x5a, "Channel Access Write"},
    {0xcc, "Write Conditional Search Register"},
    {0xc3, "Reset Activity Latches"},
    {0x3c, "Disable Test Mode"},
};

#define NUM_DS2408_COMMANDS (sizeof(ds2408_commands) / sizeof(ds2408_commands[0]))

enum ds2408_state {
    STATE_IDLE,
    STATE_CMD,
    STATE_READ_PIO_ADDR,
    STATE_READ_PIO_DATA,
    STATE_CHANNEL_ACCESS_READ,
    STATE_CHANNEL_ACCESS_WRITE_DATA,
    STATE_CHANNEL_ACCESS_WRITE_INV,
    STATE_CHANNEL_ACCESS_WRITE_ACK,
    STATE_WRITE_COND_ADDR,
    STATE_WRITE_COND_DATA,
    STATE_RESET_LATCHES,
};

typedef struct {
    enum ds2408_state state;
    uint8_t bytes[256];
    int num_bytes;
    uint64_t ss;
    uint64_t es;
    uint64_t ss_block;
    uint64_t es_block;
    int out_ann;
} ds2408_state;

static const char *ds2408_inputs[] = {"onewire_network", NULL};
static const char *ds2408_tags[] = {"Embedded/industrial", "IC", NULL};

static const char *ds2408_ann_labels[][3] = {
    {"", "reset-presence", "Reset/presence"},
    {"", "rom", "ROM address"},
    {"", "command", "Function command"},
    {"", "address", "Target address"},
    {"", "data", "Data byte"},
    {"", "pio", "PIO sample"},
    {"", "status", "Operation status"},
    {"", "warnings", "Warnings"},
};

static const int ds2408_row_cmds_classes[] = {ANN_CMD, ANN_ADDRESS, -1};
static const int ds2408_row_data_classes[] = {ANN_DATA, ANN_PIO, ANN_STATUS, -1};
static const int ds2408_row_rom_classes[] = {ANN_ROM, ANN_RESET_PRESENCE, -1};
static const int ds2408_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row ds2408_ann_rows[] = {
    {"commands", "Commands", ds2408_row_cmds_classes, 2},
    {"data", "Data", ds2408_row_data_classes, 3},
    {"rom", "ROM", ds2408_row_rom_classes, 2},
    {"warnings", "Warnings", ds2408_row_warnings_classes, 1},
};

static const struct ds2408_cmd *find_ds2408_command(uint8_t code)
{
    for (size_t i = 0; i < NUM_DS2408_COMMANDS; i++) {
        if (ds2408_commands[i].code == code)
            return &ds2408_commands[i];
    }
    return NULL;
}

static void ds2408_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ds2408_state *s = (ds2408_state *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t val = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "RESET/PRESENCE") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Reset/presence: %s", val ? "true" : "false");
        c_put(di, start_sample, end_sample, s->out_ann, ANN_RESET_PRESENCE, buf);
        s->num_bytes = 0;
        s->state = STATE_IDLE;
        return;
    }

    if (strcmp(cmd, "ROM") == 0) {
        uint64_t rom = 0;
        if (n_fields >= 8) {
            for (int i = 0; i < 8; i++)
                rom |= ((uint64_t)fields[i].u8) << (i * 8);
        }
        uint8_t family_code = n_fields > 0 ? fields[0].u8 : 0;
        char buf[256];
        snprintf(buf, sizeof(buf), "ROM: 0x%016llx (family code 0x%02x)",
                 (unsigned long long)rom, family_code);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_ROM, buf);
        s->num_bytes = 0;
        s->state = STATE_CMD;
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (s->num_bytes < 255)
        s->bytes[s->num_bytes++] = val;

    if (s->state == STATE_CMD) {
        s->ss_block = start_sample;
        s->es_block = end_sample;
        const struct ds2408_cmd *c = find_ds2408_command(val);
        if (c) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s (0x%02x)", c->name, val);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_CMD, buf);
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Unrecognized command: 0x%02x", val);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_WARN, buf);
            return;
        }

        switch (val) {
        case 0xf0: s->state = STATE_READ_PIO_ADDR; break;
        case 0xf5: s->state = STATE_CHANNEL_ACCESS_READ; break;
        case 0x5a: s->state = STATE_CHANNEL_ACCESS_WRITE_DATA; break;
        case 0xcc: s->state = STATE_WRITE_COND_ADDR; break;
        case 0xc3: s->state = STATE_RESET_LATCHES; break;
        default: s->state = STATE_IDLE; break;
        }
    } else if (s->state == STATE_READ_PIO_ADDR) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
            s->state = STATE_READ_PIO_DATA;
        }
    } else if (s->state == STATE_READ_PIO_DATA) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Data: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, buf);
    } else if (s->state == STATE_CHANNEL_ACCESS_READ) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes > 2) {
            char buf[32];
            snprintf(buf, sizeof(buf), "PIO sample: 0x%02x", val);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_PIO, buf);
        }
    } else if (s->state == STATE_CHANNEL_ACCESS_WRITE_DATA) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        }
    } else if (s->state == STATE_CHANNEL_ACCESS_WRITE_INV) {
        s->es_block = end_sample;
        uint8_t data_byte = s->bytes[s->num_bytes - 2];
        if (val == (data_byte ^ 0xff)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Data: 0x%02x (bit-inversion correct: 0x%02x)",
                     data_byte, val);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_DATA, buf);
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "Data error: second byte (0x%02x) is not bit-inverse of first (0x%02x)",
                     val, data_byte);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_WARN, buf);
        }
        s->state = STATE_CHANNEL_ACCESS_WRITE_ACK;
    } else if (s->state == STATE_CHANNEL_ACCESS_WRITE_ACK) {
        if (val == 0xaa) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS, "Success");
        } else if (val == 0xff) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS, "Fail New State");
        } else {
            char buf[32];
            snprintf(buf, sizeof(buf), "Ack: 0x%02x", val);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS, buf);
        }
        s->state = STATE_IDLE;
    } else if (s->state == STATE_WRITE_COND_ADDR) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
            s->state = STATE_WRITE_COND_DATA;
        }
    } else if (s->state == STATE_WRITE_COND_DATA) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Data: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, buf);
    } else if (s->state == STATE_RESET_LATCHES) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes > 2) {
            if (val == 0xaa) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS, "Success");
            } else {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, "Invalid byte");
            }
        }
    }

    /* Handle Channel Access Write: after first data byte, expect inversion */
    if (s->state == STATE_CHANNEL_ACCESS_WRITE_DATA && s->num_bytes == 3) {
        s->state = STATE_CHANNEL_ACCESS_WRITE_INV;
    }
}

static void ds2408_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(ds2408_state)));
    ds2408_state *s = (ds2408_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ds2408_state));
    s->state = STATE_IDLE;
}

static void ds2408_start(struct srd_decoder_inst *di)
{
    ds2408_state *s = (ds2408_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ds2408");
}

static void ds2408_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ds2408_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ds2408_c_decoder = {
    .id = "ds2408_c",
    .name = "DS2408(C)",
    .longname = "Maxim DS2408 (C)",
    .desc = "Maxim DS2408 1-Wire 8-channel addressable switch. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ds2408_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ds2408_ann_rows,
    .inputs = ds2408_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ds2408_tags,
    .num_tags = 2,
    .reset = ds2408_reset,
    .start = ds2408_start,
    .decode = ds2408_decode,
    .destroy = ds2408_destroy,
    .decode_upper = ds2408_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ds2408_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}