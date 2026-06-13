#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_BUTTON = 0,
    ANN_NO_PRESS,
    ANN_NOT_CONNECTED,
    NUM_ANN,
};

typedef struct {
    int out_ann;
    int variant; /* 0 = Standard gamepad */
} nes_gamepad_state;

static const char *nes_gamepad_inputs[] = {"spi", NULL};
static const char *nes_gamepad_tags[] = {"Retro computing", NULL};

static struct srd_decoder_option nes_gamepad_options[] = {
    {"variant", "dec_nes_gamepad_opt_variant", "Gamepad variant", NULL, NULL},
};

static const char *nes_gamepad_ann_labels[][3] = {
    {"", "button", "Button state"},
    {"", "no-press", "No button press"},
    {"", "not-connected", "Gamepad unconnected"},
};

static const int nes_gamepad_row_buttons_classes[] = {ANN_BUTTON, -1};
static const int nes_gamepad_row_no_presses_classes[] = {ANN_NO_PRESS, -1};
static const int nes_gamepad_row_not_connected_classes[] = {ANN_NOT_CONNECTED, -1};

static const struct srd_c_ann_row nes_gamepad_ann_rows[] = {
    {"buttons", "Button states", nes_gamepad_row_buttons_classes, 1},
    {"no-presses", "No button presses", nes_gamepad_row_no_presses_classes, 1},
    {"not-connected-vals", "Gamepad unconnected", nes_gamepad_row_not_connected_classes, 1},
};

static void nes_gamepad_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    nes_gamepad_state *s = (nes_gamepad_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "DATA") != 0 || n_fields < 17)
        return;

    
    uint64_t miso_val = 0;
    for (int i = 0; i < 8; i++)
        miso_val |= ((uint64_t)fields[9 + i].u8 << (8 * i));
    uint8_t miso = (uint8_t)miso_val;

    if (miso == 0xFF) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_NO_PRESS,
                  "No button is pressed");
    } else if (miso == 0x00) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_NOT_CONNECTED,
                  "Gamepad is not connected");
    } else {
        static const char *buttons[] = {
            "A", "B", "Select", "Start", "North", "South", "West", "East"
        };
        char buf[256];
        int pos = 0;
        for (int i = 0; i < 8; i++) {
            if (!(miso & (1 << i))) {
                if (pos > 0)
                    pos += snprintf(buf + pos, sizeof(buf) - pos, " + ");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", buttons[i]);
            }
        }
        c_put(di, start_sample, end_sample, s->out_ann, ANN_BUTTON, buf);
    }
}

static void nes_gamepad_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(nes_gamepad_state)));
    }
    nes_gamepad_state *s = (nes_gamepad_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(nes_gamepad_state));
    s->variant = 0;
}

static void nes_gamepad_start(struct srd_decoder_inst *di)
{
    nes_gamepad_state *s = (nes_gamepad_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "nes_gamepad");

    const char *variant = c_opt_str(di, "variant", "Standard gamepad");
    (void)variant;
    s->variant = 0; /* Only Standard gamepad supported */
}

static void nes_gamepad_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void nes_gamepad_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder nes_gamepad_c_decoder = {
    .id = "nes_gamepad_c",
    .name = "NES gamepad(C)",
    .longname = "Nintendo Entertainment System gamepad (C)",
    .desc = "NES gamepad button states. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = nes_gamepad_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = nes_gamepad_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = nes_gamepad_ann_rows,
    .inputs = nes_gamepad_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = nes_gamepad_tags,
    .num_tags = 1,
    .reset = nes_gamepad_reset,
    .start = nes_gamepad_start,
    .decode = nes_gamepad_decode,
    .destroy = nes_gamepad_destroy,
    .decode_upper = nes_gamepad_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    nes_gamepad_options[0].def = g_variant_new_string("Standard gamepad");
    GSList *variant_vals = NULL;
    variant_vals = g_slist_append(variant_vals, g_variant_new_string("Standard gamepad"));
    nes_gamepad_options[0].values = variant_vals;

    return &nes_gamepad_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}