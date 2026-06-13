/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2014 Gump Yang <gump.yang@gmail.com>
 * Copyright (C) 2019 Rene Staffen
 * Copyright (C) 2020-2021 Gerhard Sittig <gerhard.sittig@gmx.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define IR_CH 0

enum irmp_ann {
    ANN_PACKET = 0,
    NUM_ANN,
};

/* ResultData structure matching IRMP library */
typedef struct {
    uint32_t protocol;
    char *protocol_name;
    uint32_t address;
    uint32_t command;
    uint32_t flags;       /* bit0=repeat, bit1=release */
    uint32_t start_sample;
    uint32_t end_sample;
} irmp_result_data;

typedef struct {
    uint64_t samplerate;
    int active;          /* 0=active-low, 1=active-high */
    uint64_t rate_factor;
    int out_ann;

    /* IRMP library handle */
#ifdef _WIN32
    HMODULE irmp_lib;
#else
    void *irmp_lib;
#endif

    /* Function pointers */
    uint32_t (*fn_get_sample_rate)(void);
    int (*fn_add_one_sample)(int);
    int (*fn_get_result_data)(irmp_result_data *);
    char *(*fn_get_protocol_name)(uint32_t);
    void (*fn_reset_state)(void);

    /* Library loaded flag */
    int lib_loaded;
    int lib_error_reported;
} irmp_priv;

static struct srd_channel irmp_channels[] = {
    { "ir", "IR", "Data line", 0, SRD_CHANNEL_SDATA, "dec_ir_irmp_chan_ir" },
};

static struct srd_decoder_option irmp_options_arr[1];

static const char* irmp_ann_labels[][3] = {
    { "", "packet", "Packet" },
};

static const int irmp_row_packets_classes[] = { ANN_PACKET, -1 };
static const struct srd_c_ann_row irmp_ann_rows[] = {
    { "packets", "IR Packets", irmp_row_packets_classes, 1 },
};

static const char* irmp_inputs[] = { "logic", NULL };
static const char* irmp_outputs[] = { NULL };
static const char* irmp_tags[] = { "IR", NULL };

static int irmp_load_library(irmp_priv *s)
{
    if (s->lib_loaded)
        return 1;

#ifdef _WIN32
    s->irmp_lib = LoadLibraryA("irmp.dll");
    if (!s->irmp_lib)
        return 0;

    s->fn_get_sample_rate = (uint32_t (*)(void))(void *)
        GetProcAddress(s->irmp_lib, "irmp_get_sample_rate");
    s->fn_add_one_sample = (int (*)(int))(void *)
        GetProcAddress(s->irmp_lib, "irmp_add_one_sample");
    s->fn_get_result_data = (int (*)(irmp_result_data *))(void *)
        GetProcAddress(s->irmp_lib, "irmp_get_result_data");
    s->fn_get_protocol_name = (char *(*)(uint32_t))(void *)
        GetProcAddress(s->irmp_lib, "irmp_get_protocol_name");
    s->fn_reset_state = (void (*)(void))(void *)
        GetProcAddress(s->irmp_lib, "irmp_reset_state");
#else
    s->irmp_lib = dlopen("libirmp.so", RTLD_LAZY);
    if (!s->irmp_lib)
        return 0;

    s->fn_get_sample_rate = (uint32_t (*)(void))
        dlsym(s->irmp_lib, "irmp_get_sample_rate");
    s->fn_add_one_sample = (int (*)(int))
        dlsym(s->irmp_lib, "irmp_add_one_sample");
    s->fn_get_result_data = (int (*)(irmp_result_data *))
        dlsym(s->irmp_lib, "irmp_get_result_data");
    s->fn_get_protocol_name = (char *(*)(uint32_t))
        dlsym(s->irmp_lib, "irmp_get_protocol_name");
    s->fn_reset_state = (void (*)(void))
        dlsym(s->irmp_lib, "irmp_reset_state");
#endif

    if (!s->fn_get_sample_rate || !s->fn_add_one_sample ||
        !s->fn_get_result_data || !s->fn_get_protocol_name ||
        !s->fn_reset_state) {
#ifdef _WIN32
        FreeLibrary(s->irmp_lib);
#else
        dlclose(s->irmp_lib);
#endif
        s->irmp_lib = 0;
        return 0;
    }

    s->lib_loaded = 1;
    return 1;
}

static void irmp_putframe(struct srd_decoder_inst *di, irmp_priv *s,
    irmp_result_data *result, uint64_t rate_factor)
{
    uint32_t nr = result->protocol;
    char *name = result->protocol_name;
    uint32_t addr = result->address;
    uint32_t cmd = result->command;
    int repeat = result->flags & 1;
    int release = (result->flags >> 1) & 1;
    uint64_t ss = (uint64_t)result->start_sample * rate_factor;
    uint64_t es = (uint64_t)result->end_sample * rate_factor;

    /* Build flag texts */
    char flg0[64], flg1[32], flg2[16];
    flg0[0] = '\0';
    flg1[0] = '\0';
    flg2[0] = '\0';
    if (repeat && release) {
        strcpy(flg0, "repeat release");
        strcpy(flg1, "rep rel");
        strcpy(flg2, "r R");
    } else if (repeat) {
        strcpy(flg0, "repeat");
        strcpy(flg1, "rep");
        strcpy(flg2, "r");
    } else if (release) {
        strcpy(flg0, "release");
        strcpy(flg1, "rel");
        strcpy(flg2, "R");
    } else {
        strcpy(flg0, "-");
        strcpy(flg1, "-");
        strcpy(flg2, "-");
    }

    const char *proto_name = name ? name : "Unknown";

    char txt1[256], txt2[256], txt3[256], txt4[128], txt5[64];
    snprintf(txt1, sizeof(txt1), "Protocol: %s (%u), Address 0x%04x, Command: 0x%04x, Flags: %s",
        proto_name, nr, addr, cmd, flg0);
    snprintf(txt2, sizeof(txt2), "P: %s (%u), Addr: 0x%x, Cmd: 0x%x, Flg: %s",
        proto_name, nr, addr, cmd, flg1);
    snprintf(txt3, sizeof(txt3), "P: %u A: 0x%x C: 0x%x F: %s",
        nr, addr, cmd, flg1);
    snprintf(txt4, sizeof(txt4), "C:%x A:%x %s", cmd, addr, flg2);
    snprintf(txt5, sizeof(txt5), "C:%x", cmd);

    c_put(di, ss, es, s->out_ann, ANN_PACKET, txt1, txt2, txt3, txt4, txt5);
}

static void irmp_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    irmp_priv *s = (irmp_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void irmp_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(irmp_priv)));
    }
    irmp_priv *s = (irmp_priv *)c_decoder_get_private(di);
    int lib_loaded = s->lib_loaded;
    memset(s, 0, sizeof(irmp_priv));
    s->lib_loaded = lib_loaded;
}

static void irmp_start(struct srd_decoder_inst *di)
{
    irmp_priv *s = (irmp_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "ir_irmp");

    const char *polarity = c_opt_str(di, "polarity", "active-low");
    s->active = (polarity && strcmp(polarity, "active-high") == 0) ? 1 : 0;

    s->samplerate = c_samplerate(di);
}

static void irmp_decode(struct srd_decoder_inst *di)
{
    irmp_priv *s = (irmp_priv *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
    }
    if (s->samplerate == 0)
        return;

    /* Try to load IRMP library */
    if (!s->lib_loaded) {
        if (!irmp_load_library(s)) {
            if (!s->lib_error_reported) {
                s->lib_error_reported = 1;
            }
            /* Library not available, just consume data silently */
            c_wait(di, CW_SKIP(0), CW_END);
            return;
        }
    }

    /* Get library sample rate */
    uint32_t lib_rate = s->fn_get_sample_rate();
    if (lib_rate == 0)
        return;

    /* Verify samplerate is multiple of library rate */
    if (s->samplerate % lib_rate != 0)
        return;

    s->rate_factor = s->samplerate / lib_rate;

    /* Reset IRMP state */
    s->fn_reset_state();

    /* Get initial IR value */
    uint64_t cur_sample = 0;
    (void)cur_sample;
    if (c_wait(di, CW_END) != SRD_OK)
        return;

    int ir = c_pin(di, IR_CH);

    while (1) {
        int ret;

        /* Apply polarity inversion */
        int sample_val = ir;
        if (s->active == 1)
            sample_val = 1 - sample_val;

        /* Feed sample to IRMP library */
        if (s->fn_add_one_sample(sample_val)) {
            irmp_result_data result;
            memset(&result, 0, sizeof(result));
            if (s->fn_get_result_data(&result)) {
                irmp_putframe(di, s, &result, s->rate_factor);
            }
        }

        /* Wait for next sample (skip rate_factor samples) */
        ret = c_wait(di, CW_SKIP(s->rate_factor), CW_END);
        if (ret != SRD_OK)
            return;

        ir = c_pin(di, IR_CH);
    }
}

static void irmp_destroy(struct srd_decoder_inst *di)
{
    irmp_priv *s = (irmp_priv *)c_decoder_get_private(di);
    if (s) {
        if (s->lib_loaded && s->irmp_lib) {
#ifdef _WIN32
            FreeLibrary(s->irmp_lib);
#else
            dlclose(s->irmp_lib);
#endif
        }
        g_free(s);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder ir_irmp_c_decoder = {
    .id = "ir_irmp_c",
    .name = "IR IRMP(C)",
    .longname = "IRMP infrared remote control multi protocol (C)",
    .desc = "IRMP infrared remote control multi protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = irmp_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = irmp_options_arr,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = irmp_ann_labels,
    .num_annotation_rows = 1,
    .annotation_rows = irmp_ann_rows,
    .inputs = irmp_inputs,
    .num_inputs = 1,
    .outputs = irmp_outputs,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = irmp_tags,
    .num_tags = 1,
    .metadata = irmp_metadata,
    .reset = irmp_reset,
    .start = irmp_start,
    .decode = irmp_decode,
    .destroy = irmp_destroy,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    GVariant *polarity_vals[] = {
        g_variant_new_string("active-low"),
        g_variant_new_string("active-high"),
    };
    GSList *polarity_list = NULL;
    polarity_list = g_slist_append(polarity_list, polarity_vals[0]);
    polarity_list = g_slist_append(polarity_list, polarity_vals[1]);
    irmp_options_arr[0].id = "polarity";
    irmp_options_arr[0].idn = "dec_ir_irmp_opt_polarity";
    irmp_options_arr[0].desc = "Polarity";
    irmp_options_arr[0].def = g_variant_new_string("active-low");
    irmp_options_arr[0].values = polarity_list;

    return &ir_irmp_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}