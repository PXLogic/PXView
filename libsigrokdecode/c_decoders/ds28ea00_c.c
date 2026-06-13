#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* DS28EA00 1-Wire digital thermometer with Sequence Detect and PIO */

enum {
    ANN_RESET_PRESENCE = 0,
    ANN_ROM,
    ANN_CMD,
    ANN_SCRATCHPAD,
    ANN_TEMP_CONV,
    ANN_PIO_READ,
    ANN_PIO_WRITE,
    ANN_OTHER,
    ANN_WARN,
    NUM_ANN,
};

/* Function command table */
struct ds28ea00_cmd {
    uint8_t code;
    const char *name;
};

static const struct ds28ea00_cmd ds28ea00_commands[] = {
    {0x4e, "Write scratchpad"},
    {0xbe, "Read scratchpad"},
    {0x48, "Copy scratchpad"},
    {0x44, "Convert temperature"},
    {0xb4, "Read power mode"},
    {0xb8, "Recall EEPROM"},
    {0xf5, "PIO access read"},
    {0xa5, "PIO access write"},
    {0x99, "Chain"},
};

#define NUM_DS28EA00_COMMANDS (sizeof(ds28ea00_commands) / sizeof(ds28ea00_commands[0]))

enum ds28ea00_state {
    STATE_ROM,
    STATE_COMMAND,
    STATE_READ_SCRATCHPAD,
    STATE_CONVERT_TEMP,
    STATE_WRITE_SCRATCHPAD,
    STATE_COPY_SCRATCHPAD,
    STATE_READ_POWER_MODE,
    STATE_RECALL_EEPROM,
    STATE_PIO_READ,
    STATE_PIO_WRITE,
    STATE_CHAIN,
};

typedef struct {
    enum ds28ea00_state state;
    uint64_t rom;
    int out_ann;
} ds28ea00_state;

static const char *ds28ea00_inputs[] = {"onewire_network", NULL};
static const char *ds28ea00_tags[] = {"IC", "Sensor", NULL};

static const char *ds28ea00_ann_labels[][3] = {
    {"", "reset-presence", "Reset/presence"},
    {"", "rom", "ROM address"},
    {"", "command", "Function command"},
    {"", "scratchpad", "Scratchpad data"},
    {"", "temp-conv", "Temperature conversion"},
    {"", "pio-read", "PIO read"},
    {"", "pio-write", "PIO write"},
    {"", "other", "Other data"},
    {"", "warnings", "Warnings"},
};

static const int ds28ea00_row_cmds_classes[] = {ANN_CMD, -1};
static const int ds28ea00_row_data_classes[] = {ANN_SCRATCHPAD, ANN_TEMP_CONV, ANN_PIO_READ, ANN_PIO_WRITE, ANN_OTHER, -1};
static const int ds28ea00_row_rom_classes[] = {ANN_ROM, ANN_RESET_PRESENCE, -1};
static const int ds28ea00_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row ds28ea00_ann_rows[] = {
    {"commands", "Commands", ds28ea00_row_cmds_classes, 1},
    {"data", "Data", ds28ea00_row_data_classes, 5},
    {"rom", "ROM", ds28ea00_row_rom_classes, 2},
    {"warnings", "Warnings", ds28ea00_row_warnings_classes, 1},
};

static const struct ds28ea00_cmd *find_ds28ea00_command(uint8_t code)
{
    for (size_t i = 0; i < NUM_DS28EA00_COMMANDS; i++) {
        if (ds28ea00_commands[i].code == code)
            return &ds28ea00_commands[i];
    }
    return NULL;
}

static void ds28ea00_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ds28ea00_state *s = (ds28ea00_state *)c_decoder_get_private(di);
    if (!s)
        return;

    uint8_t val = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "RESET/PRESENCE") == 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Reset/presence: %s", val ? "true" : "false");
        c_put(di, start_sample, end_sample, s->out_ann, ANN_RESET_PRESENCE, buf);
        s->state = STATE_ROM;
        return;
    }

    if (strcmp(cmd, "ROM") == 0) {
        uint64_t rom = 0;
        if (n_fields >= 8) {
            for (int i = 0; i < 8; i++)
                rom |= ((uint64_t)fields[i].u8) << (i * 8);
        }
        s->rom = rom;
        char buf[64];
        snprintf(buf, sizeof(buf), "ROM: 0x%016llx", (unsigned long long)rom);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_ROM, buf);
        s->state = STATE_COMMAND;
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (s->state == STATE_COMMAND) {
        const struct ds28ea00_cmd *c = find_ds28ea00_command(val);
        if (!c) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Unrecognized command: 0x%02x", val);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARN, buf);
            return;
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "Function command: 0x%02x '%s'", val, c->name);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_CMD, buf);

        switch (val) {
        case 0x4e: s->state = STATE_WRITE_SCRATCHPAD; break;
        case 0xbe: s->state = STATE_READ_SCRATCHPAD; break;
        case 0x48: s->state = STATE_COPY_SCRATCHPAD; break;
        case 0x44: s->state = STATE_CONVERT_TEMP; break;
        case 0xb4: s->state = STATE_READ_POWER_MODE; break;
        case 0xb8: s->state = STATE_RECALL_EEPROM; break;
        case 0xf5: s->state = STATE_PIO_READ; break;
        case 0xa5: s->state = STATE_PIO_WRITE; break;
        case 0x99: s->state = STATE_CHAIN; break;
        default: s->state = STATE_ROM; break;
        }
    } else if (s->state == STATE_READ_SCRATCHPAD) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Scratchpad data: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_SCRATCHPAD, buf);
    } else if (s->state == STATE_CONVERT_TEMP) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Temperature conversion status: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_TEMP_CONV, buf);
    } else if (s->state == STATE_PIO_READ) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PIO read: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_PIO_READ, buf);
    } else if (s->state == STATE_PIO_WRITE) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PIO write: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_PIO_WRITE, buf);
    } else if (s->state == STATE_WRITE_SCRATCHPAD ||
               s->state == STATE_COPY_SCRATCHPAD ||
               s->state == STATE_READ_POWER_MODE ||
               s->state == STATE_RECALL_EEPROM ||
               s->state == STATE_CHAIN) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Data: 0x%02x", val);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_OTHER, buf);
    }
}

static void ds28ea00_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(ds28ea00_state)));
    ds28ea00_state *s = (ds28ea00_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ds28ea00_state));
    s->state = STATE_ROM;
}

static void ds28ea00_start(struct srd_decoder_inst *di)
{
    ds28ea00_state *s = (ds28ea00_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ds28ea00");
}

static void ds28ea00_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ds28ea00_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ds28ea00_c_decoder = {
    .id = "ds28ea00_c",
    .name = "DS28EA00(C)",
    .longname = "Maxim DS28EA00 (C)",
    .desc = "Maxim DS28EA00 1-Wire digital thermometer with Sequence Detect and PIO. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ds28ea00_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ds28ea00_ann_rows,
    .inputs = ds28ea00_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ds28ea00_tags,
    .num_tags = 2,
    .reset = ds28ea00_reset,
    .start = ds28ea00_start,
    .decode = ds28ea00_decode,
    .destroy = ds28ea00_destroy,
    .decode_upper = ds28ea00_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ds28ea00_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}