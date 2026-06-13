#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_REGISTER = 0,
    ANN_DECODE,
    NUM_ANN,
};

typedef struct {
    int out_ann;
} cfp_state;

static const char *cfp_inputs[] = {"mdio", NULL};
static const char *cfp_outputs[] = {NULL};
static const char *cfp_tags[] = {"Networking", NULL};

static const char *cfp_ann_labels[][3] = {
    {"", "register", "Register"},
    {"", "decode", "Decode"},
};

static const int cfp_row_registers_classes[] = {ANN_REGISTER, -1};
static const int cfp_row_decodes_classes[] = {ANN_DECODE, -1};
static const struct srd_c_ann_row cfp_ann_rows[] = {
    {"registers", "Registers", cfp_row_registers_classes, 1},
    {"decodes", "Decodes", cfp_row_decodes_classes, 1},
};

static const struct { uint8_t id; const char *name; } cfp_module_id_table[] = {
    {0x00, "Unknown or unspecified"},
    {0x01, "GBIC"},
    {0x02, "Module/connector soldered to motherboard"},
    {0x03, "SFP"},
    {0x04, "300 pin XSBI"},
    {0x05, "XENPAK"},
    {0x06, "XFP"},
    {0x07, "XFF"},
    {0x08, "XFP-E"},
    {0x09, "XPAK"},
    {0x0A, "X2"},
    {0x0B, "DWDM-SFP"},
    {0x0C, "QSFP"},
    {0x0D, "QSFP+"},
    {0x0E, "CFP"},
    {0x0F, "CXP (TBD)"},
    {0x11, "CFP2"},
    {0x12, "CFP4"},
};
#define NUM_MODULE_IDS (sizeof(cfp_module_id_table) / sizeof(cfp_module_id_table[0]))

static const char *cfp_lookup_module_id(uint8_t id)
{
    for (int i = 0; i < (int)NUM_MODULE_IDS; i++) {
        if (cfp_module_id_table[i].id == id)
            return cfp_module_id_table[i].name;
    }
    return "Reserved";
}

static void cfp_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    cfp_state *s = (cfp_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "DATA") != 0) return;
    if (!fields || n_fields < 8) return;

    int clause45 = fields[0].u8;
    int clause45_addr = (fields[1].u8 << 8) | fields[2].u8;
    int is_read = fields[3].u8;
    int reg = (fields[6].u8 << 8) | fields[7].u8;

    (void)clause45;

    if (!is_read) return;

    if (clause45_addr >= 0x8000 && clause45_addr <= 0x807F) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "CFP NVR 1: Basic ID register", "NVR1");
        if (clause45_addr == 0x8000) {
            const char *mod_name = cfp_lookup_module_id((uint8_t)reg);
            char buf[256];
            snprintf(buf, sizeof(buf), "Module identifier: %s", mod_name);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_DECODE, buf);
        }
    } else if (clause45_addr >= 0x8080 && clause45_addr <= 0x80FF) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "CFP NVR 2: Extended ID register", "NVR2");
    } else if (clause45_addr >= 0x8100 && clause45_addr <= 0x817F) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "CFP NVR 3: Network lane specific register", "NVR3");
    } else if (clause45_addr >= 0x8180 && clause45_addr <= 0x81FF) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "CFP NVR 4", "NVR4");
    } else if (clause45_addr >= 0x8400 && clause45_addr <= 0x847F) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "Vendor NVR 1: Vendor data register", "V-NVR1");
    } else if (clause45_addr >= 0x8480 && clause45_addr <= 0x84FF) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "Vendor NVR 2: Vendor data register", "V-NVR2");
    } else if (clause45_addr >= 0x8800 && clause45_addr <= 0x887F) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "User NVR 1: User data register", "U-NVR1");
    } else if (clause45_addr >= 0x8880 && clause45_addr <= 0x88FF) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "User NVR 2: User data register", "U-NVR2");
    } else if (clause45_addr >= 0xA000 && clause45_addr <= 0xA07F) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "CFP Module VR 1: CFP Module level control and DDM register", "Mod-VR1");
    } else if (clause45_addr >= 0xA080 && clause45_addr <= 0xA0FF) {
        c_put(di, start_sample, end_sample, s->out_ann, ANN_REGISTER,
                  "MLG VR 1: MLG Management Interface register", "MLG-VR1");
    }
}

static void cfp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(cfp_state)));
    }
    cfp_state *s = (cfp_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(cfp_state));
}

static void cfp_start(struct srd_decoder_inst *di)
{
    cfp_state *s = (cfp_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "cfp");
}

static void cfp_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void cfp_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder cfp_c_decoder = {
    .id = "cfp_c",
    .name = "CFP(C)",
    .longname = "100 Gigabit C form-factor pluggable (C)",
    .desc = "100 Gigabit C form-factor pluggable (CFP) protocol. (C implementation)",
    .license = "BSD",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = cfp_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = cfp_ann_rows,
    .inputs = cfp_inputs,
    .num_inputs = 1,
    .outputs = cfp_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = cfp_tags,
    .num_tags = 1,
    .reset = cfp_reset,
    .start = cfp_start,
    .decode = cfp_decode,
    .destroy = cfp_destroy,
    .decode_upper = cfp_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &cfp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}