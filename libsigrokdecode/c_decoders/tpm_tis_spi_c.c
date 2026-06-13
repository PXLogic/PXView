/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv3+
 *
 * TPM TIS 2.0 over SPI protocol decoder (C implementation).
 * Ported from Python decoder tpm_tis_spi.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation enumeration ===== */
enum {
    ANN_RW_LENGTH = 0,
    ANN_ADDRESS,
    ANN_WAIT_STATE,
    ANN_DATA,
    ANN_TRANSACTION,
    ANN_WARNING,
    NUM_ANN = 6,
};

/* ===== State enumeration ===== */
enum tpm_tis_state {
    TIS_GET_RW_LENGTH = 0,
    TIS_GET_ADDR_BYTE2,
    TIS_GET_ADDR_BYTE1,
    TIS_GET_ADDR_BYTE0,
    TIS_GET_DATA,
};

/* ===== State structure ===== */
typedef struct {
    enum tpm_tis_state state;
    int reading;            /* 1=read, 0=write */
    int length;             /* data byte count */
    uint32_t addr;          /* 24-bit address */
    uint8_t addr_bytes[3];
    int addr_idx;
    uint8_t data[256];
    int data_count;
    int wait_state;         /* whether wait state detected */
    /* Sample number records */
    uint64_t rwl_ss, rwl_es;
    uint64_t addr2_ss, addr2_es;
    uint64_t addr1_ss, addr1_es;
    uint64_t addr0_ss, addr0_es;
    uint64_t data_ss, data_es;
    int out_ann;
    int out_python;
} tpm_tis_state;

/* ===== SPI DATA packet helpers ===== */
static inline int spi_proto_get_mosi(const c_field *fields, int n_fields, uint8_t *mosi_val)
{
    if (n_fields < 17 || !(fields[0].u8 & 1)) { *mosi_val = 0; return 0; }
    *mosi_val = (uint8_t)fields[1].u8; return 1;
}

static inline int spi_proto_get_miso(const c_field *fields, int n_fields, uint8_t *miso_val)
{
    if (n_fields < 17 || !((fields[0].u8 >> 1) & 1)) { *miso_val = 0; return 0; }
    *miso_val = (uint8_t)fields[9].u8; return 1;
}

/* ===== Static data ===== */
static const char *tpm_tis_spi_inputs[] = {"spi", NULL};
static const char *tpm_tis_spi_outputs[] = {"tpm-tis", NULL};
static const char *tpm_tis_spi_tags[] = {"TPM", NULL};

static const char *tpm_tis_spi_ann_labels[][3] = {
    {"", "rw-length", "RW/Length"},
    {"", "address", "Address"},
    {"", "wait-state", "Wait State"},
    {"", "data", "Data"},
    {"", "transaction", "Transaction"},
    {"", "warning", "Warning"},
};

static const int tpm_tis_row_protocol_classes[] = {ANN_RW_LENGTH, ANN_ADDRESS, ANN_WAIT_STATE, ANN_DATA, -1};
static const int tpm_tis_row_transactions_classes[] = {ANN_TRANSACTION, -1};
static const int tpm_tis_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row tpm_tis_spi_ann_rows[] = {
    {"protocol", "Protocol", tpm_tis_row_protocol_classes, 4},
    {"transactions", "Transactions", tpm_tis_row_transactions_classes, 1},
    {"warnings", "Warnings", tpm_tis_row_warnings_classes, 1},
};

/* ===== Helper: finish annotations ===== */
static void tpm_tis_finish_annotations(struct srd_decoder_inst *di, tpm_tis_state *s)
{
    /* RW/Length annotation */
    char rwl_buf[32];
    char rw_char = s->reading ? 'R' : 'W';
    snprintf(rwl_buf, sizeof(rwl_buf), "%c %d", rw_char, s->length);
    c_put(di, s->rwl_ss, s->rwl_es, s->out_ann, ANN_RW_LENGTH, rwl_buf);

    /* Address annotation */
    char addr_buf[32];
    snprintf(addr_buf, sizeof(addr_buf), "%06X", s->addr);
    c_put(di, s->addr2_ss, s->addr0_es, s->out_ann, ANN_ADDRESS, addr_buf);

    /* Wait State annotation */
    if (s->wait_state) {
        c_put(di, s->addr0_es, s->data_ss, s->out_ann, ANN_WAIT_STATE, "wait state");
    }

    /* Data annotation */
    if (s->data_count > 0) {
        char data_str[512];
        int pos = 0;
        for (int i = 0; i < s->data_count && pos < (int)sizeof(data_str) - 3; i++)
            pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%02X", s->data[i]);
        c_put(di, s->data_ss, s->data_es, s->out_ann, ANN_DATA, data_str);
    }

    /* Transaction annotation */
    {
        const char *op_long = s->reading ? "Read" : "Write";
        const char *op_arrow = s->reading ? "->" : "<-";
        char tx_buf[1024];
        if (s->data_count > 0) {
            char data_str[512];
            int pos = 0;
            for (int i = 0; i < s->data_count && pos < (int)sizeof(data_str) - 3; i++)
                pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%02X", s->data[i]);
            snprintf(tx_buf, sizeof(tx_buf), "%X %s %s", s->addr, op_arrow, data_str);
        } else {
            snprintf(tx_buf, sizeof(tx_buf), "%s %X", op_long, s->addr);
        }
        c_put(di, s->rwl_ss, s->data_es, s->out_ann, ANN_TRANSACTION, tx_buf);
    }

    /* Output python protocol data */
    if (s->out_python >= 0) {
        
        uint8_t py_data[1];
        py_data[0] = (unsigned char)s->reading;
        py_data[1] = (unsigned char)(s->addr >> 16);
        py_data[2] = (unsigned char)(s->addr >> 8);
        py_data[3] = (unsigned char)(s->addr);
        py_data[4] = (unsigned char)s->data_count;
        if (s->data_count > 0)
            memcpy(py_data + 5, s->data, s->data_count);
        c_proto(di, s->rwl_ss, s->data_es, s->out_python,
                             "TRANSACTION", C_U8(s->reading), C_U8(s->addr >> 16), C_U8(s->addr >> 8), C_U8(s->addr), C_U8(s->data_count), C_BYTES(s->data, s->data_count), C_END);
    }
}

/* ===== recv_proto ===== */
static void tpm_tis_spi_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tpm_tis_state *s = (tpm_tis_state *)c_decoder_get_private(di);
    if (!s) return;

    if (strcmp(cmd, "DATA") != 0)
        return;

    uint8_t mosi = 0, miso = 0;
    spi_proto_get_mosi(fields, n_fields, &mosi);
    spi_proto_get_miso(fields, n_fields, &miso);

    switch (s->state) {
    case TIS_GET_RW_LENGTH: {
        /* Check duplex warning: MISO should be 0 */
        if (miso != 0) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, "unexpected duplex operation");
        }

        s->reading = (mosi & 0x80) == 0x80 ? 1 : 0;
        s->length = (mosi & 0x7f) + 1;
        s->rwl_ss = start_sample;
        s->rwl_es = end_sample;
        s->addr_idx = 0;
        s->data_count = 0;
        s->wait_state = 0;
        s->state = TIS_GET_ADDR_BYTE2;
        break;
    }

    case TIS_GET_ADDR_BYTE2: {
        /* Check duplex warning */
        if (miso != 0) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, "unexpected duplex operation");
        }

        s->addr_bytes[2] = mosi;
        s->addr2_ss = start_sample;
        s->addr2_es = end_sample;
        s->state = TIS_GET_ADDR_BYTE1;
        break;
    }

    case TIS_GET_ADDR_BYTE1: {
        /* Check duplex warning */
        if (miso != 0) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, "unexpected duplex operation");
        }

        s->addr_bytes[1] = mosi;
        s->addr1_ss = start_sample;
        s->addr1_es = end_sample;
        s->state = TIS_GET_ADDR_BYTE0;
        break;
    }

    case TIS_GET_ADDR_BYTE0: {
        s->addr_bytes[0] = mosi;

        /* Check duplex warning (miso high at end of addr0 is allowed for wait) */
        s->wait_state = (miso == 0) ? 1 : 0;
        if (miso != 0 && miso != 1) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, "unexpected duplex operation");
        }

        s->addr0_ss = start_sample;
        s->addr0_es = end_sample;
        s->addr = ((uint32_t)s->addr_bytes[2] << 16) |
                  ((uint32_t)s->addr_bytes[1] << 8) |
                  (uint32_t)s->addr_bytes[0];
        s->state = TIS_GET_DATA;
        break;
    }

    case TIS_GET_DATA: {
        uint8_t data_byte, cross_byte;
        if (s->reading) {
            data_byte = miso;
            cross_byte = mosi;
        } else {
            data_byte = mosi;
            cross_byte = miso;
        }

        /* Check duplex warning */
        if (cross_byte != 0) {
            c_put(di, start_sample, end_sample, s->out_ann, ANN_WARNING, "unexpected duplex operation");
        }

        if (s->data_count == 0)
            s->data_ss = start_sample;
        s->data_es = end_sample;

        if (s->data_count < (int)sizeof(s->data))
            s->data[s->data_count++] = data_byte;

        if (s->data_count >= s->length) {
            /* Transaction complete */
            tpm_tis_finish_annotations(di, s);

            /* Reset state for next transaction */
            s->state = TIS_GET_RW_LENGTH;
            s->data_count = 0;
        }
        break;
    }

    default:
        s->state = TIS_GET_RW_LENGTH;
        break;
    }
}

/* ===== Lifecycle callbacks ===== */
static void tpm_tis_spi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tpm_tis_state)));
    }
    tpm_tis_state *s = (tpm_tis_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tpm_tis_state));
    s->state = TIS_GET_RW_LENGTH;
    s->out_python = -1;
}

static void tpm_tis_spi_start(struct srd_decoder_inst *di)
{
    tpm_tis_state *s = (tpm_tis_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tpm_tis_spi");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PYTHON, "tpm-tis");
}

static void tpm_tis_spi_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tpm_tis_spi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== Decoder struct ===== */
struct srd_c_decoder tpm_tis_spi_c_decoder = {
    .id = "tpm_tis_spi_c",
    .name = "TPM TIS 2.0 SPI(C)",
    .longname = "Trusted Platform Module Interface (TIS 2.0) over SPI (C)",
    .desc = "TPM TIS 2.0 over SPI protocol decoder (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = tpm_tis_spi_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = tpm_tis_spi_ann_rows,
    .inputs = tpm_tis_spi_inputs,
    .num_inputs = 1,
    .outputs = tpm_tis_spi_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = tpm_tis_spi_tags,
    .num_tags = 1,
    .reset = tpm_tis_spi_reset,
    .start = tpm_tis_spi_start,
    .decode = tpm_tis_spi_decode,
    .destroy = tpm_tis_spi_destroy,
    .decode_upper = tpm_tis_spi_recv_proto,
    .state_size = 0,
};

/* ===== Export functions ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &tpm_tis_spi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}