#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* DS243x 1-Wire EEPROM decoder (DS2432/DS2433) */

enum {
    ANN_RESET_PRESENCE = 0,
    ANN_ROM,
    ANN_CMD,
    ANN_ADDRESS,
    ANN_DATA,
    ANN_ES,
    ANN_CRC,
    ANN_AUTH,
    ANN_MAC,
    ANN_STATUS,
    ANN_WARN,
    NUM_ANN,
};

/* DS2432 commands */
struct ds243x_cmd {
    uint8_t code;
    const char *name;
};

static const struct ds243x_cmd cmds_2432[] = {
    {0x0f, "Write scratchpad"},
    {0xaa, "Read scratchpad"},
    {0x55, "Copy scratchpad"},
    {0xf0, "Read memory"},
    {0x5a, "Load first secret"},
    {0x33, "Compute next secret"},
    {0xa5, "Read authenticated page"},
};

#define NUM_CMDS_2432 (sizeof(cmds_2432) / sizeof(cmds_2432[0]))

static const struct ds243x_cmd cmds_2433[] = {
    {0x0f, "Write scratchpad"},
    {0xaa, "Read scratchpad"},
    {0x55, "Copy scratchpad"},
    {0xf0, "Read memory"},
};

#define NUM_CMDS_2433 (sizeof(cmds_2433) / sizeof(cmds_2433[0]))

/* Family codes */
static const uint8_t family_2432 = 0x33;
static const uint8_t family_2433 = 0x23;

enum ds243x_state {
    STATE_IDLE,
    STATE_CMD,
    STATE_WRITE_SCRATCHPAD,
    STATE_READ_SCRATCHPAD,
    STATE_COPY_SCRATCHPAD,
    STATE_READ_MEMORY,
    STATE_LOAD_FIRST_SECRET,
    STATE_COMPUTE_NEXT_SECRET,
    STATE_READ_AUTH_PAGE,
};

typedef struct {
    enum ds243x_state state;
    uint8_t bytes[128];
    int num_bytes;
    uint8_t family_code;
    const struct ds243x_cmd *commands;
    int num_commands;
    char family_name[16];
    uint64_t ss;
    uint64_t es;
    uint64_t ss_block;
    uint64_t es_block;
    int out_ann;
} ds243x_state;

/* CRC-16 calculation */
static uint16_t crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xa001;
            else
                crc >>= 1;
        }
    }
    crc ^= 0xffff;
    return crc;
}

static const char *ds243x_inputs[] = {"onewire_network", NULL};
static const char *ds243x_tags[] = {"IC", "Memory", NULL};

static const char *ds243x_ann_labels[][3] = {
    {"", "reset-presence", "Reset/presence"},
    {"", "rom", "ROM address"},
    {"", "command", "Function command"},
    {"", "address", "Target address"},
    {"", "data", "Data"},
    {"", "es", "E/S register"},
    {"", "crc", "CRC check"},
    {"", "auth", "Authorization pattern"},
    {"", "mac", "Message authentication code"},
    {"", "status", "Operation status"},
    {"", "warnings", "Warnings"},
};

static const int ds243x_row_cmds_classes[] = {ANN_CMD, ANN_ADDRESS, ANN_ES, -1};
static const int ds243x_row_data_classes[] = {ANN_DATA, ANN_CRC, ANN_AUTH, ANN_MAC, ANN_STATUS, -1};
static const int ds243x_row_rom_classes[] = {ANN_ROM, ANN_RESET_PRESENCE, -1};
static const int ds243x_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row ds243x_ann_rows[] = {
    {"commands", "Commands", ds243x_row_cmds_classes, 3},
    {"data", "Data", ds243x_row_data_classes, 5},
    {"rom", "ROM", ds243x_row_rom_classes, 2},
    {"warnings", "Warnings", ds243x_row_warnings_classes, 1},
};

static const struct ds243x_cmd *find_command(uint8_t code,
    const struct ds243x_cmd *cmds, int num_cmds)
{
    for (int i = 0; i < num_cmds; i++) {
        if (cmds[i].code == code)
            return &cmds[i];
    }
    return NULL;
}

static void ds243x_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ds243x_state *s = (ds243x_state *)c_decoder_get_private(di);
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
        s->family_code = n_fields > 0 ? fields[0].u8 : 0;

        if (s->family_code == family_2432) {
            s->commands = cmds_2432;
            s->num_commands = NUM_CMDS_2432;
            snprintf(s->family_name, sizeof(s->family_name), "DS2432");
        } else if (s->family_code == family_2433) {
            s->commands = cmds_2433;
            s->num_commands = NUM_CMDS_2433;
            snprintf(s->family_name, sizeof(s->family_name), "DS2433");
        } else {
            s->commands = cmds_2432;
            s->num_commands = NUM_CMDS_2432;
            snprintf(s->family_name, sizeof(s->family_name), "unknown");
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "ROM: 0x%016llx (family code 0x%02x, %s)",
                 (unsigned long long)rom, s->family_code, s->family_name);
        c_put(di, start_sample, end_sample, s->out_ann, ANN_ROM, buf);
        s->num_bytes = 0;
        s->state = STATE_CMD;
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    if (s->num_bytes < 127)
        s->bytes[s->num_bytes++] = val;

    if (s->state == STATE_CMD) {
        s->ss_block = start_sample;
        s->es_block = end_sample;
        const struct ds243x_cmd *c = find_command(val, s->commands, s->num_commands);
        if (c) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Function command: %s (0x%02x)", c->name, val);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_CMD, buf);
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Unrecognized command: 0x%02x", val);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_WARN, buf);
            return;
        }

        switch (val) {
        case 0x0f: s->state = STATE_WRITE_SCRATCHPAD; break;
        case 0xaa: s->state = STATE_READ_SCRATCHPAD; break;
        case 0x55: s->state = STATE_COPY_SCRATCHPAD; break;
        case 0xf0: s->state = STATE_READ_MEMORY; break;
        case 0x5a: s->state = STATE_LOAD_FIRST_SECRET; break;
        case 0x33: s->state = STATE_COMPUTE_NEXT_SECRET; break;
        case 0xa5: s->state = STATE_READ_AUTH_PAGE; break;
        default: s->state = STATE_IDLE; break;
        }
    } else if (s->state == STATE_WRITE_SCRATCHPAD) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
        } else if (s->num_bytes >= 4 && s->num_bytes <= 11) {
            /* Data bytes 3..10 */
            if (s->num_bytes == 11) {
                char buf[512];
                int pos = 0;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "Data: ");
                for (int i = 3; i < 11; i++) {
                    if (i > 3) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
                }
                c_put(di, s->ss_block, end_sample, s->out_ann, ANN_DATA, buf);
            }
        } else if (s->num_bytes == 13) {
            /* CRC-16 */
            uint16_t recv_crc = s->bytes[11] | (s->bytes[12] << 8);
            uint16_t calc_crc = crc16(s->bytes, 11);
            char buf[64];
            snprintf(buf, sizeof(buf), "CRC: %s", calc_crc == recv_crc ? "ok" : "error");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_CRC, buf);
        }
    } else if (s->state == STATE_READ_SCRATCHPAD) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
        } else if (s->num_bytes == 4) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Data status (E/S): 0x%02x", s->bytes[3]);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_ES, buf);
        } else if (s->num_bytes >= 5 && s->num_bytes <= 12) {
            if (s->num_bytes == 12) {
                char buf[512];
                int pos = 0;
                pos += snprintf(buf + pos, sizeof(buf) - pos, "Data: ");
                for (int i = 4; i < 12; i++) {
                    if (i > 4) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
                }
                c_put(di, s->ss_block, end_sample, s->out_ann, ANN_DATA, buf);
            }
        } else if (s->num_bytes == 14) {
            uint16_t recv_crc = s->bytes[12] | (s->bytes[13] << 8);
            uint16_t calc_crc = crc16(s->bytes, 12);
            char buf[64];
            snprintf(buf, sizeof(buf), "CRC: %s", calc_crc == recv_crc ? "ok" : "error");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_CRC, buf);
        }
    } else if (s->state == STATE_COPY_SCRATCHPAD) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 4) {
            s->es_block = end_sample;
            char buf[256];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "Authorization pattern (TA1, TA2, E/S): ");
            for (int i = 1; i < 4; i++) {
                if (i > 1) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
            }
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_AUTH, buf);
        } else if (s->num_bytes == 24) {
            char buf[512];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Message authentication code: ");
            for (int i = 4; i < 24; i++) {
                if (i > 4) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
            }
            c_put(di, start_sample, end_sample, s->out_ann, ANN_MAC, buf);
        } else if (s->num_bytes > 24) {
            if (val == 0xaa || val == 0x55) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS,
                          "Operation succeeded");
            } else if (val == 0x00) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS,
                          "Operation failed");
            }
        }
    } else if (s->state == STATE_READ_MEMORY) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
        } else if (s->num_bytes > 3) {
            char buf[32];
            snprintf(buf, sizeof(buf), "Data: 0x%02x", val);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, buf);
        }
    } else if (s->state == STATE_LOAD_FIRST_SECRET) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 4) {
            s->es_block = end_sample;
            char buf[256];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "Authorization pattern (TA1, TA2, E/S): ");
            for (int i = 1; i < 4; i++) {
                if (i > 1) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
            }
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_AUTH, buf);
        } else if (s->num_bytes > 4) {
            if (val == 0xaa || val == 0x55) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS,
                          "End of operation");
            }
        }
    } else if (s->state == STATE_COMPUTE_NEXT_SECRET) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
        } else if (s->num_bytes > 3) {
            if (val == 0xaa || val == 0x55) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS,
                          "End of operation");
            }
        }
    } else if (s->state == STATE_READ_AUTH_PAGE) {
        if (s->num_bytes == 2) {
            s->ss_block = start_sample;
        } else if (s->num_bytes == 3) {
            s->es_block = end_sample;
            uint16_t addr = (s->bytes[2] << 8) + s->bytes[1];
            char buf[64];
            snprintf(buf, sizeof(buf), "Target address: 0x%04x", addr);
            c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_ADDRESS, buf);
        } else if (s->num_bytes == 35) {
            char buf[512];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Data: ");
            for (int i = 3; i < 35; i++) {
                if (i > 3) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
            }
            c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, buf);
        } else if (s->num_bytes == 36) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Padding: %s", val == 0xff ? "ok" : "error");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_DATA, buf);
        } else if (s->num_bytes == 38) {
            uint16_t recv_crc = s->bytes[36] | (s->bytes[37] << 8);
            uint16_t calc_crc = crc16(s->bytes, 36);
            char buf[64];
            snprintf(buf, sizeof(buf), "CRC: %s", calc_crc == recv_crc ? "ok" : "error");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_CRC, buf);
        } else if (s->num_bytes == 58) {
            char buf[512];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Message authentication code: ");
            for (int i = 38; i < 58; i++) {
                if (i > 38) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "0x%02x", s->bytes[i]);
            }
            c_put(di, start_sample, end_sample, s->out_ann, ANN_MAC, buf);
        } else if (s->num_bytes == 60) {
            uint16_t recv_crc = s->bytes[58] | (s->bytes[59] << 8);
            uint16_t calc_crc = crc16(s->bytes + 38, 20);
            char buf[64];
            snprintf(buf, sizeof(buf), "MAC CRC: %s", calc_crc == recv_crc ? "ok" : "error");
            c_put(di, start_sample, end_sample, s->out_ann, ANN_CRC, buf);
        } else if (s->num_bytes > 60) {
            if (val == 0xaa || val == 0x55) {
                c_put(di, start_sample, end_sample, s->out_ann, ANN_STATUS,
                          "Operation completed");
            }
        }
    }
}

static void ds243x_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(ds243x_state)));
    ds243x_state *s = (ds243x_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ds243x_state));
    s->state = STATE_IDLE;
    s->commands = cmds_2432;
    s->num_commands = NUM_CMDS_2432;
    snprintf(s->family_name, sizeof(s->family_name), "DS2432");
}

static void ds243x_start(struct srd_decoder_inst *di)
{
    ds243x_state *s = (ds243x_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ds243x");
}

static void ds243x_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ds243x_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ds243x_c_decoder = {
    .id = "ds243x_c",
    .name = "DS243x(C)",
    .longname = "Maxim DS2432/3 (C)",
    .desc = "Maxim DS243x series 1-Wire EEPROM protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ds243x_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = ds243x_ann_rows,
    .inputs = ds243x_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = ds243x_tags,
    .num_tags = 2,
    .reset = ds243x_reset,
    .start = ds243x_start,
    .decode = ds243x_decode,
    .destroy = ds243x_destroy,
    .decode_upper = ds243x_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ds243x_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}