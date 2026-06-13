/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv3+
 *
 * Trusted Platform Module Interface (TIS 2.0) over I2C.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation 枚举 ===== */
enum {
    ANN_ADDRESS = 0,
    ANN_DATA_READ,
    ANN_DATA_WRITE,
    ANN_TRANSACTION,
    ANN_WARNING,
    NUM_ANN,
};

/* ===== 状态枚举 ===== */
enum tpm_tis_state {
    TPM_TIS_IDLE,
    TPM_TIS_ADDR_WRITE,
    TPM_TIS_ADDR_ACK,
    TPM_TIS_REG_ADDR,
    TPM_TIS_REG_ADDR_ACK,
    TPM_TIS_WAIT_OP,
    TPM_TIS_READ_ADDR_READ,
    TPM_TIS_READ_ADDR_ACK,
    TPM_TIS_READ_DATA,
    TPM_TIS_WRITE_DATA,
    TPM_TIS_WRITE_DATA_ACK,
};

/* ===== 私有数据结构 ===== */
typedef struct {
    enum tpm_tis_state state;
    uint8_t tis_addr;
    uint8_t data[256];
    int data_len;
    int reading;
    uint64_t addr_ss, data_ss, data_es;
    uint64_t ss, es;
    
    int out_ann;
    int out_proto;
} tpm_tis_state;

/* ===== 静态数据 ===== */
static const char *tpm_tis_inputs[] = {"i2c", NULL};
static const char *tpm_tis_outputs[] = {"tpm-tis", NULL};
static const char *tpm_tis_tags[] = {"TPM", NULL};

static const char *tpm_tis_ann_labels[][3] = {
    {"", "address", "Address"},
    {"", "data_read", "Data (Read)"},
    {"", "data_write", "Data (Write)"},
    {"", "transaction", "Transaction"},
    {"", "warning", "Warning"},
};

static const int tpm_tis_row_protocol_classes[] = {ANN_ADDRESS, ANN_DATA_READ, ANN_DATA_WRITE, -1};
static const int tpm_tis_row_transactions_classes[] = {ANN_TRANSACTION, -1};
static const int tpm_tis_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row tpm_tis_ann_rows[] = {
    {"protocol", "Protocol", tpm_tis_row_protocol_classes, 3},
    {"transactions", "Transactions", tpm_tis_row_transactions_classes, 1},
    {"warnings", "Warnings", tpm_tis_row_warnings_classes, 1},
};

/* ===== 辅助函数 ===== */
static void tpm_tis_output_transaction(struct srd_decoder_inst *di,
    tpm_tis_state *s)
{
    char buf[1024];
    const char *op_long = s->reading ? "Read" : "Write";
    const char *op_short = s->reading ? "R" : "W";
    const char *arrow = s->reading ? "->" : "<-";

    /* Data hex string */
    char hex[512];
    int pos = 0;
    for (int i = 0; i < s->data_len && pos < (int)sizeof(hex) - 4; i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X", s->data[i]);

    /* Transaction annotation */
    snprintf(buf, sizeof(buf), "%s %02X %s %s", op_long, s->tis_addr, arrow, hex);
    char buf2[64];
    snprintf(buf2, sizeof(buf2), "%s %02X", op_short, s->tis_addr);
    c_put(di, s->addr_ss, s->data_es, s->out_ann, ANN_TRANSACTION, buf, buf2);

    /* Output PROTO for downstream decoders */
    if (s->out_proto >= 0) {
        unsigned char proto_data[258];
        proto_data[0] = s->reading ? 1 : 0;
        proto_data[1] = s->tis_addr;
        int dlen = s->data_len < 256 ? s->data_len : 256;
        memcpy(proto_data + 2, s->data, dlen);
        c_proto(di, s->addr_ss, s->data_es, s->out_proto,
            "TRANSACTION", C_U8(s->reading ? 1 : 0), C_U8(s->tis_addr), C_BYTES(s->data, dlen), C_END);
    }
}

/* ===== recv_proto ===== */
static void tpm_tis_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tpm_tis_state *s = (tpm_tis_state *)c_decoder_get_private(di);
    if (!s) return;
    s->ss = start_sample;
    s->es = end_sample;
    uint8_t databyte = (fields && n_fields > 0) ? fields[0].u8 : 0;

    if (strcmp(cmd, "BITS") == 0) return;

    switch (s->state) {
    case TPM_TIS_IDLE:
        if (strcmp(cmd, "START") == 0) {
            s->state = TPM_TIS_ADDR_WRITE;
            s->data_len = 0;
        }
        break;
    case TPM_TIS_ADDR_WRITE:
        if (strcmp(cmd, "ADDRESS WRITE") == 0) {
            s->addr_ss = start_sample;
            s->state = TPM_TIS_ADDR_ACK;
        } else {
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_ADDR_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->state = TPM_TIS_REG_ADDR;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_REG_ADDR:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            s->tis_addr = databyte;
            char buf[16];
            snprintf(buf, sizeof(buf), "%02X", databyte);
            c_put(di, s->addr_ss, end_sample, s->out_ann, ANN_ADDRESS, buf);
            s->state = TPM_TIS_REG_ADDR_ACK;
        } else {
            /* Unexpected command, reset with warning */
            char buf[256];
            snprintf(buf, sizeof(buf), "got I2C %s but expected DATA WRITE", cmd);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, buf);
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_REG_ADDR_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->state = TPM_TIS_WAIT_OP;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_WAIT_OP:
        if (strcmp(cmd, "START REPEAT") == 0) {
            s->reading = 1;
            s->data_len = 0;
            s->state = TPM_TIS_READ_ADDR_READ;
        } else if (strcmp(cmd, "STOP") == 0) {
            /* Read with no data - need new START */
            s->reading = 1;
            s->data_len = 0;
            s->state = TPM_TIS_IDLE;
        } else if (strcmp(cmd, "DATA WRITE") == 0) {
            s->reading = 0;
            s->data_len = 0;
            s->data[s->data_len++] = databyte;
            s->data_ss = start_sample;
            s->state = TPM_TIS_WRITE_DATA_ACK;
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "got I2C %s but expected START REPEAT, STOP, or DATA WRITE", cmd);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, buf);
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_READ_ADDR_READ:
        if (strcmp(cmd, "ADDRESS READ") == 0) {
            s->data_len = 0;
            s->data_ss = start_sample;
            s->state = TPM_TIS_READ_ADDR_ACK;
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "got I2C %s but expected ADDRESS READ", cmd);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, buf);
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_READ_ADDR_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->state = TPM_TIS_READ_DATA;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_READ_DATA:
        if (strcmp(cmd, "DATA READ") == 0) {
            if (s->data_len < (int)sizeof(s->data))
                s->data[s->data_len++] = databyte;
        } else if (strcmp(cmd, "ACK") == 0) {
            /* Continue reading */
        } else if (strcmp(cmd, "NACK") == 0) {
            /* Read complete, wait for STOP */
        } else if (strcmp(cmd, "STOP") == 0) {
            s->data_es = end_sample;
            /* Output data_read annotation */
            char hex[512];
            int pos = 0;
            for (int i = 0; i < s->data_len && pos < (int)sizeof(hex) - 4; i++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X", s->data[i]);
            c_put(di, s->data_ss, s->data_es, s->out_ann, ANN_DATA_READ, hex);
            tpm_tis_output_transaction(di, s);
            s->state = TPM_TIS_IDLE;
        } else {
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_WRITE_DATA_ACK:
        if (strcmp(cmd, "ACK") == 0) {
            s->state = TPM_TIS_WRITE_DATA;
        } else if (strcmp(cmd, "NACK") == 0) {
            s->state = TPM_TIS_IDLE;
        }
        break;
    case TPM_TIS_WRITE_DATA:
        if (strcmp(cmd, "DATA WRITE") == 0) {
            if (s->data_len < (int)sizeof(s->data))
                s->data[s->data_len++] = databyte;
            s->state = TPM_TIS_WRITE_DATA_ACK;
        } else if (strcmp(cmd, "STOP") == 0) {
            s->data_es = end_sample;
            /* Output data_write annotation */
            char hex[512];
            int pos = 0;
            for (int i = 0; i < s->data_len && pos < (int)sizeof(hex) - 4; i++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X", s->data[i]);
            c_put(di, s->data_ss, s->data_es, s->out_ann, ANN_DATA_WRITE, hex);
            tpm_tis_output_transaction(di, s);
            s->state = TPM_TIS_IDLE;
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "got I2C %s but expected DATA WRITE or STOP", cmd);
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, buf);
            s->state = TPM_TIS_IDLE;
        }
        break;
    }
}

/* ===== 生命周期回调 ===== */
static void tpm_tis_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tpm_tis_state)));
    }
    tpm_tis_state *s = (tpm_tis_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tpm_tis_state));
    s->out_proto = -1;
}

static void tpm_tis_start(struct srd_decoder_inst *di)
{
    tpm_tis_state *s = (tpm_tis_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tpm_tis_i2c");
    s->out_proto = c_reg_out(di, SRD_OUTPUT_PROTO, "tpm-tis");
}

static void tpm_tis_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tpm_tis_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== 解码器结构体 ===== */
struct srd_c_decoder tpm_tis_i2c_c_decoder = {
    .id = "tpm_tis_i2c_c",
    .name = "TPM TIS 2.0 I2C(C)",
    .longname = "Trusted Platform Module Interface (TIS 2.0) over Inter-Integrated Circuit Bus",
    .desc = "Trusted Platform Module Interface (TIS 2.0) over Inter-Integrated Circuit Bus",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = tpm_tis_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = tpm_tis_ann_rows,
    .inputs = tpm_tis_inputs,
    .num_inputs = 1,
    .outputs = tpm_tis_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = tpm_tis_tags,
    .num_tags = 1,
    .reset = tpm_tis_reset,
    .start = tpm_tis_start,
    .decode = tpm_tis_decode,
    .destroy = tpm_tis_destroy,
    .decode_upper = tpm_tis_recv_proto,
    .state_size = 0,
};

/* ===== 导出函数 ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &tpm_tis_i2c_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}