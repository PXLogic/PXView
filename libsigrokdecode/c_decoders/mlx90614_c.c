/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012 Uwe Hermann <uwe@hermann-uwe.de>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum {
    ANN_CELSIUS = 0,
    ANN_KELVIN,
    NUM_ANN,
};

enum mlx90614_state {
    MLX90614_IGNORE_START_REPEAT,
    MLX90614_IGNORE_ADDRESS_READ,
    MLX90614_GET_TEMPERATURE,
};

typedef struct {
    enum mlx90614_state state;
    uint8_t data[2];
    int data_count;
    
    int out_ann;
    uint64_t ss;
    uint64_t es;
} mlx90614_priv;

static const char *mlx90614_inputs[] = {"i2c", NULL};
static const char *mlx90614_tags[] = {"IC", "Sensor", NULL};

static const char *mlx90614_ann_labels[][3] = {
    {"", "celsius", "Temperature / °C"},
    {"", "kelvin", "Temperature / K"},
};

static const int mlx90614_row_celsius_classes[] = {ANN_CELSIUS};
static const int mlx90614_row_kelvin_classes[] = {ANN_KELVIN};
static const struct srd_c_ann_row mlx90614_ann_rows[] = {
    {"temps-celsius", "Temperature / °C", mlx90614_row_celsius_classes, 1},
    {"temps-kelvin", "Temperature / K", mlx90614_row_kelvin_classes, 1},
};

static void mlx90614_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    mlx90614_priv *s = (mlx90614_priv *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    if (s->state == MLX90614_IGNORE_START_REPEAT) {
        if (strcmp(cmd, "START REPEAT") == 0)
            s->state = MLX90614_IGNORE_ADDRESS_READ;
    } else if (s->state == MLX90614_IGNORE_ADDRESS_READ) {
        if (strcmp(cmd, "ADDRESS READ") == 0)
            s->state = MLX90614_GET_TEMPERATURE;
    } else if (s->state == MLX90614_GET_TEMPERATURE) {
        if (strcmp(cmd, "DATA READ") == 0) {
            uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;
            if (s->data_count == 0) {
                s->data[0] = databyte;
                s->ss = start_sample;
                s->data_count = 1;
            } else if (s->data_count == 1) {
                s->data[1] = databyte;
                s->es = end_sample;
                double kelvin = (double)(s->data[0] | (s->data[1] << 8)) * 0.02;
                double celsius = kelvin - 273.15;
                char buf[64];
                snprintf(buf, sizeof(buf), "Temperature: %.2f °C", celsius);
                c_put(di, s->ss, s->es, s->out_ann, ANN_CELSIUS, buf);
                snprintf(buf, sizeof(buf), "Temperature: %.2f K", kelvin);
                c_put(di, s->ss, s->es, s->out_ann, ANN_KELVIN, buf);
                s->state = MLX90614_IGNORE_START_REPEAT;
                s->data_count = 0;
            }
        }
    }
}

static void mlx90614_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(mlx90614_priv)));
    }
    mlx90614_priv *s = (mlx90614_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mlx90614_priv));
    s->state = MLX90614_IGNORE_START_REPEAT;
}

static void mlx90614_start(struct srd_decoder_inst *di)
{
    mlx90614_priv *s = (mlx90614_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mlx90614");
}

static void mlx90614_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void mlx90614_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder mlx90614_c_decoder = {
    .id = "mlx90614_c",
    .name = "MLX90614(C)",
    .longname = "Melexis MLX90614 (C)",
    .desc = "Melexis MLX90614 infrared thermometer protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mlx90614_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = mlx90614_ann_rows,
    .inputs = mlx90614_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = mlx90614_tags,
    .num_tags = 2,
    .reset = mlx90614_reset,
    .start = mlx90614_start,
    .decode = mlx90614_decode,
    .destroy = mlx90614_destroy,
    .decode_upper = mlx90614_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &mlx90614_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}