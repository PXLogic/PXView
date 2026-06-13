/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2012-2014 Uwe Hermann <uwe@hermann-uwe.de>
 * Copyright (C) 2013 Matt Ranostay <mranostay@gmail.com>
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
    ANN_REG_SMPLRT_DIV = 0,
    ANN_REG_CONFIG,
    ANN_REG_GYRO_CONFIG,
    ANN_REG_ACCEL_CONFIG,
    ANN_REG_INT_ENABLE,
    ANN_REG_PWR_MGMT_1,
    ANN_REG_WHO_AM_I,
    ANN_REG_DATA,
    ANN_REG_RESERVED,
    ANN_BIT_FS_SEL,
    ANN_BIT_AFS_SEL,
    ANN_BIT_CLK_SEL,
    ANN_BIT_SLEEP,
    ANN_BIT_CYCLE,
    ANN_BIT_TEMP_DIS,
    ANN_BIT_RESET,
    ANN_BIT_DLPF,
    ANN_BIT_EXT_SYNC,
    ANN_BIT_INT_EN,
    ANN_BIT_XG_ST,
    ANN_BIT_YG_ST,
    ANN_BIT_ZG_ST,
    ANN_BIT_XA_ST,
    ANN_BIT_YA_ST,
    ANN_ACCEL_DATA,
    ANN_GYRO_DATA,
    ANN_TEMP_DATA,
    ANN_REG_WRITE,
    ANN_WARNING,
    NUM_ANN,
};

enum mpu6050_state {
    MPU6050_IDLE,
    MPU6050_GET_SLAVE_ADDR,
    MPU6050_GET_REG_ADDR,
    MPU6050_WRITE_REGS,
};

#define MPU6050_I2C_ADDRESS 0x68

typedef struct {
    enum mpu6050_state state;
    int reg;
    int acc_x, acc_y, acc_z;
    int temp;
    int gyro_x, gyro_y, gyro_z;
    
    uint64_t ss_block;
    uint64_t ss;
    uint64_t es;
    int out_ann;
} mpu6050_priv;

static const char *mpu6050_inputs[] = {"i2c", NULL};
static const char *mpu6050_outputs[] = {"mpu6050", NULL};
static const char *mpu6050_tags[] = {"Gyroscope", NULL};

static const char *mpu6050_ann_labels[][3] = {
    {"", "reg-smplrt-div", "SMPLRT_DIV register"},
    {"", "reg-config", "CONFIG register"},
    {"", "reg-gyro-config", "GYRO_CONFIG register"},
    {"", "reg-accel-config", "ACCEL_CONFIG register"},
    {"", "reg-int-enable", "INT_ENABLE register"},
    {"", "reg-pwr-mgmt-1", "PWR_MGMT_1 register"},
    {"", "reg-who-am-i", "WHO_AM_I register"},
    {"", "reg-data", "Data register"},
    {"", "reg-reserved", "Reserved register"},
    {"", "bit-fs-sel", "FS_SEL (gyro full scale select)"},
    {"", "bit-afs-sel", "AFS_SEL (accel full scale select)"},
    {"", "bit-clk-sel", "CLK_SEL (clock select)"},
    {"", "bit-sleep", "SLEEP bit"},
    {"", "bit-cycle", "CYCLE bit"},
    {"", "bit-temp-dis", "TEMP_DIS bit"},
    {"", "bit-reset", "DEVICE_RESET bit"},
    {"", "bit-dlpf", "Digital low pass filter bits"},
    {"", "bit-ext-sync", "External sync bits"},
    {"", "bit-int-en", "Interrupt enable bits"},
    {"", "bit-xg-st", "XG_ST (gyro X self-test)"},
    {"", "bit-yg-st", "YG_ST (gyro Y self-test)"},
    {"", "bit-zg-st", "ZG_ST (gyro Z self-test)"},
    {"", "bit-xa-st", "XA_ST (accel X self-test)"},
    {"", "bit-ya-st", "YA_ST (accel Y self-test)"},
    {"", "accel-data", "Accelerometer data"},
    {"", "gyro-data", "Gyroscope data"},
    {"", "temp-data", "Temperature data"},
    {"", "reg-write", "Register write"},
    {"", "warning", "Warning"},
};

static const int mpu6050_row_bits_classes[] = {
    ANN_BIT_FS_SEL, ANN_BIT_AFS_SEL, ANN_BIT_CLK_SEL, ANN_BIT_SLEEP,
    ANN_BIT_CYCLE, ANN_BIT_TEMP_DIS, ANN_BIT_RESET, ANN_BIT_DLPF,
    ANN_BIT_EXT_SYNC, ANN_BIT_INT_EN, ANN_BIT_XG_ST, ANN_BIT_YG_ST,
    ANN_BIT_ZG_ST, ANN_BIT_XA_ST, ANN_BIT_YA_ST
};
static const int mpu6050_row_regs_classes[] = {
    ANN_REG_SMPLRT_DIV, ANN_REG_CONFIG, ANN_REG_GYRO_CONFIG,
    ANN_REG_ACCEL_CONFIG, ANN_REG_INT_ENABLE, ANN_REG_PWR_MGMT_1,
    ANN_REG_WHO_AM_I, ANN_REG_DATA, ANN_REG_RESERVED
};
static const int mpu6050_row_accel_gyro_classes[] = {
    ANN_ACCEL_DATA, ANN_GYRO_DATA, ANN_TEMP_DATA, ANN_REG_WRITE
};
static const int mpu6050_row_warnings_classes[] = {ANN_WARNING};

static const struct srd_c_ann_row mpu6050_ann_rows[] = {
    {"bits", "Bits", mpu6050_row_bits_classes, 15},
    {"regs", "Registers", mpu6050_row_regs_classes, 9},
    {"accel-gyro", "AccelXYZ/Temp/GyroXYZ", mpu6050_row_accel_gyro_classes, 4},
    {"warnings", "Warnings", mpu6050_row_warnings_classes, 1},
};

static void mpu6050_handle_reg_0x1b(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_GYRO_CONFIG,
              "GYRO_CONFIG", "GYR", "G");

    char buf[64];
    const char *xg = (b & (1 << 7)) ? "self test on" : "self test off";
    snprintf(buf, sizeof(buf), "XG_ST: %s", xg);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_XG_ST, buf);

    const char *yg = (b & (1 << 6)) ? "self test on" : "self test off";
    snprintf(buf, sizeof(buf), "YG_ST: %s", yg);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_YG_ST, buf);

    const char *zg = (b & (1 << 5)) ? "self test on" : "self test off";
    snprintf(buf, sizeof(buf), "ZG_ST: %s", zg);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_ZG_ST, buf);

    const char *fs_sel;
    switch (b & (0b11 << 3)) {
    case 0b00000: fs_sel = "± 250 °/s"; break;
    case 0b01000: fs_sel = "± 500 °/s"; break;
    case 0b10000: fs_sel = "± 1000 °/s"; break;
    case 0b11000: fs_sel = "± 2000 °/s"; break;
    default: fs_sel = "unknown"; break;
    }
    snprintf(buf, sizeof(buf), "FS_SEL: %s", fs_sel);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_FS_SEL, buf);
}

static void mpu6050_handle_reg_0x1c(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_ACCEL_CONFIG,
              "ACCEL_CONFIG", "ACC", "A");

    char buf[64];
    const char *xa = (b & (1 << 7)) ? "self test on" : "self test off";
    snprintf(buf, sizeof(buf), "XA_ST: %s", xa);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_XA_ST, buf);

    const char *ya = (b & (1 << 6)) ? "self test on" : "self test off";
    snprintf(buf, sizeof(buf), "YA_ST: %s", ya);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_YA_ST, buf);

    const char *afs_sel;
    switch (b & (0b11 << 3)) {
    case 0b00000: afs_sel = "± 2g"; break;
    case 0b01000: afs_sel = "± 4g"; break;
    case 0b10000: afs_sel = "± 8g"; break;
    case 0b11000: afs_sel = "± 16g"; break;
    default: afs_sel = "unknown"; break;
    }
    snprintf(buf, sizeof(buf), "AFS_SEL: %s", afs_sel);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_AFS_SEL, buf);
}

static void mpu6050_handle_reg_0x38(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_INT_ENABLE,
              "INT_ENABLE", "INT", "I");

    char buf[64];
    snprintf(buf, sizeof(buf), "FIFO_OFLOW_EN: %d", (b >> 4) & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_INT_EN, buf);
    snprintf(buf, sizeof(buf), "I2C_MST_INT: %d", (b >> 3) & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_INT_EN, buf);
    snprintf(buf, sizeof(buf), "DATA_RDY_INT: %d", b & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_INT_EN, buf);
}

static void mpu6050_handle_reg_0x6b(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_PWR_MGMT_1,
              "PWR_MGMT_1", "PWR", "P");

    char buf[64];
    snprintf(buf, sizeof(buf), "DEVICE_RESET: %d", (b >> 7) & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_RESET, buf);
    snprintf(buf, sizeof(buf), "SLEEP: %d", (b >> 6) & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_SLEEP, buf);
    snprintf(buf, sizeof(buf), "CYCLE: %d", (b >> 5) & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_CYCLE, buf);
    snprintf(buf, sizeof(buf), "TEMP_DIS: %d", (b >> 3) & 1);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_TEMP_DIS, buf);
    snprintf(buf, sizeof(buf), "CLKSEL: %d", b & 0x07);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_CLK_SEL, buf);
}

static void mpu6050_handle_reg_0x75(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    c_put(di, s->ss, s->es, s->out_ann, ANN_REG_WHO_AM_I,
              "WHO_AM_I", "WHO", "W");
    char buf[64];
    snprintf(buf, sizeof(buf), "WHO_AM_I: 0x%02X", b);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT_XG_ST, buf);
}

static void mpu6050_handle_reg_data(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    char buf[64];

    switch (s->reg) {
    case 0x3b:
        s->acc_x = (int8_t)b;
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "ACCEL_XOUT[15:8]");
        break;
    case 0x3c:
        s->acc_x = (s->acc_x << 8) | b;
        snprintf(buf, sizeof(buf), "ACCEL_XOUT: %d", s->acc_x);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_ACCEL_DATA, buf);
        break;
    case 0x3d:
        s->acc_y = (int8_t)b;
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "ACCEL_YOUT[15:8]");
        break;
    case 0x3e:
        s->acc_y = (s->acc_y << 8) | b;
        snprintf(buf, sizeof(buf), "ACCEL_YOUT: %d", s->acc_y);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_ACCEL_DATA, buf);
        break;
    case 0x3f:
        s->acc_z = (int8_t)b;
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "ACCEL_ZOUT[15:8]");
        break;
    case 0x40:
        s->acc_z = (s->acc_z << 8) | b;
        snprintf(buf, sizeof(buf), "ACCEL_ZOUT: %d", s->acc_z);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_ACCEL_DATA, buf);
        break;
    case 0x41:
        s->temp = (int16_t)(b << 8);
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "TEMP_OUT[15:8]");
        break;
    case 0x42: {
        s->temp = (int16_t)(s->temp | b);
        double temp_c = (double)s->temp / 340.0 + 36.53;
        snprintf(buf, sizeof(buf), "TEMP: %.1f°C", temp_c);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_TEMP_DATA, buf);
        break;
    }
    case 0x43:
        s->gyro_x = (int8_t)b;
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "GYRO_XOUT[15:8]");
        break;
    case 0x44:
        s->gyro_x = (s->gyro_x << 8) | b;
        snprintf(buf, sizeof(buf), "GYRO_XOUT: %d", s->gyro_x);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_GYRO_DATA, buf);
        break;
    case 0x45:
        s->gyro_y = (int8_t)b;
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "GYRO_YOUT[15:8]");
        break;
    case 0x46:
        s->gyro_y = (s->gyro_y << 8) | b;
        snprintf(buf, sizeof(buf), "GYRO_YOUT: %d", s->gyro_y);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_GYRO_DATA, buf);
        break;
    case 0x47:
        s->gyro_z = (int8_t)b;
        s->ss_block = s->ss;
        c_put(di, s->ss, s->es, s->out_ann, ANN_REG_DATA, "GYRO_ZOUT[15:8]");
        break;
    case 0x48:
        s->gyro_z = (s->gyro_z << 8) | b;
        snprintf(buf, sizeof(buf), "GYRO_ZOUT: %d", s->gyro_z);
        c_put(di, s->ss_block, s->es, s->out_ann, ANN_GYRO_DATA, buf);
        break;
    default:
        break;
    }

    if (s->reg >= 0x3b && s->reg <= 0x48)
        s->reg++;
}

static void mpu6050_handle_reg(struct srd_decoder_inst *di, mpu6050_priv *s, uint8_t b)
{
    switch (s->reg) {
    case 0x1b: mpu6050_handle_reg_0x1b(di, s, b); break;
    case 0x1c: mpu6050_handle_reg_0x1c(di, s, b); break;
    case 0x38: mpu6050_handle_reg_0x38(di, s, b); break;
    case 0x6b: mpu6050_handle_reg_0x6b(di, s, b); break;
    case 0x75: mpu6050_handle_reg_0x75(di, s, b); break;
    default:
        if (s->reg >= 0x3b && s->reg <= 0x48) {
            mpu6050_handle_reg_data(di, s, b);
        } else {
            c_put(di, s->ss, s->es, s->out_ann, ANN_REG_RESERVED, "Reserved register");
        }
        break;
    }
}

static void mpu6050_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    mpu6050_priv *s = (mpu6050_priv *)c_decoder_get_private(di);
    if (!s)
        return;

    s->ss = start_sample;
    s->es = end_sample;

    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (s->state == MPU6050_IDLE) {
        if (strcmp(cmd, "START") == 0)
            s->state = MPU6050_GET_SLAVE_ADDR;
    } else if (s->state == MPU6050_GET_SLAVE_ADDR) {
        if (strcmp(cmd, "ADDRESS WRITE") == 0 || strcmp(cmd, "ADDRESS READ") == 0) {
            if (databyte != MPU6050_I2C_ADDRESS) {
                if (databyte != 0x7F) {
                    char buf[64];
                    snprintf(buf, sizeof(buf),
                        "Ignoring non-MPU6050 data (slave 0x%02X)", databyte);
                    c_put(di, s->ss, s->es, s->out_ann, ANN_WARNING, buf);
                }
                s->state = MPU6050_IDLE;
                return;
            }
            s->state = MPU6050_GET_REG_ADDR;
        }
    } else if (s->state == MPU6050_GET_REG_ADDR) {
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = MPU6050_GET_SLAVE_ADDR;
        } else if (strcmp(cmd, "STOP") == 0) {
            s->state = MPU6050_IDLE;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reg = databyte;
            s->state = MPU6050_WRITE_REGS;
        } else if (strcmp(cmd, "DATA READ") == 0) {
            mpu6050_handle_reg(di, s, databyte);
        }
    } else if (s->state == MPU6050_WRITE_REGS) {
        if (strcmp(cmd, "STOP") == 0) {
            s->state = MPU6050_IDLE;
        } else if (strcmp(cmd, "START REPEAT") == 0) {
            s->state = MPU6050_GET_SLAVE_ADDR;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            mpu6050_handle_reg(di, s, databyte);
            c_put(di, s->ss, s->es, s->out_ann, ANN_REG_WRITE, "Register write");
        }
    }
}

static void mpu6050_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(mpu6050_priv)));
    }
    mpu6050_priv *s = (mpu6050_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(mpu6050_priv));
    s->state = MPU6050_IDLE;
}

static void mpu6050_start(struct srd_decoder_inst *di)
{
    mpu6050_priv *s = (mpu6050_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "mpu6050");
}

static void mpu6050_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void mpu6050_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder mpu6050_c_decoder = {
    .id = "mpu6050_c",
    .name = "MPU6050(C)",
    .longname = "InvenSense MPU6050 (C)",
    .desc = "Accelerometer module protocol. (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = mpu6050_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = mpu6050_ann_rows,
    .inputs = mpu6050_inputs,
    .num_inputs = 1,
    .outputs = mpu6050_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = mpu6050_tags,
    .num_tags = 1,
    .reset = mpu6050_reset,
    .start = mpu6050_start,
    .decode = mpu6050_decode,
    .destroy = mpu6050_destroy,
    .decode_upper = mpu6050_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &mpu6050_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}