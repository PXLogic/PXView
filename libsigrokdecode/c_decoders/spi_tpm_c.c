/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * TPM SPI transaction decoder with VMK extraction (C implementation).
 * Ported from Python decoder spi_tpm.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation enumeration ===== */
enum {
    ANN_READ = 0,
    ANN_WRITE,
    ANN_ADDRESS,
    ANN_WAIT,
    ANN_DATA,
    ANN_VMK,
    NUM_ANN = 6,
};

/* ===== Transaction state enumeration ===== */
enum tpm_transaction_state {
    TPM_TS_NONE = 0,
    TPM_TS_READ,
    TPM_TS_WRITE,
    TPM_TS_READ_ADDRESS,
    TPM_TS_WAIT,
    TPM_TS_TRANSFER_DATA,
};

/* ===== FIFO register range lookup ===== */
typedef struct {
    uint16_t start;
    uint16_t end;
    const char *name;
} tpm_fifo_reg_range;

/* TPM 2.0 FIFO registers */
static const tpm_fifo_reg_range tpm2_fifo_regs[] = {
    {0x0000, 0x0000, "TPM_ACCESS_0"},
    {0x0001, 0x0007, "Reserved"},
    {0x0008, 0x000b, "TPM_INT_ENABLE_0"},
    {0x000c, 0x000c, "TPM_INT_VECTOR_0"},
    {0x000d, 0x000f, "Reserved"},
    {0x0010, 0x0013, "TPM_INT_STATUS_0"},
    {0x0014, 0x0017, "TPM_INTF_CAPABILITY_0"},
    {0x0018, 0x001b, "TPM_STS_0"},
    {0x001c, 0x0023, "Reserved"},
    {0x0024, 0x0027, "TPM_DATA_FIFO_0"},
    {0x0028, 0x002f, "Reserved"},
    {0x0030, 0x0033, "TPM_INTERFACE_ID_0"},
    {0x0034, 0x007f, "Reserved"},
    {0x0080, 0x0083, "TPM_XDATA_FIFO_0"},
    {0x0084, 0x0881, "Reserved"},
    {0x0f00, 0x0f03, "TPM_DID_VID_0"},
    {0x0f04, 0x0f04, "TPM_RID_0"},
    {0x0f90, 0x0fff, "Reserved"},
    {0x1000, 0x1000, "TPM_ACCESS_1"},
    {0x1001, 0x1007, "Reserved"},
    {0x1008, 0x100b, "TPM_INT_ENABLE_1"},
    {0x100c, 0x100c, "TPM_INT_VECTOR_1"},
    {0x100d, 0x100f, "Reserved"},
    {0x1010, 0x1013, "TPM_INT_STATUS_1"},
    {0x1014, 0x1017, "TPM_INTF_CAPABILITY_1"},
    {0x1018, 0x101b, "TPM_STS_1"},
    {0x101c, 0x1023, "Reserved"},
    {0x1024, 0x1027, "TPM_DATA_FIFO_1"},
    {0x1028, 0x102f, "Reserved"},
    {0x1030, 0x1030, "TPM_INTERFACE_ID_1"},
    {0x1037, 0x107f, "Reserved"},
    {0x1080, 0x1083, "TPM_XDATA_FIFO_1"},
    {0x1084, 0x1eff, "Reserved"},
    {0x1f00, 0x1f03, "TPM_DID_VID_1"},
    {0x1f04, 0x1f04, "TPM_RID_1"},
    {0x1f05, 0x1fff, "Reserved"},
    {0x2000, 0x2000, "TPM_ACCESS_2"},
    {0x2001, 0x2007, "Reserved"},
    {0x2008, 0x200b, "TPM_INT_ENABLE_2"},
    {0x200c, 0x200c, "TPM_INT_VECTOR_2"},
    {0x200d, 0x200f, "Reserved"},
    {0x2010, 0x2013, "TPM_INT_STATUS_2"},
    {0x2014, 0x2017, "TPM_INTF_CAPABILITY_2"},
    {0x2018, 0x201b, "TPM_STS_2"},
    {0x201c, 0x2023, "Reserved"},
    {0x2024, 0x2027, "TPM_DATA_FIFO_2"},
    {0x2028, 0x202f, "Reserved"},
    {0x2030, 0x2033, "TPM_INTERFACE_ID_2"},
    {0x2034, 0x207f, "Reserved"},
    {0x2080, 0x2083, "TPM_XDATA_FIFO_2"},
    {0x2084, 0x2eff, "Reserved"},
    {0x2f00, 0x2f03, "TPM_DID_VID_2"},
    {0x2f04, 0x2f04, "TPM_RID_2"},
    {0x2f05, 0x2fff, "Reserved"},
    {0x3000, 0x3000, "TPM_ACCESS_3"},
    {0x3001, 0x3007, "Reserved"},
    {0x3008, 0x300b, "TPM_INT_ENABLE_3"},
    {0x300c, 0x300c, "TPM_INT_VECTOR_3"},
    {0x300d, 0x300f, "Reserved"},
    {0x3010, 0x3013, "TPM_INT_STATUS_3"},
    {0x3014, 0x3017, "TPM_INTF_CAPABILITY_3"},
    {0x3018, 0x301b, "TPM_STS_3"},
    {0x301c, 0x3023, "Reserved"},
    {0x3024, 0x3027, "TPM_DATA_FIFO_3"},
    {0x3028, 0x302f, "Reserved"},
    {0x3030, 0x3033, "TPM_INTERFACE_ID_3"},
    {0x3034, 0x307f, "Reserved"},
    {0x3080, 0x3083, "TPM_XDATA_FIFO_3"},
    {0x3084, 0x3eff, "Reserved"},
    {0x3f00, 0x3f03, "TPM_DID_VID_3"},
    {0x3f04, 0x3f04, "TPM_RID_3"},
    {0x3f05, 0x3fff, "Reserved"},
    {0x4000, 0x4000, "TPM_ACCESS_4"},
    {0x4001, 0x4007, "Reserved"},
    {0x4008, 0x400b, "TPM_INT_ENABLE_4"},
    {0x400c, 0x400c, "TPM_INT_VECTOR_4"},
    {0x400d, 0x400f, "Reserved"},
    {0x4010, 0x4013, "TPM_INT_STATUS_4"},
    {0x4014, 0x4017, "TPM_INTF_CAPABILITY_4"},
    {0x4018, 0x401b, "TPM_STS_4"},
    {0x401c, 0x401f, "Reserved"},
    {0x4020, 0x4023, "TPM_HASH_END"},
    {0x4024, 0x4027, "TPM_DATA_FIFO_4"},
    {0x4028, 0x402f, "TPM_HASH_START"},
    {0x4030, 0x4033, "TPM_INTERFACE_ID_4"},
    {0x4034, 0x407f, "Reserved"},
    {0x4080, 0x4083, "TPM_XDATA_FIFO_4"},
    {0x4084, 0x4eff, "Reserved"},
    {0x4f00, 0x4f03, "TPM_DID_VID_4"},
    {0x4f04, 0x4f04, "TPM_RID_4"},
    {0x4f05, 0x4fff, "Reserved"},
    {0x5000, 0x5fff, "Reserved"},
    {0xFFFF, 0xFFFF, NULL},
};

/* TPM 1.2 FIFO registers */
static const tpm_fifo_reg_range tpm1_fifo_regs[] = {
    {0x0000, 0x0000, "TPM_ACCESS_0"},
    {0x0008, 0x000b, "TPM_INT_ENABLE_0"},
    {0x000c, 0x000c, "TPM_INT_VECTOR_0"},
    {0x0010, 0x0013, "TPM_INT_STATUS_0"},
    {0x0014, 0x0017, "TPM_INTF_CAPABILITY_0"},
    {0x0018, 0x001a, "TPM_STS_0"},
    {0x0024, 0x0027, "TPM_DATA_FIFO_0"},
    {0x0080, 0x0083, "TPM_XDATA_FIFO_0"},
    {0x0084, 0x00bf, "Reserved"},
    {0x0f00, 0x0f03, "TPM_DID_VID_0"},
    {0x0f04, 0x0f04, "TPM_RID_0"},
    {0x0f05, 0x0f7f, "Reserved"},
    {0x0f80, 0x0f80, "FIRST_LEGACY_ADDRESS_0"},
    {0x0f84, 0x0f84, "FIRST_LEGACY_ADDRESS_EXTENSION_0"},
    {0x0f88, 0x0f88, "SECOND_LEGACY_ADDRESS_0"},
    {0x0f8c, 0x0f8c, "SECOND_LEGACY_ADDRESS_EXTENSION_0"},
    {0x0f90, 0x0fff, "VENDOR_DEFINED"},
    {0x1000, 0x1000, "TPM_ACCESS_1"},
    {0x1008, 0x100b, "TPM_INT_ENABLE_1"},
    {0x100c, 0x100c, "TPM_INT_VECTOR_1"},
    {0x1010, 0x1013, "TPM_INT_STATUS_1"},
    {0x1014, 0x1017, "TPM_INTF_CAPABILITY_1"},
    {0x1018, 0x101a, "TPM_STS_1"},
    {0x1024, 0x1027, "TPM_DATA_FIFO_1"},
    {0x1080, 0x1083, "TPM_XDATA_FIFO_1"},
    {0x1084, 0x10bf, "Reserved"},
    {0x1f00, 0x1f03, "TPM_DID_VID_1"},
    {0x1f04, 0x1f04, "TPM_RID_1"},
    {0x1f05, 0x1f7f, "Reserved"},
    {0x1f90, 0x1fff, "VENDOR_DEFINED"},
    {0x2000, 0x2000, "TPM_ACCESS_2"},
    {0x2008, 0x200b, "TPM_INT_ENABLE_2"},
    {0x200c, 0x200c, "TPM_INT_VECTOR_2"},
    {0x2010, 0x2013, "TPM_INT_STATUS_2"},
    {0x2014, 0x2017, "TPM_INTF_CAPABILITY_2"},
    {0x2018, 0x201a, "TPM_STS_2"},
    {0x2024, 0x2027, "TPM_DATA_FIFO_2"},
    {0x2080, 0x2083, "TPM_XDATA_FIFO_2"},
    {0x2084, 0x20bf, "Reserved"},
    {0x2f00, 0x2f03, "TPM_DID_VID_2"},
    {0x2f04, 0x2f04, "TPM_RID_2"},
    {0x2f90, 0x2fff, "VENDOR_DEFINED"},
    {0x3000, 0x3000, "TPM_ACCESS_3"},
    {0x3008, 0x300b, "TPM_INT_ENABLE_3"},
    {0x300c, 0x300c, "TPM_INT_VECTOR_3"},
    {0x3010, 0x3013, "TPM_INT_STATUS_3"},
    {0x3014, 0x3017, "TPM_INTF_CAPABILITY_3"},
    {0x3018, 0x301a, "TPM_STS_3"},
    {0x3024, 0x3027, "TPM_DATA_FIFO_3"},
    {0x3080, 0x3083, "TPM_XDATA_FIFO_3"},
    {0x3084, 0x30bf, "Reserved"},
    {0x3f00, 0x3f03, "TPM_DID_VID_3"},
    {0x3f04, 0x3f04, "TPM_RID_3"},
    {0x3f90, 0x3fff, "VENDOR_DEFINED"},
    {0x4000, 0x4000, "TPM_ACCESS_4"},
    {0x4008, 0x400b, "TPM_INT_ENABLE_4"},
    {0x400c, 0x400c, "TPM_INT_VECTOR_4"},
    {0x4010, 0x4013, "TPM_INT_STATUS_4"},
    {0x4014, 0x4017, "TPM_INTF_CAPABILITY_4"},
    {0x4018, 0x401a, "TPM_STS_4"},
    {0x4020, 0x4020, "TPM_HASH_END"},
    {0x4024, 0x4027, "TPM_DATA_FIFO_4"},
    {0x4028, 0x4028, "TPM_HASH_START"},
    {0x4080, 0x4083, "TPM_XDATA_FIFO_4"},
    {0x4084, 0x40bf, "Reserved"},
    {0x4f00, 0x4f03, "TPM_DID_VID_4"},
    {0x4f04, 0x4f04, "TPM_RID_4"},
    {0x4f90, 0x4fff, "VENDOR_DEFINED"},
    {0xFFFF, 0xFFFF, NULL},
};

/* ===== State structure ===== */
typedef struct {
    enum tpm_transaction_state state;
    int operation;          /* 0x00=WRITE, 0x80=READ */
    int transfer_size;
    uint8_t address[3];
    int addr_count;
    uint8_t data[256];
    int data_count;
    int wait_count;
    uint64_t ss_op, es_op;
    uint64_t ss_addr, es_addr;
    uint64_t ss_data, es_data;
    uint64_t ss_wait, es_wait;
    
    /* VMK extraction */
    uint8_t vmk_queue[12];
    int vmk_queue_count;
    uint64_t vmk_queue_ss[12];
    int saving_vmk;
    uint8_t vmk[32];
    int vmk_count;
    uint64_t vmk_ss, vmk_es;
    /* Config */
    int tpm_version;        /* 0=2.0, 1=1.2 */
    uint8_t end_wait;       /* 0x01 */
    uint8_t wait_mask;      /* 0x00 (2.0) or 0xFE (1.2) */
    int out_ann;
    uint64_t ss;
    uint64_t es;
} spi_tpm_state;

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
static const char *spi_tpm_inputs[] = {"spi", NULL};
static const char *spi_tpm_tags[] = {"IC", "TPM", "BitLocker", NULL};

static struct srd_decoder_option spi_tpm_options[] = {
    {"tpm_version", "dec_spi_tpm_opt_tpm_version", "TPM Version 1.2 or 2.0", NULL, NULL},
};

static const char *spi_tpm_ann_labels[][3] = {
    {"", "Read", "Read register operation"},
    {"", "Write", "Write register operation"},
    {"", "Address", "Register address"},
    {"", "Wait", "Wait"},
    {"", "Data", "Data"},
    {"", "VMK", "Extracted BitLocker VMK"},
};

static const int spi_tpm_row_transactions_classes[] = {ANN_READ, ANN_WRITE, ANN_ADDRESS, ANN_WAIT, ANN_DATA, -1};
static const int spi_tpm_row_vmk_classes[] = {ANN_VMK, -1};

static const struct srd_c_ann_row spi_tpm_ann_rows[] = {
    {"Transactions", "TPM transactions", spi_tpm_row_transactions_classes, 5},
    {"B-VMK", "BitLocker Volume Master Key", spi_tpm_row_vmk_classes, 1},
};

/* ===== Helper: find register name ===== */
static const char *spi_tpm_find_register(uint16_t addr, int tpm_version)
{
    const tpm_fifo_reg_range *regs = (tpm_version == 0) ? tpm2_fifo_regs : tpm1_fifo_regs;
    for (int i = 0; regs[i].name != NULL; i++) {
        if (addr >= regs[i].start && addr <= regs[i].end)
            return regs[i].name;
    }
    return "Unknown";
}

/* ===== Helper: check if address is TPM_DATA_FIFO_0 ===== */
static int spi_tpm_is_data_fifo0(uint16_t addr, int tpm_version)
{
    const char *name = spi_tpm_find_register(addr, tpm_version);
    return (strcmp(name, "TPM_DATA_FIFO_0") == 0) ? 1 : 0;
}

/* ===== Helper: check VMK header pattern ===== */
static int spi_tpm_check_vmk_header(const uint8_t *queue)
{
    /* Pattern: 2c 000[0-6] 000[1-9] 000[0-1] 000[0-5] 20 00 00 */
    if (queue[0] != 0x2c) return 0;
    if ((queue[1] & 0xf0) != 0x00 || (queue[1] & 0x0f) > 6) return 0;
    if ((queue[2] & 0xf0) != 0x00 || (queue[3] & 0xf0) != 0x00 || (queue[3] & 0x0f) < 1 || (queue[3] & 0x0f) > 9) return 0;
    if ((queue[4] & 0xf0) != 0x00 || (queue[5] & 0xf0) != 0x00 || (queue[5] & 0x0f) > 1) return 0;
    if ((queue[6] & 0xf0) != 0x00 || (queue[7] & 0xf0) != 0x00 || (queue[7] & 0x0f) > 5) return 0;
    if (queue[8] != 0x20 || queue[9] != 0x00 || queue[10] != 0x00) return 0;
    return 1;
}

/* ===== Helper: recover VMK ===== */
static void spi_tpm_recover_vmk(struct srd_decoder_inst *di, spi_tpm_state *s, uint8_t miso)
{
    if (!s->saving_vmk) {
        /* Add to circular buffer if reading from TPM_DATA_FIFO_0 */
        uint16_t addr = ((uint16_t)s->address[0] << 8) | s->address[1];
        addr = (addr << 8) | s->address[2]; /* Actually 3-byte address */
        uint16_t addr16 = ((uint16_t)s->address[0] << 8) | s->address[1];
        addr16 &= 0xFFFF;

        if (spi_tpm_is_data_fifo0(addr16, s->tpm_version)) {
            /* Push into ring buffer */
            if (s->vmk_queue_count < 12) {
                s->vmk_queue[s->vmk_queue_count] = miso;
                s->vmk_queue_ss[s->vmk_queue_count] = s->ss;
                s->vmk_queue_count++;
            } else {
                /* Shift left */
                for (int i = 0; i < 11; i++) {
                    s->vmk_queue[i] = s->vmk_queue[i + 1];
                    s->vmk_queue_ss[i] = s->vmk_queue_ss[i + 1];
                }
                s->vmk_queue[11] = miso;
                s->vmk_queue_ss[11] = s->ss;
            }

            /* Check for VMK header */
            if (s->vmk_queue_count >= 12) {
                if (spi_tpm_check_vmk_header(s->vmk_queue)) {
                    char hex[32];
                    int pos = 0;
                    for (int i = 0; i < 12; i++)
                        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x", s->vmk_queue[i]);
                    char buf[256];
                    snprintf(buf, sizeof(buf), "VMK header: %s", hex);
                    c_put(di, s->vmk_queue_ss[0], s->es, s->out_ann, ANN_VMK, buf);
                    s->saving_vmk = 1;
                    s->vmk_count = 0;
                }
            }
        }
    } else {
        /* Collecting VMK bytes */
        uint16_t addr16 = ((uint16_t)s->address[0] << 8) | s->address[1];
        addr16 &= 0xFFFF;

        if (spi_tpm_is_data_fifo0(addr16, s->tpm_version)) {
            if (s->vmk_count == 0)
                s->vmk_ss = s->ss;
            if (s->vmk_count < 32) {
                s->vmk[s->vmk_count++] = miso;
                s->vmk_es = s->es;
            }
            if (s->vmk_count >= 32) {
                char hex[72];
                int pos = 0;
                for (int i = 0; i < 32; i++)
                    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x", s->vmk[i]);
                char buf[256];
                snprintf(buf, sizeof(buf), "VMK: %s", hex);
                c_put(di, s->vmk_ss, s->vmk_es, s->out_ann, ANN_VMK, buf);
                s->saving_vmk = 0;
            }
        }
    }
}

/* ===== Helper: end current transaction ===== */
static void spi_tpm_end_current_transaction(struct srd_decoder_inst *di, spi_tpm_state *s)
{
    (void)di;
    s->state = TPM_TS_NONE;
    s->addr_count = 0;
    s->data_count = 0;
    s->wait_count = 0;
}

/* ===== Helper: output transaction annotations ===== */
static void spi_tpm_output_transaction(struct srd_decoder_inst *di, spi_tpm_state *s)
{
    uint16_t addr16 = ((uint16_t)s->address[0] << 8) | s->address[1];
    addr16 &= 0xFFFF;
    const char *reg_name = spi_tpm_find_register(addr16, s->tpm_version);

    /* Data hex string */
    char data_str[512];
    int pos = 0;
    for (int i = 0; i < s->data_count && pos < (int)sizeof(data_str) - 3; i++)
        pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%02x", s->data[i]);

    /* Operation annotation */
    int op_ann = (s->operation == 0x80) ? ANN_READ : ANN_WRITE;
    const char *op_str = (s->operation == 0x80) ? "Read" : "Write";
    c_put(di, s->ss_op, s->es_op, s->out_ann, op_ann, op_str);

    /* Address annotation */
    char addr_buf[128];
    snprintf(addr_buf, sizeof(addr_buf), "Register: %s", reg_name);
    c_put(di, s->es_op, s->es_addr, s->out_ann, ANN_ADDRESS, addr_buf);

    /* Wait annotation */
    if (s->wait_count > 0) {
        c_put(di, s->es_addr, s->es_wait, s->out_ann, ANN_WAIT, "Wait");
        c_put(di, s->es_wait, s->es_data, s->out_ann, ANN_DATA, data_str);
    } else {
        c_put(di, s->es_addr, s->es_data, s->out_ann, ANN_DATA, data_str);
    }

    spi_tpm_end_current_transaction(di, s);
}

/* ===== recv_proto ===== */
static void spi_tpm_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    spi_tpm_state *s = (spi_tpm_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        spi_tpm_end_current_transaction(di, s);
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    uint8_t mosi = 0, miso = 0;
    spi_proto_get_mosi(fields, n_fields, &mosi);
    spi_proto_get_miso(fields, n_fields, &miso);

    switch (s->state) {
    case TPM_TS_NONE: {
        /* Determine transaction type from first MOSI byte */
        if (mosi & 0x80) {
            s->operation = 0x80; /* READ */
            s->state = TPM_TS_READ;
        } else {
            s->operation = 0x00; /* WRITE */
            s->state = TPM_TS_WRITE;
        }
        s->transfer_size = (mosi & 0x3f) + 1;
        s->ss_op = start_sample;
        s->es_op = end_sample;
        s->addr_count = 0;
        s->data_count = 0;
        s->wait_count = 0;
        s->state = TPM_TS_READ_ADDRESS;
        break;
    }

    case TPM_TS_READ_ADDRESS: {
        s->address[s->addr_count++] = mosi;
        if (s->addr_count >= 3) {
            s->es_addr = end_sample;
            if (miso == s->wait_mask) {
                s->state = TPM_TS_WAIT;
            } else {
                s->state = TPM_TS_TRANSFER_DATA;
            }
        }
        break;
    }

    case TPM_TS_WAIT: {
        s->wait_count++;
        if (miso == s->end_wait) {
            s->es_wait = end_sample;
            s->state = TPM_TS_TRANSFER_DATA;
        }
        break;
    }

    case TPM_TS_TRANSFER_DATA: {
        s->es_data = end_sample;
        if (s->operation == 0x80) {
            /* READ: data from MISO */
            if (s->data_count < (int)sizeof(s->data))
                s->data[s->data_count++] = miso;
            spi_tpm_recover_vmk(di, s, miso);
        } else {
            /* WRITE: data from MOSI */
            if (s->data_count < (int)sizeof(s->data))
                s->data[s->data_count++] = mosi;
        }

        if (s->data_count >= s->transfer_size) {
            spi_tpm_output_transaction(di, s);
        }
        break;
    }

    default:
        break;
    }
}

/* ===== Lifecycle callbacks ===== */
static void spi_tpm_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(spi_tpm_state)));
    }
    spi_tpm_state *s = (spi_tpm_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(spi_tpm_state));
    s->end_wait = 0x01;
    s->state = TPM_TS_NONE;
}

static void spi_tpm_start(struct srd_decoder_inst *di)
{
    spi_tpm_state *s = (spi_tpm_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "spi_tpm");

    const char *ver = c_opt_str(di, "tpm_version", "2.0");
    if (strcmp(ver, "1.2") == 0) {
        s->tpm_version = 1;
        s->wait_mask = 0xFE;
    } else {
        s->tpm_version = 0;
        s->wait_mask = 0x00;
    }
}

static void spi_tpm_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void spi_tpm_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== Decoder struct ===== */
struct srd_c_decoder spi_tpm_c_decoder = {
    .id = "spi_tpm_c",
    .name = "SPI TPM(C)",
    .longname = "SPI TPM transactions (C)",
    .desc = "TPM SPI transaction decoder with VMK extraction (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = spi_tpm_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = spi_tpm_ann_labels,
    .num_annotation_rows = 2,
    .annotation_rows = spi_tpm_ann_rows,
    .inputs = spi_tpm_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = spi_tpm_tags,
    .num_tags = 3,
    .reset = spi_tpm_reset,
    .start = spi_tpm_start,
    .decode = spi_tpm_decode,
    .destroy = spi_tpm_destroy,
    .decode_upper = spi_tpm_recv_proto,
    .state_size = 0,
};

/* ===== Export functions ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    spi_tpm_options[0].def = g_variant_new_string("2.0");
    GSList *ver_vals = NULL;
    ver_vals = g_slist_append(ver_vals, g_variant_new_string("2.0"));
    ver_vals = g_slist_append(ver_vals, g_variant_new_string("1.2"));
    spi_tpm_options[0].values = ver_vals;

    return &spi_tpm_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}