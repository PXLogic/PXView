/*
 * This file is part of the libsigrokdecode project.
 *
 * Copyright (C) 2020-2021 Tobias Peter <tobias.peter@infineon.com>
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
    ANN_REG_READ = 0,
    ANN_REG_WRITE,
    ANN_TPM_CMD,
    ANN_TPM_RSP,
    ANN_WARN,
    ANN_STATE,
    NUM_ANN,
};

#define TPM_MAX_BUFFER 4096

/* TPM STS register bits */
#define TPM_STS_X            0x0018
#define TPM_STS_stsValid     0x80
#define TPM_STS_commandReady 0x40
#define TPM_STS_tpmGo        0x20
#define TPM_STS_dataAvail    0x10
#define TPM_STS_Expect       0x08
#define TPM_STS_selfTestDone 0x04
#define TPM_STS_responseRetry 0x02

#define TPM_STS_read_mask  (TPM_STS_commandReady | TPM_STS_dataAvail | TPM_STS_Expect)
#define TPM_STS_write_mask (TPM_STS_commandReady | TPM_STS_tpmGo | TPM_STS_responseRetry)

#define TPM_DATA_FIFO_X 0x0024

enum TpmState {
    TPM_STATE_UNKNOWN = 0,
    TPM_STATE_IDLE,
    TPM_STATE_READY,
    TPM_STATE_RECEPTION,
    TPM_STATE_EXECUTION,
    TPM_STATE_COMPLETION,
};

typedef struct {
    const uint32_t code;
    const char *name;
} tpm_cmd_entry;

static const tpm_cmd_entry tpm_command_names[] = {
    {0x0000011f, "NV_UndefineSpaceSpecial"},
    {0x00000120, "EvictControl"},
    {0x00000121, "HierarchyControl"},
    {0x00000122, "NV_UndefineSpace"},
    {0x00000124, "ChangeEPS"},
    {0x00000125, "ChangePPS"},
    {0x00000126, "Clear"},
    {0x00000127, "ClearControl"},
    {0x00000128, "ClockSet"},
    {0x00000129, "HierarchyChangeAuth"},
    {0x0000012a, "NV_DefineSpace"},
    {0x0000012b, "PCR_Allocate"},
    {0x0000012c, "PCR_SetAuthPolicy"},
    {0x0000012d, "PP_Commands"},
    {0x0000012e, "SetPrimaryPolicy"},
    {0x0000012f, "FieldUpgradeStart"},
    {0x00000130, "ClockRateAdjust"},
    {0x00000131, "CreatePrimary"},
    {0x00000132, "NV_GlobalWriteLock"},
    {0x00000133, "GetCommandAuditDigest"},
    {0x00000134, "NV_Increment"},
    {0x00000135, "NV_SetBits"},
    {0x00000136, "NV_Extend"},
    {0x00000137, "NV_Write"},
    {0x00000138, "NV_WriteLock"},
    {0x00000139, "DictionaryAttackLockReset"},
    {0x0000013a, "DictionaryAttackParameters"},
    {0x0000013b, "NV_ChangeAuth"},
    {0x0000013c, "PCR_Event"},
    {0x0000013d, "PCR_Reset"},
    {0x0000013e, "SequenceComplete"},
    {0x0000013f, "SetAlgorithmSet"},
    {0x00000140, "SetCommandCodeAuditStatus"},
    {0x00000141, "FieldUpgradeData"},
    {0x00000142, "IncrementalSelfTest"},
    {0x00000143, "SelfTest"},
    {0x00000144, "Startup"},
    {0x00000145, "Shutdown"},
    {0x00000146, "StirRandom"},
    {0x00000147, "ActivateCredential"},
    {0x00000148, "Certify"},
    {0x00000149, "PolicyNV"},
    {0x0000014a, "CertifyCreation"},
    {0x0000014b, "Duplicate"},
    {0x0000014c, "GetTime"},
    {0x0000014d, "GetSessionAuditDigest"},
    {0x0000014e, "NV_Read"},
    {0x0000014f, "NV_ReadLock"},
    {0x00000150, "ObjectChangeAuth"},
    {0x00000151, "PolicySecret"},
    {0x00000152, "Rewrap"},
    {0x00000153, "Create"},
    {0x00000154, "ECDH_ZGen"},
    {0x00000155, "HMAC"},
    {0x00000156, "Import"},
    {0x00000157, "Load"},
    {0x00000158, "Quote"},
    {0x00000159, "RSA_Decrypt"},
    {0x0000015b, "HMAC_Start"},
    {0x0000015c, "SequenceUpdate"},
    {0x0000015d, "Sign"},
    {0x0000015e, "Unseal"},
    {0x00000160, "PolicySigned"},
    {0x00000161, "ContextLoad"},
    {0x00000162, "ContextSave"},
    {0x00000163, "ECDH_KeyGen"},
    {0x00000164, "EncryptDecrypt"},
    {0x00000165, "FlushContext"},
    {0x00000167, "LoadExternal"},
    {0x00000168, "MakeCredential"},
    {0x00000169, "NV_ReadPublic"},
    {0x0000016a, "PolicyAuthorize"},
    {0x0000016b, "PolicyAuthValue"},
    {0x0000016c, "PolicyCommandCode"},
    {0x0000016d, "PolicyCounterTimer"},
    {0x0000016e, "PolicyCpHash"},
    {0x0000016f, "PolicyLocality"},
    {0x00000170, "PolicyNameHash"},
    {0x00000171, "PolicyOR"},
    {0x00000172, "PolicyTicket"},
    {0x00000173, "ReadPublic"},
    {0x00000174, "RSA_Encrypt"},
    {0x00000176, "StartAuthSession"},
    {0x00000177, "VerifySignature"},
    {0x00000178, "ECC_Parameters"},
    {0x00000179, "FirmwareRead"},
    {0x0000017a, "GetCapability"},
    {0x0000017b, "GetRandom"},
    {0x0000017c, "GetTestResult"},
    {0x0000017d, "Hash"},
    {0x0000017e, "PCR_Read"},
    {0x0000017f, "PolicyPCR"},
    {0x00000180, "PolicyRestart"},
    {0x00000181, "ReadClock"},
    {0x00000182, "PCR_Extend"},
    {0x00000183, "PCR_SetAuthValue"},
    {0x00000184, "NV_Certify"},
    {0x00000185, "EventSequenceComplete"},
    {0x00000186, "HashSequenceStart"},
    {0x00000187, "PolicyPhysicalPresence"},
    {0x00000188, "PolicyDuplicationSelect"},
    {0x00000189, "PolicyGetDigest"},
    {0x0000018a, "TestParms"},
    {0x0000018b, "Commit"},
    {0x0000018c, "PolicyPassword"},
    {0x0000018d, "ZGen_2Phase"},
    {0x0000018e, "EC_Ephemeral"},
    {0x0000018f, "PolicyNvWritten"},
    {0x00000190, "PolicyTemplate"},
    {0x00000191, "CreateLoaded"},
    {0x00000192, "PolicyAuthorizeNV"},
    {0x00000193, "EncryptDecrypt2"},
    {0x00000194, "AC_GetCapability"},
    {0x00000195, "AC_Send"},
    {0x00000196, "Policy_AC_SendSelect"},
    {0x00000197, "CertifyX509"},
    {0x00000198, "ACT_SetTimeout"},
    {0x20000000, "Vendor_TCG_Test"},
};
#define TPM_COMMAND_NAMES_SIZE (sizeof(tpm_command_names) / sizeof(tpm_command_names[0]))

typedef struct {
    const uint32_t code;
    const char *name;
} tpm_rsp_entry;

static const tpm_rsp_entry tpm_response_names[] = {
    {0x0000, "Success"},
    {0x001E, "Error: Bad Tag"},
    {0x0100, "Error: Initialize"},
    {0x0101, "Error: Failure"},
    {0x0103, "Error: Sequence"},
    {0x010B, "Error: Private"},
    {0x0119, "Error: Hmac"},
    {0x0120, "Error: Disabled"},
    {0x0121, "Error: Exclusive"},
    {0x0124, "Error: Auth Type"},
    {0x0125, "Error: Auth Missing"},
    {0x0126, "Error: Policy"},
    {0x0127, "Error: PCR"},
    {0x0128, "Error: Pcr Changed"},
    {0x012D, "Error: Upgrade"},
    {0x012E, "Error: Too Many Contexts"},
    {0x012F, "Error: Auth Unavailable"},
    {0x0130, "Error: Reboot"},
    {0x0131, "Error: Unbalanced"},
    {0x0142, "Error: Command Size"},
    {0x0143, "Error: Command Code"},
    {0x0144, "Error: Authsize"},
    {0x0145, "Error: Auth Context"},
    {0x0146, "Error: NV Range"},
    {0x0147, "Error: NV Size"},
    {0x0148, "Error: NV Locked"},
    {0x0149, "Error: NV Authorization"},
    {0x014A, "Error: NV Uninitialized"},
    {0x014B, "Error: NV Space"},
    {0x014C, "Error: NV Defined"},
    {0x0150, "Error: Bad Context"},
    {0x0151, "Error: Cphash"},
    {0x0152, "Error: Parent"},
    {0x0153, "Error: Needs Test"},
    {0x0154, "Error: No Result"},
    {0x0155, "Error: Sensitive"},
    {0x017F, "Error: Max Fm0"},
    {0x0081, "Error: Asymmetric"},
    {0x0082, "Error: Attributes"},
    {0x0083, "Error: Hash"},
    {0x0084, "Error: Value"},
    {0x0085, "Error: Hierarchy"},
    {0x0087, "Error: Key Size"},
    {0x0088, "Error: Mgf"},
    {0x0089, "Error: Mode"},
    {0x008A, "Error: Type"},
    {0x008B, "Error: Handle"},
    {0x008C, "Error: Kdf"},
    {0x008D, "Error: Range"},
    {0x008E, "Error: Auth Fail"},
    {0x008F, "Error: Nonce"},
    {0x0090, "Error: Pp"},
    {0x0092, "Error: Scheme"},
    {0x0095, "Error: Size"},
    {0x0096, "Error: Symmetric"},
    {0x0097, "Error: Tag"},
    {0x0098, "Error: Selector"},
    {0x009A, "Error: Insufficient"},
    {0x009B, "Error: Signature"},
    {0x009C, "Error: Key"},
    {0x009D, "Error: Policy Fail"},
    {0x009F, "Error: Integrity"},
    {0x00A0, "Error: Ticket"},
    {0x00A1, "Error: Reserved Bits"},
    {0x00A2, "Error: Bad Auth"},
    {0x00A3, "Error: Expired"},
    {0x00A4, "Error: Policy Cc"},
    {0x00A5, "Error: Binding"},
    {0x00A6, "Error: Curve"},
    {0x00A7, "Error: Ecc Point"},
    {0x0901, "Error: Context Gap"},
    {0x0902, "Error: Object Memory"},
    {0x0903, "Error: Session Memory"},
    {0x0904, "Error: Memory"},
    {0x0905, "Error: Session Handles"},
    {0x0906, "Error: Object Handles"},
    {0x0907, "Error: Locality"},
    {0x0908, "Error: Yielded"},
    {0x0909, "Error: Canceled"},
    {0x090A, "Error: Testing"},
    {0x0910, "Error: Reference H0"},
    {0x0911, "Error: Reference H1"},
    {0x0912, "Error: Reference H2"},
    {0x0913, "Error: Reference H3"},
    {0x0914, "Error: Reference H4"},
    {0x0915, "Error: Reference H5"},
    {0x0916, "Error: Reference H6"},
    {0x0918, "Error: Reference S0"},
    {0x0919, "Error: Reference S1"},
    {0x091A, "Error: Reference S2"},
    {0x091B, "Error: Reference S3"},
    {0x091C, "Error: Reference S4"},
    {0x091D, "Error: Reference S5"},
    {0x091E, "Error: Reference S6"},
    {0x0920, "Error: NV Rate"},
    {0x0921, "Error: Lockout"},
    {0x0922, "Error: Retry"},
    {0x0923, "Error: NV Unavailable"},
    {0x097F, "Error: Not Used"},
};
#define TPM_RESPONSE_NAMES_SIZE (sizeof(tpm_response_names) / sizeof(tpm_response_names[0]))

/* SPI register lookup table */
typedef struct {
    uint32_t addr;
    const char *name;
    int size;
} tpm_reg_entry;

static const tpm_reg_entry spi_tpm_regs[] = {
    {0xD40000, "TPM_ACCESS_{locality}", 1},
    {0xD40008, "TPM_INT_ENABLE_{locality}", 4},
    {0xD4000c, "TPM_INT_VECTOR_{locality}", 1},
    {0xD40010, "TPM_INT_STATUS_{locality}", 4},
    {0xD40014, "TPM_INTF_CAPABILITY_{locality}", 5},
    {0xD40018, "TPM_STS_{locality}", 6},
    {0xD40024, "TPM_DATA_FIFO_{locality}", 4},
    {0xD40030, "TPM_INTERFACE_ID_{locality}", 4},
    {0xD40080, "TPM_XDATA_FIFO_{locality}", 4},
    {0xD40F00, "TPM_DID_VID_{locality}", 4},
    {0xD40F04, "TPM_RID_{locality}", 1},
    {0xD40F90, "vendor-defined", 0x70},
};
#define SPI_TPM_REGS_SIZE (sizeof(spi_tpm_regs) / sizeof(spi_tpm_regs[0]))

/* I2C register lookup table */
static const tpm_reg_entry i2c_tpm_regs[] = {
    {0x00, "TPM_LOC_SEL", 1},
    {0x04, "TPM_ACCESS", 1},
    {0x08, "TPM_INT_ENABLE", 4},
    {0x10, "TPM_INT_STATUS", 4},
    {0x14, "TPM_INT_CAPABILITY", 4},
    {0x18, "TPM_STS", 4},
    {0x20, "TPM_HASH_END", 1},
    {0x24, "TPM_DATA_FIFO", 4},
    {0x28, "TPM_HASH_START", 1},
    {0x30, "TPM_I2C_INTERFACE_CAPABILITY", 4},
    {0x38, "TPM_I2C_DEVICE_ADDRESS", 2},
    {0x40, "TPM_DATA_CSUM_ENABLE", 1},
    {0x44, "TPM_DATA_CSUM", 2},
    {0x48, "TPM_DID_VID", 4},
    {0x4C, "TPM_DID_RID", 1},
};
#define I2C_TPM_REGS_SIZE (sizeof(i2c_tpm_regs) / sizeof(i2c_tpm_regs[0]))

typedef struct {
    int out_ann;
    int out_py;
    /* TPM State */
    int state;
    int state_finished;
    uint64_t state_start;
    /* Command buffer */
    uint8_t command_buffer[TPM_MAX_BUFFER];
    int command_len;
    uint64_t command_start;
    /* Response buffer */
    uint8_t response_buffer[TPM_MAX_BUFFER];
    int response_len;
    uint64_t response_start;
} tpm_state;

static const char *tpm_inputs[] = {"tpm-tis", NULL};
static const char *tpm_outputs[] = {"tpm", NULL};
static const char *tpm_tags[] = {"TPM", NULL};

static const char *tpm_ann_labels[][3] = {
    {"", "register-read", "Register Read"},
    {"", "register-write", "Register Write"},
    {"", "tpm-command", "TPM Command"},
    {"", "tpm-response", "TPM Response"},
    {"", "warning", "Warning"},
    {"", "state", "State"},
};

static const int tpm_row_register_classes[] = {ANN_REG_READ, ANN_REG_WRITE, -1};
static const int tpm_row_tpm_classes[] = {ANN_TPM_CMD, ANN_TPM_RSP, -1};
static const int tpm_row_warnings_classes[] = {ANN_WARN, -1};
static const int tpm_row_states_classes[] = {ANN_STATE, -1};

static const struct srd_c_ann_row tpm_ann_rows[] = {
    {"register", "Register Transaction", tpm_row_register_classes, 2},
    {"tpm", "TPM Command/Response", tpm_row_tpm_classes, 2},
    {"warnings", "Warnings", tpm_row_warnings_classes, 1},
    {"states", "TPM States", tpm_row_states_classes, 1},
};

static const char *tpm_lookup_command_name(uint32_t code)
{
    for (int i = 0; i < (int)TPM_COMMAND_NAMES_SIZE; i++) {
        if (tpm_command_names[i].code == code)
            return tpm_command_names[i].name;
    }
    return "Unknown";
}

static const char *tpm_lookup_response_name(uint32_t code)
{
    for (int i = 0; i < (int)TPM_RESPONSE_NAMES_SIZE; i++) {
        if (tpm_response_names[i].code == code)
            return tpm_response_names[i].name;
    }
    return "Error: Unknown";
}

static void tpm_annotate_command(struct srd_decoder_inst *di, tpm_state *s,
    const uint8_t *data, int len)
{
    if (len < 10) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[%d bytes]", len);
        c_put(di, s->command_start, s->state_start, s->out_ann, ANN_TPM_CMD, buf);
        return;
    }
    uint32_t cmd_code = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                        ((uint32_t)data[8] << 8) | data[9];
    const char *name = tpm_lookup_command_name(cmd_code);

    char buf[512];
    snprintf(buf, sizeof(buf), "%s (%04X)", name, cmd_code);
    c_put(di, s->command_start, s->state_start, s->out_ann, ANN_TPM_CMD, buf);
}

static void tpm_annotate_response(struct srd_decoder_inst *di, tpm_state *s,
    const uint8_t *data, int len, uint64_t es)
{
    if (len < 10) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[%d bytes]", len);
        c_put(di, s->response_start, es, s->out_ann, ANN_TPM_RSP, buf);
        return;
    }
    uint32_t rsp_code = ((uint32_t)data[6] << 24) | ((uint32_t)data[7] << 16) |
                        ((uint32_t)data[8] << 8) | data[9];
    const char *name = tpm_lookup_response_name(rsp_code);

    char buf[512];
    snprintf(buf, sizeof(buf), "%s (%04X)", name, rsp_code);
    c_put(di, s->response_start, es, s->out_ann, ANN_TPM_RSP, buf);
}

static const char *tpm_state_name(int state)
{
    switch (state) {
    case TPM_STATE_UNKNOWN: return "Unknown";
    case TPM_STATE_IDLE: return "Idle";
    case TPM_STATE_READY: return "Ready";
    case TPM_STATE_RECEPTION: return "Reception";
    case TPM_STATE_EXECUTION: return "Execution";
    case TPM_STATE_COMPLETION: return "Completion";
    default: return "Unknown";
    }
}

static void tpm_set_state(struct srd_decoder_inst *di, tpm_state *s,
    int new_state, uint64_t ss)
{
    if (new_state == s->state)
        return;
    if (s->state_start != 0) {
        c_put(di, s->state_start, ss, s->out_ann, ANN_STATE,
                  tpm_state_name(s->state));
    }
    s->state = new_state;
    s->state_start = ss;
    if (new_state == TPM_STATE_RECEPTION || new_state == TPM_STATE_COMPLETION)
        s->state_finished = 0;
}

static void tpm_reset_command(tpm_state *s, uint64_t ss, const uint8_t *data, int len)
{
    s->command_len = 0;
    s->command_start = ss;
    if (data && len > 0) {
        int copy = len < TPM_MAX_BUFFER ? len : TPM_MAX_BUFFER;
        for (int j = 0; j < copy; j++) s->command_buffer[j] = data[j];
        s->command_len = copy;
    }
}

static void tpm_reset_response(tpm_state *s, uint64_t ss, const uint8_t *data, int len)
{
    s->response_len = 0;
    s->response_start = ss;
    if (data && len > 0) {
        int copy = len < TPM_MAX_BUFFER ? len : TPM_MAX_BUFFER;
        for (int j = 0; j < copy; j++) s->response_buffer[j] = data[j];
        s->response_len = copy;
    }
}

static int is_power_of_two(uint8_t value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

static void tpm_on_read(struct srd_decoder_inst *di, tpm_state *s,
    uint32_t addr, const uint8_t *xfer_data, int xfer_len,
    uint64_t ss, uint64_t es)
{
    uint32_t reg = addr & 0xfff;

    if (reg == TPM_STS_X) {
        uint8_t status = xfer_data[0];

        if (s->state == TPM_STATE_UNKNOWN) {
            if ((status & (TPM_STS_commandReady | TPM_STS_dataAvail)) == TPM_STS_commandReady)
                tpm_set_state(di, s, TPM_STATE_READY, ss);
            else if (status & TPM_STS_Expect)
                tpm_set_state(di, s, TPM_STATE_RECEPTION, ss);
            else if (status & TPM_STS_dataAvail)
                tpm_set_state(di, s, TPM_STATE_COMPLETION, ss);
        }
        if (s->state == TPM_STATE_IDLE) {
            if ((status & (TPM_STS_commandReady | TPM_STS_dataAvail)) == TPM_STS_commandReady)
                tpm_set_state(di, s, TPM_STATE_READY, ss);
            else if (status & TPM_STS_read_mask)
                tpm_set_state(di, s, TPM_STATE_UNKNOWN, ss);
        } else if (s->state == TPM_STATE_READY) {
            if ((status & TPM_STS_read_mask) == TPM_STS_Expect)
                tpm_set_state(di, s, TPM_STATE_RECEPTION, ss);
            else if ((status & (TPM_STS_commandReady | TPM_STS_dataAvail)) != TPM_STS_commandReady)
                tpm_set_state(di, s, TPM_STATE_UNKNOWN, ss);
        } else if (s->state == TPM_STATE_RECEPTION) {
            if ((status & TPM_STS_stsValid) && !(status & TPM_STS_Expect))
                s->state_finished = 1;
        } else if (s->state == TPM_STATE_EXECUTION) {
            if ((status & TPM_STS_read_mask) == TPM_STS_dataAvail) {
                tpm_reset_response(s, ss, NULL, 0);
                tpm_set_state(di, s, TPM_STATE_COMPLETION, ss);
            } else if (status & TPM_STS_read_mask) {
                tpm_set_state(di, s, TPM_STATE_UNKNOWN, ss);
            }
        } else if (s->state == TPM_STATE_COMPLETION) {
            if ((status & TPM_STS_read_mask) == 0) {
                tpm_annotate_response(di, s, s->response_buffer, s->response_len, es);
                s->state_finished = 1;
                tpm_reset_response(s, 0, NULL, 0);
                tpm_set_state(di, s, TPM_STATE_IDLE, ss);
            } else if ((status & TPM_STS_stsValid) && (status & TPM_STS_read_mask) != TPM_STS_dataAvail) {
                tpm_set_state(di, s, TPM_STATE_UNKNOWN, ss);
            }
        }
    } else if (reg == TPM_DATA_FIFO_X) {
        if (s->state == TPM_STATE_COMPLETION) {
            if (!s->state_finished) {
                int copy = xfer_len;
                if (s->response_len + copy > TPM_MAX_BUFFER)
                    copy = TPM_MAX_BUFFER - s->response_len;
                if (copy > 0) {
                    memcpy(s->response_buffer + s->response_len, xfer_data, copy);
                    s->response_len += copy;
                }
            }
        }
    }
}

static void tpm_on_write(struct srd_decoder_inst *di, tpm_state *s,
    uint32_t addr, const uint8_t *xfer_data, int xfer_len,
    uint64_t ss, uint64_t es)
{
    uint32_t reg = addr & 0xfff;

    if (reg == TPM_STS_X) {
        uint8_t status = xfer_data[0];
        if (!is_power_of_two(status)) {
            c_put(di, ss, es, s->out_ann, ANN_WARN,
                      "Only one field may be set at a time when writing to TPM_STS_X");
            tpm_set_state(di, s, TPM_STATE_UNKNOWN, ss);
            return;
        }

        if (s->state == TPM_STATE_IDLE) {
            if (status & TPM_STS_commandReady)
                tpm_set_state(di, s, TPM_STATE_READY, ss);
        } else if (s->state == TPM_STATE_READY) {
            /* Already ready */
        } else if (s->state == TPM_STATE_RECEPTION) {
            if (status & TPM_STS_commandReady) {
                c_put(di, ss, es, s->out_ann, ANN_WARN,
                          "Command aborted (while sending command)");
                tpm_reset_command(s, 0, NULL, 0);
                tpm_set_state(di, s, TPM_STATE_IDLE, es);
            } else if (status & TPM_STS_tpmGo) {
                if (!s->state_finished) {
                    c_put(di, ss, es, s->out_ann, ANN_WARN,
                              "TPM is still expecting data, so this tpmGo signal is ignored");
                } else {
                    tpm_annotate_command(di, s, s->command_buffer, s->command_len);
                    tpm_reset_command(s, 0, NULL, 0);
                    tpm_set_state(di, s, TPM_STATE_EXECUTION, ss);
                }
            }
        } else if (s->state == TPM_STATE_EXECUTION) {
            if (status & TPM_STS_commandReady) {
                c_put(di, ss, es, s->out_ann, ANN_WARN,
                          "Command aborted (while executing command)");
                tpm_set_state(di, s, TPM_STATE_IDLE, es);
            }
        } else if (s->state == TPM_STATE_COMPLETION) {
            if (status & TPM_STS_responseRetry) {
                tpm_reset_response(s, ss, NULL, 0);
            } else if (status & TPM_STS_commandReady) {
                tpm_reset_response(s, 0, NULL, 0);
                tpm_set_state(di, s, TPM_STATE_IDLE, es);
            }
        }
    }

    if (reg == TPM_DATA_FIFO_X) {
        if (s->state == TPM_STATE_READY || s->state == TPM_STATE_IDLE) {
            tpm_reset_command(s, ss, xfer_data, xfer_len);
            tpm_set_state(di, s, TPM_STATE_RECEPTION, ss);
        } else if (s->state == TPM_STATE_RECEPTION) {
            if (!s->state_finished) {
                int copy = xfer_len;
                if (s->command_len + copy > TPM_MAX_BUFFER)
                    copy = TPM_MAX_BUFFER - s->command_len;
                if (copy > 0) {
                    memcpy(s->command_buffer + s->command_len, xfer_data, copy);
                    s->command_len += copy;
                }
            }
        }
    }
}

static void tpm_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    tpm_state *s = (tpm_state *)c_decoder_get_private(di);
    if (!s)
        return;

    if (strcmp(cmd, "TRANSACTION") != 0)
        return;

    /* Data format from tpm_tis_spi_c:
       fields[0].u8 = reading (0=write, 1=read)
       fields[1].u8 = addr >> 16
       fields[2].u8 = addr >> 8
       fields[3].u8 = addr & 0xFF
       fields[4].u8 = data_count
       fields[5..] = data bytes (C_BYTES) */
    if (n_fields < 5)
        return;

    int reading = fields[0].u8 ? 1 : 0;
    uint32_t addr = ((uint32_t)fields[1].u8 << 16) | ((uint32_t)fields[2].u8 << 8) | fields[3].u8;
    uint16_t xfer_len = fields[4].u8;
    const c_field *xfer_data = fields + 5;
    if (xfer_len > n_fields - 5)
        xfer_len = (uint16_t)(n_fields - 5);

    /* Emit register read/write annotation */
    int ann_cls = reading ? ANN_REG_READ : ANN_REG_WRITE;
    char reg_buf[128];
    /* Simple register name lookup */
    const char *reg_name = NULL;
    uint32_t reg_offset = addr & 0xfff;
    (void)reg_offset;
    if (addr > 0xff && (addr & 0xffff0000) == 0x00d40000) {
        int locality = (addr & 0xf000) >> 12;
        (void)locality;
        for (int i = 0; i < (int)SPI_TPM_REGS_SIZE; i++) {
            if (spi_tpm_regs[i].addr == (addr & 0xffff0fff)) {
                reg_name = spi_tpm_regs[i].name;
                break;
            }
        }
        if (!reg_name) {
            snprintf(reg_buf, sizeof(reg_buf), "%08X (resvd)", addr);
            reg_name = reg_buf;
        }
    } else {
        for (int i = 0; i < (int)I2C_TPM_REGS_SIZE; i++) {
            if (i2c_tpm_regs[i].addr == addr) {
                reg_name = i2c_tpm_regs[i].name;
                break;
            }
        }
        if (!reg_name) {
            snprintf(reg_buf, sizeof(reg_buf), "%02X (resvd)", addr);
            reg_name = reg_buf;
        }
    }

    char ann_buf[256];
    /* Format: regname=hexdata */
    int pos = 0;
    pos += snprintf(ann_buf + pos, sizeof(ann_buf) - pos, "%s=", reg_name);
    for (int i = 0; i < xfer_len && pos < (int)sizeof(ann_buf) - 3; i++)
        pos += snprintf(ann_buf + pos, sizeof(ann_buf) - pos, "%02X", xfer_data[i].u8);

    c_put(di, start_sample, end_sample, s->out_ann, ann_cls, ann_buf);

    /* Process state machine */
    uint8_t *xfer_bytes = (uint8_t *)g_malloc(xfer_len);
    for (int i = 0; i < xfer_len; i++)
        xfer_bytes[i] = xfer_data[i].u8;
    if (reading)
        tpm_on_read(di, s, addr, xfer_bytes, xfer_len, start_sample, end_sample);
    else
        tpm_on_write(di, s, addr, xfer_bytes, xfer_len, start_sample, end_sample);
    g_free(xfer_bytes);
}

static void tpm_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(tpm_state)));
    }
    tpm_state *s = (tpm_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(tpm_state));
    s->state = TPM_STATE_UNKNOWN;
}

static void tpm_start(struct srd_decoder_inst *di)
{
    tpm_state *s = (tpm_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "tpm_fifo_tis");
    s->out_py = c_reg_out(di, SRD_OUTPUT_PYTHON, "tpm");
}

static void tpm_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void tpm_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder tpm_fifo_tis_c_decoder = {
    .id = "tpm_fifo_tis_c",
    .name = "TPM FIFO(C)",
    .longname = "Trusted Platform Module Commands over TIS 2.0 interface (C)",
    .desc = "Trusted Platform Module Commands over TIS 2.0 interface. (C implementation)",
    .license = "gplv3+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = tpm_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = tpm_ann_rows,
    .inputs = tpm_inputs,
    .num_inputs = 1,
    .outputs = tpm_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = tpm_tags,
    .num_tags = 1,
    .reset = tpm_reset,
    .start = tpm_start,
    .decode = tpm_decode,
    .destroy = tpm_destroy,
    .decode_upper = tpm_recv_proto,
    .state_size = 0,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &tpm_fifo_tis_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}