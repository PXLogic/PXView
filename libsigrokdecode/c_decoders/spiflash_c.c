/*
 * Copyright (C) 2024 DreamSourceLab <support@dreamsourcelab.com>
 * License: gplv2+
 *
 * xx25 series SPI NOR Flash chip protocol decoder (C implementation).
 * Ported from Python decoder spiflash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

/* ===== Annotation enumeration ===== */
enum {
    ANN_WRSR = 0,
    ANN_PP,
    ANN_READ,
    ANN_WRDI,
    ANN_RDSR,
    ANN_WREN,
    ANN_FAST_READ,
    ANN_SE,
    ANN_RDSCUR,
    ANN_WRSCUR,
    ANN_RDSR2,
    ANN_CE,
    ANN_ESRY,
    ANN_DSRY,
    ANN_WRITE1,
    ANN_WRITE2,
    ANN_REMS,
    ANN_RDID,
    ANN_RDP_RES,
    ANN_CP,
    ANN_ENSO,
    ANN_DP,
    ANN_READ2X,
    ANN_EXSO,
    ANN_CE2,
    ANN_STATUS,
    ANN_BE,
    ANN_REMS2,
    ANN_BIT,
    ANN_FIELD,
    ANN_WARN,
    NUM_ANN = 31,
};

/* ===== Chip info structure ===== */
typedef struct {
    const char *key;
    const char *vendor;
    const char *model;
    uint64_t rdid_id;
    uint16_t rems_id;
    uint16_t rems2_id;
    int page_size;
    int sector_size;
    int block_size;
} spiflash_chip_info;

static const spiflash_chip_info spiflash_chips[] = {
    {"adesto_at45db161e", "Adesto", "AT45DB161E",
     0x1f26000100, 0xffff, 0xffff, 528, 128*1024, 4*1024},
    {"fidelix_fm25q32", "FIDELIX", "FM25Q32",
     0xa14016, 0xa115, 0xa115, 256, 4*1024, 64*1024},
    {"macronix_mx25l1605d", "Macronix", "MX25L1605D",
     0xc22015, 0xc214, 0xc214, 256, 4*1024, 64*1024},
    {"macronix_mx25l3205d", "Macronix", "MX25L3205D",
     0xc22016, 0xc215, 0xc215, 256, 4*1024, 64*1024},
    {"macronix_mx25l6405d", "Macronix", "MX25L6405D",
     0xc22017, 0xc216, 0xc216, 256, 4*1024, 64*1024},
    {"winbond_w25q80dv", "Winbond", "W25Q80DV",
     0xef4014, 0xef13, 0xffff, 256, 4*1024, 64*1024},
};

/* Device name lookup */
typedef struct { uint8_t id; const char *name; } spiflash_device_name_entry;
static const spiflash_device_name_entry adesto_devices[] = {{0x00, "AT45Dxxx family"}, {0xFF, NULL}};
static const spiflash_device_name_entry fidelix_devices[] = {{0x15, "FM25Q32"}, {0xFF, NULL}};
static const spiflash_device_name_entry macronix_devices[] = {
    {0x14, "MX25L1605D"}, {0x15, "MX25L3205D"}, {0x16, "MX25L6405D"}, {0xFF, NULL}};
static const spiflash_device_name_entry winbond_devices[] = {{0x13, "W25Q80DV"}, {0xFF, NULL}};

typedef struct { const char *vendor; const spiflash_device_name_entry *devices; } spiflash_vendor_devices;
static const spiflash_vendor_devices spiflash_vendor_table[] = {
    {"adesto", adesto_devices},
    {"fidelix", fidelix_devices},
    {"macronix", macronix_devices},
    {"winbond", winbond_devices},
};

/* Command definitions */
typedef struct { uint8_t cmd; const char *shortname; const char *longname; } spiflash_cmd_def;
static const spiflash_cmd_def spiflash_cmds[] = {
    {0x01, "WRSR", "Write status register"},
    {0x02, "PP", "Page program"},
    {0x03, "READ", "Read data"},
    {0x04, "WRDI", "Write disable"},
    {0x05, "RDSR", "Read status register"},
    {0x06, "WREN", "Write enable"},
    {0x0b, "FAST/READ", "Fast read data"},
    {0x20, "SE", "Sector erase"},
    {0x2b, "RDSCUR", "Read security register"},
    {0x2f, "WRSCUR", "Write security register"},
    {0x35, "RDSR2", "Read status register 2"},
    {0x60, "CE", "Chip erase"},
    {0x70, "ESRY", "Enable SO to output RY/BY#"},
    {0x80, "DSRY", "Disable SO to output RY/BY#"},
    {0x82, "WRITE1", "Main memory page program through buffer 1 with built-in erase"},
    {0x85, "WRITE2", "Main memory page program through buffer 2 with built-in erase"},
    {0x90, "REMS", "Read electronic manufacturer & device ID"},
    {0x9f, "RDID", "Read identification"},
    {0xab, "RDP/RES", "Release from deep powerdown / Read electronic ID"},
    {0xad, "CP", "Continuously program mode"},
    {0xb1, "ENSO", "Enter secured OTP"},
    {0xb9, "DP", "Deep power down"},
    {0xbb, "2READ", "2x I/O read"},
    {0xc1, "EXSO", "Exit secured OTP"},
    {0xc7, "CE2", "Chip erase"},
    {0xd7, "STATUS", "Status register read"},
    {0xd8, "BE", "Block erase"},
    {0xef, "REMS2", "Read ID for 2x I/O mode"},
};

/* ===== State structure ===== */
typedef struct {
    uint64_t ss;
    uint64_t es;
    int state;          /* Current command byte, 0 = NULL */
    int cmdstate;       /* Byte counter within command */
    uint32_t addr;
    uint8_t data_buf[4096];
    int data_count;
    int writestate;
    int device_id;
    uint64_t ss_cmd, es_cmd;
    uint64_t ss_field, es_field;
    
    int chip_index;
    int format;         /* 0=hex, 1=ascii */
    int manufacturer_id_first;
    uint8_t ids[2];
    int out_ann;
    /* Delayed output callback flag */
    int have_delayed_output;
    int delayed_ann;
} spiflash_state;

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

static inline int spi_proto_cs_change_get_values(const c_field *fields, int n_fields,
                                                  uint8_t *prev, uint8_t *cur)
{
    if (n_fields < 2) { *prev = 0xFF; *cur = 0xFF; return -1; }
    *prev = fields[0].u8; *cur = fields[1].u8; return 0;
}

/* ===== Static data ===== */
static const char *spiflash_inputs[] = {"spi", NULL};
static const char *spiflash_tags[] = {"IC", "Memory", NULL};

static struct srd_decoder_option spiflash_options[] = {
    {"chip", "dec_spiflash_opt_chip", "Chip", NULL, NULL},
    {"format", "dec_spiflash_opt_format", "Data format", NULL, NULL},
};

static const char *spiflash_ann_labels[][3] = {
    {"", "wrsr", "Write status register"},
    {"", "pp", "Page program"},
    {"", "read", "Read data"},
    {"", "wrdi", "Write disable"},
    {"", "rdsr", "Read status register"},
    {"", "wren", "Write enable"},
    {"", "fast_read", "Fast read data"},
    {"", "se", "Sector erase"},
    {"", "rdscur", "Read security register"},
    {"", "wrscur", "Write security register"},
    {"", "rdsr2", "Read status register 2"},
    {"", "ce", "Chip erase"},
    {"", "esry", "Enable SO to output RY/BY#"},
    {"", "dsry", "Disable SO to output RY/BY#"},
    {"", "write1", "Main memory page program through buffer 1"},
    {"", "write2", "Main memory page program through buffer 2"},
    {"", "rems", "Read electronic manufacturer & device ID"},
    {"", "rdid", "Read identification"},
    {"", "rdp_res", "Release from deep powerdown / Read electronic ID"},
    {"", "cp", "Continuously program mode"},
    {"", "enso", "Enter secured OTP"},
    {"", "dp", "Deep power down"},
    {"", "2read", "2x I/O read"},
    {"", "exso", "Exit secured OTP"},
    {"", "ce2", "Chip erase"},
    {"", "status", "Status register read"},
    {"", "be", "Block erase"},
    {"", "rems2", "Read ID for 2x I/O mode"},
    {"", "bit", "Bit"},
    {"", "field", "Field"},
    {"", "warning", "Warning"},
};

static const int spiflash_row_bits_classes[] = {ANN_BIT, -1};
static const int spiflash_row_fields_classes[] = {ANN_FIELD, -1};
static const int spiflash_row_commands_classes[] = {
    ANN_WRSR, ANN_PP, ANN_READ, ANN_WRDI, ANN_RDSR, ANN_WREN, ANN_FAST_READ,
    ANN_SE, ANN_RDSCUR, ANN_WRSCUR, ANN_RDSR2, ANN_CE, ANN_ESRY, ANN_DSRY,
    ANN_WRITE1, ANN_WRITE2, ANN_REMS, ANN_RDID, ANN_RDP_RES, ANN_CP,
    ANN_ENSO, ANN_DP, ANN_READ2X, ANN_EXSO, ANN_CE2, ANN_STATUS, ANN_BE, ANN_REMS2, -1
};
static const int spiflash_row_warnings_classes[] = {ANN_WARN, -1};

static const struct srd_c_ann_row spiflash_ann_rows[] = {
    {"bits", "Bits", spiflash_row_bits_classes, 1},
    {"fields", "Fields", spiflash_row_fields_classes, 1},
    {"commands", "Commands", spiflash_row_commands_classes, 28},
    {"warnings", "Warnings", spiflash_row_warnings_classes, 1},
};

/* ===== Helper: find command definition ===== */
static const spiflash_cmd_def *spiflash_find_cmd(uint8_t cmd_byte)
{
    for (int i = 0; i < (int)(sizeof(spiflash_cmds)/sizeof(spiflash_cmds[0])); i++) {
        if (spiflash_cmds[i].cmd == cmd_byte)
            return &spiflash_cmds[i];
    }
    return NULL;
}

/* ===== Helper: find chip info ===== */
static int spiflash_find_chip_index(const char *key)
{
    for (int i = 0; i < (int)(sizeof(spiflash_chips)/sizeof(spiflash_chips[0])); i++) {
        if (strcmp(spiflash_chips[i].key, key) == 0)
            return i;
    }
    return 0; /* default to first chip */
}

/* ===== Helper: find device name ===== */
static const char *spiflash_find_device_name(const char *vendor, uint8_t device_id)
{
    for (int i = 0; i < (int)(sizeof(spiflash_vendor_table)/sizeof(spiflash_vendor_table[0])); i++) {
        if (strcmp(spiflash_vendor_table[i].vendor, vendor) == 0) {
            const spiflash_device_name_entry *e = spiflash_vendor_table[i].devices;
            for (int j = 0; e[j].name != NULL; j++) {
                if (e[j].id == device_id)
                    return e[j].name;
            }
        }
    }
    return "Unknown";
}

/* ===== Helper: decode status register ===== */
static void spiflash_decode_status_reg(uint8_t data, char *buf, int bufsize)
{
    int pos = 0;
    pos += snprintf(buf + pos, bufsize - pos, "%srite operation in progress. ", (data & 1) ? "W" : "No w");
    pos += snprintf(buf + pos, bufsize - pos, "WEL %sset. ", (data & 2) ? "" : "not ");
    pos += snprintf(buf + pos, bufsize - pos, "BP3-BP0: 0x%x. ", (data >> 2) & 0xf);
    pos += snprintf(buf + pos, bufsize - pos, "%sCP mode. ", (data & 0x40) ? "" : "not ");
    pos += snprintf(buf + pos, bufsize - pos, "SRWD %sallowed.", (data & 0x80) ? "not " : "");
}

/* ===== Helper: emit command byte ===== */
static void spiflash_emit_cmd_byte(struct srd_decoder_inst *di, spiflash_state *s)
{
    s->ss_cmd = s->ss;
    const spiflash_cmd_def *c = spiflash_find_cmd((uint8_t)s->state);
    if (c) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Command: %s (%s)", c->longname, c->shortname);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    }
    s->addr = 0;
}

/* ===== Helper: emit address bytes ===== */
static void spiflash_emit_addr_bytes(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi)
{
    s->addr |= (mosi << ((4 - s->cmdstate) * 8));
    int b = ((3 - (s->cmdstate - 2)) * 8) - 1;
    char buf[64];
    snprintf(buf, sizeof(buf), "Addr bits %d..%d", b, b - 7);
    c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    if (s->cmdstate == 2) s->ss_field = s->ss;
    if (s->cmdstate == 4) {
        s->es_field = s->es;
        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "@%06x", s->addr);
        c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, addr_str);
    }
}

/* ===== Helper: output data block (delayed) ===== */
static void spiflash_output_data_block(struct srd_decoder_inst *di, spiflash_state *s)
{
    s->es_cmd = s->es;
    const spiflash_cmd_def *c = spiflash_find_cmd((uint8_t)s->state);
    const char *cmd_name = c ? c->longname : "Unknown";

    /* Format data */
    char data_str[2048];
    int pos = 0;
    if (s->format == 0) { /* hex */
        for (int i = 0; i < s->data_count && pos < (int)sizeof(data_str) - 4; i++)
            pos += snprintf(data_str + pos, sizeof(data_str) - pos, "%02x ", s->data_buf[i]);
    } else { /* ascii */
        for (int i = 0; i < s->data_count && pos < (int)sizeof(data_str) - 2; i++) {
            char ch = (s->data_buf[i] >= 32 && s->data_buf[i] < 127) ? s->data_buf[i] : '.';
            data_str[pos++] = ch;
        }
    }
    data_str[pos] = '\0';

    char field_buf[128];
    snprintf(field_buf, sizeof(field_buf), "Data (%d bytes)", s->data_count);
    c_put(di, s->ss_field, s->es_field, s->out_ann, ANN_FIELD, field_buf);

    char cmd_buf[4096];
    snprintf(cmd_buf, sizeof(cmd_buf), "%s (addr @%06x, %d bytes): %s",
             cmd_name, s->addr, s->data_count, data_str);
    c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, s->delayed_ann, cmd_buf);
}

/* ===== Helper: end current transaction ===== */
static void spiflash_end_current_transaction(struct srd_decoder_inst *di, spiflash_state *s)
{
    if (s->have_delayed_output && s->data_count > 0) {
        spiflash_output_data_block(di, s);
    }
    s->state = 0;
    s->cmdstate = 1;
    s->addr = 0;
    s->data_count = 0;
    s->have_delayed_output = 0;
}

/* ===== Command handlers ===== */
static void spiflash_handle_wren(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi; (void)miso;
    c_put(di, s->ss, s->es, s->out_ann, ANN_WREN, "Write enable");
    s->writestate = 1;
    s->state = 0;
}

static void spiflash_handle_wrdi(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi; (void)miso;
    c_put(di, s->ss, s->es, s->out_ann, ANN_WRDI, "Write disable");
    s->writestate = 0;
    s->state = 0;
}

static void spiflash_handle_rdid(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else if (s->cmdstate == 2) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Manufacturer ID: 0x%02x", miso);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    } else if (s->cmdstate == 3) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Memory type: 0x%02x", miso);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    } else if (s->cmdstate == 4) {
        s->device_id = miso;
        char buf[64];
        snprintf(buf, sizeof(buf), "Device ID: 0x%02x", miso);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
        const char *vendor = spiflash_chips[s->chip_index].vendor;
        const char *dev_name = spiflash_find_device_name(vendor, miso);
        char vbuf[128];
        snprintf(vbuf, sizeof(vbuf), "Device = %s %s", vendor, dev_name);
        c_put(di, s->ss_cmd, s->es, s->out_ann, ANN_RDID, vbuf);
        s->state = 0;
        return;
    }
    s->cmdstate++;
}

static void spiflash_handle_rdsr(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else {
        s->es_cmd = s->es;
        char buf[512];
        spiflash_decode_status_reg(miso, buf, sizeof(buf));
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "Status register");
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RDSR, "Read status register");
        s->writestate = (miso & 2) ? 1 : 0;
    }
    s->cmdstate++;
}

static void spiflash_handle_rdsr2(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else {
        s->es_cmd = s->es;
        char buf[512];
        spiflash_decode_status_reg(miso, buf, sizeof(buf));
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "Status register 2");
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RDSR2, "Read status register 2");
    }
    s->cmdstate++;
}

static void spiflash_handle_wrsr(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)miso;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else if (s->cmdstate == 2) {
        char buf[512];
        spiflash_decode_status_reg(mosi, buf, sizeof(buf));
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "Status register 1");
        s->writestate = (mosi & 2) ? 1 : 0;
    } else if (s->cmdstate == 3) {
        char buf[512];
        spiflash_decode_status_reg(mosi, buf, sizeof(buf));
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, "Status register 2");
        s->es_cmd = s->es;
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WRSR, "Write status register");
    }
    s->cmdstate++;
}

static void spiflash_handle_read(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else if (s->cmdstate >= 2 && s->cmdstate <= 4) {
        spiflash_emit_addr_bytes(di, s, mosi);
    } else if (s->cmdstate >= 5) {
        s->es_field = s->es;
        if (s->cmdstate == 5) {
            s->ss_field = s->ss;
            s->have_delayed_output = 1;
            s->delayed_ann = ANN_READ;
        }
        if (s->data_count < (int)sizeof(s->data_buf))
            s->data_buf[s->data_count++] = miso;
    }
    s->cmdstate++;
}

static void spiflash_handle_fast_read(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else if (s->cmdstate >= 2 && s->cmdstate <= 4) {
        spiflash_emit_addr_bytes(di, s, mosi);
    } else if (s->cmdstate == 5) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Dummy byte: 0x%02x", mosi);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    } else if (s->cmdstate >= 6) {
        s->es_field = s->es;
        if (s->cmdstate == 6) {
            s->ss_field = s->ss;
            s->have_delayed_output = 1;
            s->delayed_ann = ANN_FAST_READ;
        }
        if (s->data_count < (int)sizeof(s->data_buf))
            s->data_buf[s->data_count++] = miso;
    }
    s->cmdstate++;
}

static void spiflash_handle_write_common(struct srd_decoder_inst *di, spiflash_state *s,
                                          uint8_t mosi, uint8_t miso, int ann)
{
    (void)miso;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
        if (s->writestate == 0) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Warning: WREN might be missing");
        }
    } else if (s->cmdstate >= 2 && s->cmdstate <= 4) {
        spiflash_emit_addr_bytes(di, s, mosi);
    } else if (s->cmdstate >= 5) {
        s->es_field = s->es;
        if (s->cmdstate == 5) {
            s->ss_field = s->ss;
            s->have_delayed_output = 1;
            s->delayed_ann = ann;
        }
        if (s->data_count < (int)sizeof(s->data_buf))
            s->data_buf[s->data_count++] = mosi;
    }
    s->cmdstate++;
}

static void spiflash_handle_pp(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    spiflash_handle_write_common(di, s, mosi, miso, ANN_PP);
}

static void spiflash_handle_write1(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    spiflash_handle_write_common(di, s, mosi, miso, ANN_WRITE1);
}

static void spiflash_handle_write2(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    spiflash_handle_write_common(di, s, mosi, miso, ANN_WRITE2);
}

static void spiflash_handle_se(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)miso;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
        if (s->writestate == 0) {
            c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Warning: WREN might be missing");
        }
    } else if (s->cmdstate >= 2 && s->cmdstate <= 4) {
        spiflash_emit_addr_bytes(di, s, mosi);
    }
    if (s->cmdstate == 4) {
        s->es_cmd = s->es;
        char buf[256];
        snprintf(buf, sizeof(buf), "Erase sector @%06x", s->addr);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_SE, buf);
        if (s->addr % 4096 != 0) {
            c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_WARN, "Warning: Invalid sector address!");
        }
        s->state = 0;
    } else {
        s->cmdstate++;
    }
}

static void spiflash_handle_be(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)di; (void)s; (void)mosi; (void)miso;
    /* TODO */
}

static void spiflash_handle_ce(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi; (void)miso;
    c_put(di, s->ss, s->es, s->out_ann, ANN_CE, "Chip erase");
    if (s->writestate == 0) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Warning: WREN might be missing");
    }
    s->state = 0;
}

static void spiflash_handle_ce2(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi; (void)miso;
    c_put(di, s->ss, s->es, s->out_ann, ANN_CE2, "Chip erase");
    if (s->writestate == 0) {
        c_put(di, s->ss, s->es, s->out_ann, ANN_WARN, "Warning: WREN might be missing");
    }
    s->state = 0;
}

static void spiflash_handle_rdp_res(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else if (s->cmdstate >= 2 && s->cmdstate <= 4) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Dummy byte: 0x%02x", mosi);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    } else if (s->cmdstate == 5) {
        s->device_id = miso;
        const char *vendor = spiflash_chips[s->chip_index].vendor;
        const char *dev_name = spiflash_find_device_name(vendor, miso);
        char buf[256];
        snprintf(buf, sizeof(buf), "Device ID: %s %s", vendor, dev_name);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
        s->es_cmd = s->es;
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_RDP_RES, "Release from deep powerdown / Read electronic ID");
        s->state = 0;
        return;
    }
    s->cmdstate++;
}

static void spiflash_handle_rems(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
    } else if (s->cmdstate == 2 || s->cmdstate == 3) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Dummy byte: 0x%02X", mosi);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    } else if (s->cmdstate == 4) {
        s->manufacturer_id_first = (mosi == 0x00) ? 1 : 0;
        const char *d = (mosi == 0x00) ? "manufacturer" : "device";
        char buf[64];
        snprintf(buf, sizeof(buf), "Master wants %s ID first", d);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    } else if (s->cmdstate == 5) {
        s->ids[0] = miso;
        const char *d = s->manufacturer_id_first ? "Manufacturer" : "Device";
        char buf[64];
        snprintf(buf, sizeof(buf), "%s ID: 0x%02X", d, miso);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);
    } else if (s->cmdstate == 6) {
        s->ids[1] = miso;
        const char *d = s->manufacturer_id_first ? "Device" : "Manufacturer";
        char buf[64];
        snprintf(buf, sizeof(buf), "%s ID: 0x%02X", d, miso);
        c_put(di, s->ss, s->es, s->out_ann, ANN_FIELD, buf);

        int id_ = s->manufacturer_id_first ? s->ids[1] : s->ids[0];
        s->device_id = id_;
        s->es_cmd = s->es;
        const char *vendor = spiflash_chips[s->chip_index].vendor;
        const char *dev_name = spiflash_find_device_name(vendor, (uint8_t)id_);
        char vbuf[128];
        snprintf(vbuf, sizeof(vbuf), "Device = %s %s", vendor, dev_name);
        c_put(di, s->ss_cmd, s->es_cmd, s->out_ann, ANN_REMS, vbuf);
        s->state = 0;
        return;
    }
    s->cmdstate++;
}

/* Stub handlers for TODO commands */
static void spiflash_handle_stub(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)di; (void)s; (void)mosi; (void)miso;
    /* TODO */
}

static void spiflash_handle_status(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
{
    (void)mosi;
    if (s->cmdstate == 1) {
        spiflash_emit_cmd_byte(di, s);
        s->have_delayed_output = 1;
        s->delayed_ann = ANN_STATUS;
    } else {
        s->es_cmd = s->es;
        s->es_field = s->es;
        if (s->cmdstate == 2) s->ss_field = s->ss;
        char buf[64];
        snprintf(buf, sizeof(buf), "Status register byte %d: 0x%02x", ((s->cmdstate % 2) + 1), miso);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
    }
    s->cmdstate++;
}

/* ===== Command handler dispatch ===== */
typedef void (*spiflash_cmd_handler)(struct srd_decoder_inst *di, spiflash_state *s, uint8_t mosi, uint8_t miso)
;

typedef struct { uint8_t cmd; spiflash_cmd_handler handler; } spiflash_cmd_entry;
static const spiflash_cmd_entry spiflash_cmd_table[] = {
    {0x01, spiflash_handle_wrsr},
    {0x02, spiflash_handle_pp},
    {0x03, spiflash_handle_read},
    {0x04, spiflash_handle_wrdi},
    {0x05, spiflash_handle_rdsr},
    {0x06, spiflash_handle_wren},
    {0x0b, spiflash_handle_fast_read},
    {0x20, spiflash_handle_se},
    {0x2b, spiflash_handle_stub}, /* RDSCUR */
    {0x2f, spiflash_handle_stub}, /* WRSCUR */
    {0x35, spiflash_handle_rdsr2},
    {0x60, spiflash_handle_ce},
    {0x70, spiflash_handle_stub}, /* ESRY */
    {0x80, spiflash_handle_stub}, /* DSRY */
    {0x82, spiflash_handle_write1},
    {0x85, spiflash_handle_write2},
    {0x90, spiflash_handle_rems},
    {0x9f, spiflash_handle_rdid},
    {0xab, spiflash_handle_rdp_res},
    {0xad, spiflash_handle_stub}, /* CP */
    {0xb1, spiflash_handle_stub}, /* ENSO */
    {0xb9, spiflash_handle_stub}, /* DP */
    {0xbb, spiflash_handle_stub}, /* 2READ */
    {0xc1, spiflash_handle_stub}, /* EXSO */
    {0xc7, spiflash_handle_ce2},
    {0xd7, spiflash_handle_status},
    {0xd8, spiflash_handle_be},
    {0xef, spiflash_handle_stub}, /* REMS2 */
};

static spiflash_cmd_handler spiflash_find_handler(uint8_t cmd_byte)
{
    for (int i = 0; i < (int)(sizeof(spiflash_cmd_table)/sizeof(spiflash_cmd_table[0])); i++) {
        if (spiflash_cmd_table[i].cmd == cmd_byte)
            return spiflash_cmd_table[i].handler;
    }
    return NULL;
}

/* ===== recv_proto ===== */
static void spiflash_recv_proto(struct srd_decoder_inst *di, uint64_t start_sample, uint64_t end_sample, const char *cmd, const c_field *fields, int n_fields)
{
    (void)start_sample; (void)end_sample;
    spiflash_state *s = (spiflash_state *)c_decoder_get_private(di);
    if (!s) return;

    s->ss = start_sample;
    s->es = end_sample;

    if (strcmp(cmd, "CS-CHANGE") == 0) {
        spiflash_end_current_transaction(di, s);
        return;
    }

    if (strcmp(cmd, "DATA") != 0)
        return;

    uint8_t mosi = 0, miso = 0;
    spi_proto_get_mosi(fields, n_fields, &mosi);
    spi_proto_get_miso(fields, n_fields, &miso);

    /* If no current command, first MOSI byte is command */
    if (s->state == 0) {
        s->state = mosi;
        s->cmdstate = 1;
    }

    /* Dispatch to handler */
    spiflash_cmd_handler handler = spiflash_find_handler((uint8_t)s->state);
    if (handler) {
        handler(di, s, mosi, miso);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unknown command: 0x%02x", mosi);
        c_put(di, s->ss, s->es, s->out_ann, ANN_BIT, buf);
        s->state = 0;
    }
}

/* ===== Lifecycle callbacks ===== */
static void spiflash_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(spiflash_state)));
    }
    spiflash_state *s = (spiflash_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(spiflash_state));
    s->device_id = -1;
    s->chip_index = 0;
    s->format = 0;
}

static void spiflash_start(struct srd_decoder_inst *di)
{
    spiflash_state *s = (spiflash_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "spiflash");

    const char *chip = c_opt_str(di, "chip", "macronix_mx25l1605d");
    s->chip_index = spiflash_find_chip_index(chip);

    const char *fmt = c_opt_str(di, "format", "hex");
    s->format = (strcmp(fmt, "ascii") == 0) ? 1 : 0;
}

static void spiflash_decode(struct srd_decoder_inst *di)
{
    (void)di;
}

static void spiflash_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

/* ===== Decoder struct ===== */
struct srd_c_decoder spiflash_c_decoder = {
    .id = "spiflash_c",
    .name = "SPI Flash(C)",
    .longname = "xx25 series SPI NOR flash chips (C)",
    .desc = "xx25 series SPI NOR flash chip protocol decoder (C implementation)",
    .license = "gplv2+",
    .channels = NULL,
    .num_channels = 0,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = spiflash_options,
    .num_options = 2,
    .num_annotations = NUM_ANN,
    .ann_labels = spiflash_ann_labels,
    .num_annotation_rows = 4,
    .annotation_rows = spiflash_ann_rows,
    .inputs = spiflash_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = spiflash_tags,
    .num_tags = 2,
    .reset = spiflash_reset,
    .start = spiflash_start,
    .decode = spiflash_decode,
    .destroy = spiflash_destroy,
    .decode_upper = spiflash_recv_proto,
    .state_size = 0,
};

/* ===== Export functions ===== */
SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    spiflash_options[0].def = g_variant_new_string("macronix_mx25l1605d");
    GSList *chip_vals = NULL;
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("adesto_at45db161e"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("fidelix_fm25q32"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("macronix_mx25l1605d"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("macronix_mx25l3205d"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("macronix_mx25l6405d"));
    chip_vals = g_slist_append(chip_vals, g_variant_new_string("winbond_w25q80dv"));
    spiflash_options[0].values = chip_vals;

    spiflash_options[1].def = g_variant_new_string("hex");
    GSList *fmt_vals = NULL;
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("hex"));
    fmt_vals = g_slist_append(fmt_vals, g_variant_new_string("ascii"));
    spiflash_options[1].values = fmt_vals;

    return &spiflash_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}