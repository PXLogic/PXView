#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_PRESS = 0,
    ANN_RELEASE,
    ANN_ACK,
    NUM_ANN,
};

enum {
    BINARY_KEYS = 0,
};

typedef struct {
    int out_ann;
    int out_binary;
    int sw;           /* 0=initial, 1=got first byte, 4=ACK */
    int ann;          /* ANN_PRESS / ANN_RELEASE / ANN_ACK */
    int extended;     /* extended character flag */
    uint64_t ss;      /* start sample */
} ps2kb_state;

static const char *ps2kb_inputs[] = {"ps2", NULL};
static const char *ps2kb_outputs[] = {NULL};
static const char *ps2kb_tags[] = {"PC", NULL};

static const char *ps2kb_ann_labels[][3] = {
    {"", "Press", "Key pressed"},
    {"", "Release", "Key released"},
    {"", "Ack", "Acknowledge"},
};

static const int ps2kb_row_keys_classes[] = {ANN_PRESS, ANN_RELEASE, ANN_ACK, -1};
static const struct srd_c_ann_row ps2kb_ann_rows[] = {
    {"keys", "Key presses and releases", ps2kb_row_keys_classes, 3},
};

static const struct srd_decoder_binary ps2kb_binary[] = {
    {0, "keys", "Key presses"},
};

/* Standard scan code table */
static const struct { uint8_t code; const char *name; } ps2kb_std_keys[] = {
    {0x1C, "A"}, {0x32, "B"}, {0x21, "C"}, {0x23, "D"}, {0x24, "E"},
    {0x2B, "F"}, {0x34, "G"}, {0x33, "H"}, {0x43, "I"}, {0x3B, "J"},
    {0x42, "K"}, {0x4B, "L"}, {0x3A, "M"}, {0x31, "N"}, {0x44, "O"},
    {0x4D, "P"}, {0x15, "Q"}, {0x2D, "R"}, {0x1B, "S"}, {0x2C, "T"},
    {0x3C, "U"}, {0x2A, "V"}, {0x1D, "W"}, {0x22, "X"}, {0x35, "Y"},
    {0x1A, "Z"}, {0x45, "0)"}, {0x16, "1!"}, {0x1E, "2@"}, {0x26, "3#"},
    {0x25, "4$"}, {0x2E, "5%"}, {0x36, "6^"}, {0x3D, "7&"}, {0x3E, "8*"},
    {0x46, "9("}, {0x0E, "`~"}, {0x4E, "-_"}, {0x55, "=+"}, {0x5D, "\\|"},
    {0x66, "Backsp"}, {0x29, "Space"}, {0x0D, "Tab"}, {0x58, "CapsLk"},
    {0x12, "L Shft"}, {0x14, "L Ctrl"}, {0x11, "L Alt"}, {0x59, "R Shft"},
    {0x5A, "Enter"}, {0x76, "Esc"}, {0x05, "F1"}, {0x06, "F2"},
    {0x04, "F3"}, {0x0C, "F4"}, {0x03, "F5"}, {0x0B, "F6"},
    {0x83, "F7"}, {0x0A, "F8"}, {0x01, "F9"}, {0x09, "F10"},
    {0x78, "F11"}, {0x07, "F12"}, {0x7E, "ScrLck"},
};
#define NUM_STD_KEYS (sizeof(ps2kb_std_keys) / sizeof(ps2kb_std_keys[0]))

/* Extended scan code table */
static const struct { uint8_t code; const char *name; } ps2kb_ext_keys[] = {
    {0x1F, "L Sup"}, {0x14, "R Ctrl"}, {0x27, "R Sup"}, {0x11, "R Alt"},
    {0x2F, "Menu"}, {0x12, "PrtScr"}, {0x7C, "SysRq"}, {0x70, "Insert"},
    {0x6C, "Home"}, {0x7D, "Pg Up"}, {0x71, "Delete"}, {0x69, "End"},
    {0x7A, "Pg Dn"}, {0x75, "Up arrow"}, {0x6B, "Left arrow"},
    {0x74, "Right arrow"}, {0x72, "Down arrow"}, {0x4A, "KP /"},
    {0x5A, "KP Ent"},
};
#define NUM_EXT_KEYS (sizeof(ps2kb_ext_keys) / sizeof(ps2kb_ext_keys[0]))

static const char *ps2kb_lookup_key(uint8_t code, int extended)
{
    if (extended) {
        for (int i = 0; i < (int)NUM_EXT_KEYS; i++) {
            if (ps2kb_ext_keys[i].code == code)
                return ps2kb_ext_keys[i].name;
        }
    } else {
        for (int i = 0; i < (int)NUM_STD_KEYS; i++) {
            if (ps2kb_std_keys[i].code == code)
                return ps2kb_std_keys[i].name;
        }
    }
    return NULL;
}

static void ps2kb_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    ps2kb_state *s = (ps2kb_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "BYTE") != 0) return;
    if (!fields || n_fields < 4) return;

    uint8_t val = fields[0].u8;
    int is_host = fields[1].u8;

    /* Reset state when host sends */
    if (is_host) {
        s->sw = 0;
        s->ann = ANN_PRESS;
        s->extended = 0;
        return;
    }

    if (s->sw < 1) {
        s->ss = start_sample;
        s->sw = 1;
    }

    if (s->sw < 2) {
        if (val == 0xF0) {
            s->ann = ANN_RELEASE;
            return;
        } else if (val == 0xE0) {
            s->extended = 1;
            return;
        } else if (val == 0xFA) {
            s->ann = ANN_ACK;
            s->sw = 4;
        }
    }

    if (s->sw < 3) {
        const char *key_name = ps2kb_lookup_key(val, s->extended);
        if (key_name) {
            c_put(di, s->ss, end_sample, s->out_ann, s->ann, key_name);
        } else {
            char buf[16];
            if (s->extended)
                snprintf(buf, sizeof(buf), "[E0%02X]", val);
            else
                snprintf(buf, sizeof(buf), "[%02X]", val);
            c_put(di, s->ss, end_sample, s->out_ann, s->ann, buf);
        }
    }

    /* Output binary on key press */
    if (s->ann == ANN_PRESS && s->sw < 3) {
        const char *key_name = ps2kb_lookup_key(val, s->extended);
        if (key_name) {
            c_put_bin(di, s->ss, end_sample, s->out_binary,
                                 BINARY_KEYS, strlen(key_name), (const uint8_t *)key_name);
        }
    }

    /* Reset state */
    s->sw = 0;
    s->ann = ANN_PRESS;
    s->extended = 0;
}

static void ps2kb_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(ps2kb_state)));
    }
    ps2kb_state *s = (ps2kb_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(ps2kb_state));
    s->ann = ANN_PRESS;
}

static void ps2kb_start(struct srd_decoder_inst *di)
{
    ps2kb_state *s = (ps2kb_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ps2_keyboard");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "keys");
}

static void ps2kb_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void ps2kb_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ps2_keyboard_c_decoder = {
    .id = "ps2_keyboard_c",
    .name = "PS/2 Keyboard(C)",
    .longname = "PS/2 Keyboard (C)",
    .desc = "PS/2 keyboard interface. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = ps2kb_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = ps2kb_ann_rows,
    .inputs = ps2kb_inputs,
    .num_inputs = 1,
    .outputs = ps2kb_outputs,
    .num_outputs = 0,
    .binary = ps2kb_binary,
    .num_binary = 1,
    .tags = ps2kb_tags,
    .num_tags = 1,
    .reset = ps2kb_reset,
    .start = ps2kb_start,
    .decode = ps2kb_decode,
    .destroy = ps2kb_destroy,
    .decode_upper = ps2kb_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &ps2_keyboard_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}