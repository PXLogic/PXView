/*
 * QSPI (Quad SPI) C decoder
 * Ported from Python smart_qspi decoder
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRANSFER_BYTES 300

enum qspi_ann {
    ANN_DATA_TRANSFER = 0,
    ANN_QUAD_DATA,
    ANN_DUAL_DATA,
    ANN_D0,
    ANN_D1,
    ANN_D2,
    ANN_D3,
    ANN_OTHER,
    NUM_ANN,
};

enum process_enum {
    PROCESS_COMMAND = 0,
    PROCESS_WRITE_BYTE,
    PROCESS_READ_BYTE,
    PROCESS_READ_BYTE_CONTINUOUS,
    PROCESS_CONTINUOUS_READ_MODE_BITS,
    PROCESS_ADDRESS_BY_MODE,
    PROCESS_ADDRESS_24BIT,
    PROCESS_ADDRESS_32BIT,
    PROCESS_DUMMY_BY_MODE,
    PROCESS_DUMMY_8BIT,
    PROCESS_DUMMY_32BIT,
    PROCESS_DUMMY_40BIT,
};

enum process_mode {
    MODE_SINGLE = 0,
    MODE_DUAL,
    MODE_QUAD,
};

typedef struct {
    int proc_enum;
    int mode;
} process_info;

typedef struct {
    uint8_t cmd_byte;
    const char *name;
    const char *abbrev;
    const process_info *data_after;
    int data_after_count;
} qspi_cmd_entry;

/* Command data sequences */
static const process_info cmd_06_data[] = {}; /* WREN */
static const process_info cmd_04_data[] = {}; /* WRDI */
static const process_info cmd_05_data[] = {{PROCESS_READ_BYTE, MODE_SINGLE}};
static const process_info cmd_01_data[] = {{PROCESS_WRITE_BYTE, MODE_SINGLE}};
static const process_info cmd_02_data[] = {{PROCESS_ADDRESS_BY_MODE, MODE_SINGLE}, {PROCESS_DUMMY_BY_MODE, MODE_SINGLE}, {PROCESS_WRITE_BYTE, MODE_SINGLE}};
static const process_info cmd_03_data[] = {{PROCESS_ADDRESS_BY_MODE, MODE_SINGLE}, {PROCESS_DUMMY_BY_MODE, MODE_SINGLE}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_SINGLE}};
static const process_info cmd_0B_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_8BIT, MODE_SINGLE}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_SINGLE}};
static const process_info cmd_3B_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_8BIT, MODE_DUAL}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_DUAL}};
static const process_info cmd_6B_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_8BIT, MODE_QUAD}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_QUAD}};
static const process_info cmd_BB_data[] = {{PROCESS_ADDRESS_24BIT, MODE_DUAL}, {PROCESS_DUMMY_BY_MODE, MODE_DUAL}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_DUAL}};
static const process_info cmd_EB_data[] = {{PROCESS_ADDRESS_24BIT, MODE_QUAD}, {PROCESS_DUMMY_BY_MODE, MODE_QUAD}, {PROCESS_CONTINUOUS_READ_MODE_BITS, MODE_QUAD}, {PROCESS_DUMMY_BY_MODE, MODE_QUAD}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_QUAD}};
static const process_info cmd_0C_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_32BIT, MODE_SINGLE}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_SINGLE}};
static const process_info cmd_3C_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_32BIT, MODE_DUAL}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_DUAL}};
static const process_info cmd_6C_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_32BIT, MODE_QUAD}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_QUAD}};
static const process_info cmd_BC_data[] = {{PROCESS_ADDRESS_24BIT, MODE_DUAL}, {PROCESS_DUMMY_BY_MODE, MODE_DUAL}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_DUAL}};
static const process_info cmd_EC_data[] = {{PROCESS_ADDRESS_24BIT, MODE_QUAD}, {PROCESS_DUMMY_BY_MODE, MODE_QUAD}, {PROCESS_CONTINUOUS_READ_MODE_BITS, MODE_QUAD}, {PROCESS_DUMMY_BY_MODE, MODE_QUAD}, {PROCESS_READ_BYTE_CONTINUOUS, MODE_QUAD}};
static const process_info cmd_20_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}};
static const process_info cmd_21_data[] = {{PROCESS_ADDRESS_32BIT, MODE_SINGLE}};
static const process_info cmd_D8_data[] = {{PROCESS_ADDRESS_BY_MODE, MODE_SINGLE}};
static const process_info cmd_5A_data[] = {{PROCESS_READ_BYTE, MODE_SINGLE}};
static const process_info cmd_35_data[] = {{PROCESS_READ_BYTE, MODE_SINGLE}};
static const process_info cmd_15_data[] = {{PROCESS_READ_BYTE, MODE_SINGLE}};
static const process_info cmd_85_data[] = {{PROCESS_WRITE_BYTE, MODE_SINGLE}};
static const process_info cmd_65_data[] = {{PROCESS_WRITE_BYTE, MODE_SINGLE}};
static const process_info cmd_71_data[] = {}; /* QSPI no data */
static const process_info cmd_B7_data[] = {}; /* Enter 4-byte addr */
static const process_info cmd_E9_data[] = {}; /* Exit 4-byte addr */
static const process_info cmd_38_data[] = {{PROCESS_READ_BYTE_CONTINUOUS, MODE_DUAL}};
static const process_info cmd_32_cmd_02_data[] = {{PROCESS_ADDRESS_24BIT, MODE_SINGLE}, {PROCESS_DUMMY_8BIT, MODE_QUAD}, {PROCESS_WRITE_BYTE, MODE_QUAD}};
static const process_info cmd_32_cmd_2C_data[] = {{PROCESS_ADDRESS_24BIT, MODE_QUAD}, {PROCESS_DUMMY_BY_MODE, MODE_QUAD}, {PROCESS_WRITE_BYTE, MODE_QUAD}};

/* Command table */
static const qspi_cmd_entry qspi_cmd_table[] = {
    {0x06, "Write Enable", "WREN", cmd_06_data, 0},
    {0x04, "Write Disable", "WRDI", cmd_04_data, 0},
    {0x05, "Read Status Register", "RDSR", cmd_05_data, 1},
    {0x01, "Write Status Register", "WRSR", cmd_01_data, 1},
    {0x02, "Page Program", "PP", cmd_02_data, 3},
    {0x03, "Read Data", "READ", cmd_03_data, 3},
    {0x0B, "Fast Read", "FAST_READ", cmd_0B_data, 3},
    {0x3B, "Dual Output Fast Read", "DOR", cmd_3B_data, 3},
    {0x6B, "Quad Output Fast Read", "QOR", cmd_6B_data, 3},
    {0xBB, "Dual I/O Fast Read", "DIOR", cmd_BB_data, 3},
    {0xEB, "Quad I/O Fast Read", "QIOR", cmd_EB_data, 5},
    {0x0C, "Fast Read 4Byte", "FAST_READ4B", cmd_0C_data, 3},
    {0x3C, "Dual Output Fast Read 4Byte", "DOR4B", cmd_3C_data, 3},
    {0x6C, "Quad Output Fast Read 4Byte", "QOR4B", cmd_6C_data, 3},
    {0xBC, "Dual I/O Fast Read 4Byte", "DIOR4B", cmd_BC_data, 3},
    {0xEC, "Quad I/O Fast Read 4Byte", "QIOR4B", cmd_EC_data, 5},
    {0x20, "Sector Erase", "SE", cmd_20_data, 1},
    {0x21, "Sector Erase 4Byte", "SE4B", cmd_21_data, 1},
    {0xD8, "Block Erase", "BE", cmd_D8_data, 1},
    {0x5A, "Read SFDP", "RDSFDP", cmd_5A_data, 1},
    {0x35, "Read QE Register", "RDCR", cmd_35_data, 1},
    {0x15, "Read Status Register-2", "RDSR2", cmd_15_data, 1},
    {0x85, "Write Status Register-2", "WRSR2", cmd_85_data, 1},
    {0x65, "Write QE Register", "WRCR", cmd_65_data, 1},
    {0x71, "Quad Page Program ECC", "QPP_ECC", cmd_71_data, 0},
    {0xB7, "Enter 4-Byte Address Mode", "EN4B", cmd_B7_data, 0},
    {0xE9, "Exit 4-Byte Address Mode", "EX4B", cmd_E9_data, 0},
    {0x38, "Quad Page Program", "QPP", cmd_38_data, 1},
    {0x32, "Quad Page Program Security", "QPPS", cmd_32_cmd_02_data, 3},
    {0x2C, "Quad I/O Page Program", "QIOPP", cmd_32_cmd_2C_data, 3},
};

#define CMD_TABLE_SIZE (sizeof(qspi_cmd_table) / sizeof(qspi_cmd_table[0]))

static const qspi_cmd_entry *find_command(uint8_t cmd_byte)
{
    for (int i = 0; i < (int)CMD_TABLE_SIZE; i++) {
        if (qspi_cmd_table[i].cmd_byte == cmd_byte)
            return &qspi_cmd_table[i];
    }
    return NULL;
}

typedef struct {
    int have_io1;
    int have_io3;
    int have_cs;
    int cs_cond_idx;

    int cs_polarity;
    int cpol;
    int cpha;
    int bit_order;
    int ads;
    int frame;
    int spi_mode_set;
    int invalid_level;

    uint64_t samplerate;
    double bit_width;

    int bitcount;
    uint64_t io0data, io1data, io2data, io3data;

    int io0bits_val[8]; uint64_t io0bits_ss[8]; uint64_t io0bits_es[8];
    int io1bits_val[8]; uint64_t io1bits_ss[8]; uint64_t io1bits_es[8];
    int io2bits_val[8]; uint64_t io2bits_ss[8]; uint64_t io2bits_es[8];
    int io3bits_val[8]; uint64_t io3bits_ss[8]; uint64_t io3bits_es[8];

    uint64_t io0bytes_ss[MAX_TRANSFER_BYTES]; uint64_t io0bytes_es[MAX_TRANSFER_BYTES]; uint64_t io0bytes_val[MAX_TRANSFER_BYTES];
    int io0bytes_cnt;
    uint64_t io1bytes_ss[MAX_TRANSFER_BYTES]; uint64_t io1bytes_es[MAX_TRANSFER_BYTES]; uint64_t io1bytes_val[MAX_TRANSFER_BYTES];
    int io1bytes_cnt;
    uint64_t io2bytes_ss[MAX_TRANSFER_BYTES]; uint64_t io2bytes_es[MAX_TRANSFER_BYTES]; uint64_t io2bytes_val[MAX_TRANSFER_BYTES];
    int io2bytes_cnt;
    uint64_t io3bytes_ss[MAX_TRANSFER_BYTES]; uint64_t io3bytes_es[MAX_TRANSFER_BYTES]; uint64_t io3bytes_val[MAX_TRANSFER_BYTES];
    int io3bytes_cnt;

    int command;
    int state_count;
    int count;
    uint64_t bits_data;
    uint64_t ss;

    uint64_t ss_block;
    uint64_t ss_transfer;
    int cs_was_deasserted;

    int sample_edge_rise;

    int out_ann;
    int out_python;
    int out_binary;
    int out_bitrate;
    int bw;
} qspi_state;

static struct srd_channel qspi_channels[] = {
    { "clk", "CLK", "Clock", 0, SRD_CHANNEL_SCLK, "dec_qspi_chan_clk" },
    { "io0", "IO0", "Data i/o 0", 1, SRD_CHANNEL_SDATA, "dec_qspi_chan_io0" },
};

static struct srd_channel qspi_optional_channels[] = {
    { "io1", "IO1", "Data i/o 1", 2, SRD_CHANNEL_SDATA, "dec_qspi_opt_chan_io1" },
    { "io2", "IO2", "Data i/o 2", 3, SRD_CHANNEL_SDATA, "dec_qspi_opt_chan_io2" },
    { "io3", "IO3", "Data i/o 3", 4, SRD_CHANNEL_SDATA, "dec_qspi_opt_chan_io3" },
    { "cs", "CS#", "Chip-select", 5, SRD_CHANNEL_COMMON, "dec_qspi_opt_chan_cs" },
};

static struct srd_decoder_option qspi_options[] = {
    { "cs_polarity", NULL, "CS# polarity", NULL, NULL },
    { "cpol", NULL, "Clock polarity (CPOL)", NULL, NULL },
    { "cpha", NULL, "Clock phase (CPHA)", NULL, NULL },
    { "bitorder", NULL, "Bit order", NULL, NULL },
    { "ads", NULL, "Address Mode", NULL, NULL },
    { "frame", NULL, "Frame Decoder", NULL, NULL },
    { "twolinesmode", NULL, "TwoLinesMode", NULL, NULL },
    { "invalidlevel", NULL, "Keep high or low as invalid", NULL, NULL },
};

static const char *qspi_ann_labels[][3] = {
    { "", "data-transfer", "data transfer" },
    { "", "Quad data", "Q-Data" },
    { "", "Dual data", "D-Data" },
    { "", "d0", "IO0 data" },
    { "", "d1", "IO1 data" },
    { "", "d2", "IO2 data" },
    { "", "d3", "IO3 data" },
    { "", "other", "Human-readable warnings" },
};

static const int qspi_row_data_transfer_classes[] = { ANN_DATA_TRANSFER, -1 };
static const int qspi_row_quad_data_classes[] = { ANN_QUAD_DATA, -1 };
static const int qspi_row_dual_data_classes[] = { ANN_DUAL_DATA, -1 };
static const int qspi_row_d0_classes[] = { ANN_D0, -1 };
static const int qspi_row_d1_classes[] = { ANN_D1, -1 };
static const int qspi_row_d2_classes[] = { ANN_D2, -1 };
static const int qspi_row_d3_classes[] = { ANN_D3, -1 };
static const int qspi_row_other_classes[] = { ANN_OTHER, -1 };

static const struct srd_c_ann_row qspi_ann_rows[] = {
    { "data-transfer", "data transfer", qspi_row_data_transfer_classes, 1 },
    { "Quad data", "Q-Data", qspi_row_quad_data_classes, 1 },
    { "Dual data", "D-Data", qspi_row_dual_data_classes, 1 },
    { "d0", "D0", qspi_row_d0_classes, 1 },
    { "d1", "D1", qspi_row_d1_classes, 1 },
    { "d2", "D2", qspi_row_d2_classes, 1 },
    { "d3", "D3", qspi_row_d3_classes, 1 },
    { "Other", "Other", qspi_row_other_classes, 1 },
};

static const char *qspi_inputs[] = { "logic" };
static const char *qspi_outputs[] = { "spi" };
static const char *qspi_tags[] = { "Embedded/industrial" };

static int qspi_cs_asserted(qspi_state *s, int cs_val)
{
    return (s->cs_polarity == 0) ? (cs_val == 0) : (cs_val == 1);
}

static void qspi_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(qspi_state)));
    }
    qspi_state *s = (qspi_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(qspi_state));
    s->cs_polarity = 0;
    s->cpol = 0;
    s->cpha = 0;
    s->bit_order = 0;
    s->ads = 0;
    s->frame = 0;
    s->spi_mode_set = 0;
    s->invalid_level = 0;
    s->ss_transfer = (uint64_t)-1;
    s->out_ann = -1;
    s->out_python = -1;
    s->out_binary = -1;
    s->out_bitrate = -1;
    s->bw = 1;
}

static void qspi_start(struct srd_decoder_inst *di)
{
    qspi_state *s = (qspi_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "spi");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "spi");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "spi");
    s->out_bitrate = c_reg_out(di, SRD_OUTPUT_META, "spi");

    const char *cs_pol_str = c_opt_str(di, "cs_polarity", "active-low");
    s->cs_polarity = (strcmp(cs_pol_str, "active-low") == 0) ? 0 : 1;

    s->cpol = (int)c_opt_int(di, "cpol", 0);
    s->cpha = (int)c_opt_int(di, "cpha", 0);

    const char *bitorder_str = c_opt_str(di, "bitorder", "msb-first");
    s->bit_order = (strcmp(bitorder_str, "msb-first") == 0) ? 0 : 1;

    const char *ads_str = c_opt_str(di, "ads", "24-Bit Address");
    s->ads = (strcmp(ads_str, "32-Bit Address") == 0) ? 1 : 0;

    const char *frame_str = c_opt_str(di, "frame", "no");
    s->frame = (strcmp(frame_str, "yes") == 0) ? 1 : 0;

    const char *twoln_str = c_opt_str(di, "twolinesmode", "spi");
    if (strcmp(twoln_str, "dspi") == 0)
        s->spi_mode_set = 1;
    else if (strcmp(twoln_str, "qspi") == 0)
        s->spi_mode_set = 2;
    else
        s->spi_mode_set = 0;

    const char *inv_str = c_opt_str(di, "invalidlevel", "both");
    if (strcmp(inv_str, "low") == 0)
        s->invalid_level = 1;
    else if (strcmp(inv_str, "high") == 0)
        s->invalid_level = 2;
    else
        s->invalid_level = 0;

    s->have_io1 = c_has_ch(di, 2);
    s->have_io3 = c_has_ch(di, 3) && c_has_ch(di, 4);
    s->have_cs = c_has_ch(di, 5);

    int mode;
    if (s->cpol == 0 && s->cpha == 0) mode = 0;
    else if (s->cpol == 0 && s->cpha == 1) mode = 1;
    else if (s->cpol == 1 && s->cpha == 0) mode = 2;
    else mode = 3;
    s->sample_edge_rise = (mode == 0 || mode == 3) ? 1 : 0;
}

static void qspi_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    qspi_state *s = (qspi_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        if (value > 0)
            s->bit_width = 1.0 / (double)value;
    }
}

static void qspi_reset_word(qspi_state *s)
{
    s->bitcount = 0;
    s->io0data = 0;
    s->io1data = 0;
    s->io2data = 0;
    s->io3data = 0;
}

static void qspi_putdata(struct srd_decoder_inst *di, qspi_state *s)
{
    int ws = 8;
    int current_mode = MODE_SINGLE;

    /* Determine mode based on data validity */
    int io2_valid = 0, io3_valid = 0;
    if (s->have_io3) {
        /* Check if IO2/IO3 data is not all same (invalid) */
        int io2_all_zero = (s->io2data == 0);
        int io2_all_ff = (s->io2data == 0xFF);
        int io3_all_zero = (s->io3data == 0);
        int io3_all_ff = (s->io3data == 0xFF);
        int io2_invalid = 0, io3_invalid = 0;

        if (s->invalid_level == 0) { /* both */
            io2_invalid = io2_all_zero || io2_all_ff;
            io3_invalid = io3_all_zero || io3_all_ff;
        } else if (s->invalid_level == 1) { /* low */
            io2_invalid = io2_all_zero;
            io3_invalid = io3_all_zero;
        } else { /* high */
            io2_invalid = io2_all_ff;
            io3_invalid = io3_all_ff;
        }

        if (!io2_invalid && !io3_invalid) {
            current_mode = MODE_QUAD;
            io2_valid = 1;
            io3_valid = 1;
        }
    }

    if (s->have_io1 && current_mode != MODE_QUAD) {
        current_mode = MODE_DUAL;
    }

    /* If user set mode explicitly and IO lines are invalid, use user setting */
    if (current_mode == MODE_SINGLE && s->spi_mode_set == 2 && s->have_io3)
        current_mode = MODE_QUAD;
    else if (current_mode == MODE_SINGLE && s->spi_mode_set == 1 && s->have_io1)
        current_mode = MODE_DUAL;

    uint64_t ss = s->io0bits_ss[0];
    uint64_t es = s->io0bits_es[ws - 1];

    /* Output IO line annotations */
    {
        char str[16];
        snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io0data);
        c_put(di, ss, es, s->out_ann, ANN_D0, str);
    }
    if (s->have_io1) {
        char str[16];
        snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io1data);
        c_put(di, ss, es, s->out_ann, ANN_D1, str);
    }
    if (io2_valid) {
        char str[16];
        snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io2data);
        c_put(di, ss, es, s->out_ann, ANN_D2, str);
    }
    if (io3_valid) {
        char str[16];
        snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io3data);
        c_put(di, ss, es, s->out_ann, ANN_D3, str);
    }

    /* Output combined data */
    if (current_mode == MODE_QUAD) {
        /* Quad: 2 clock cycles -> 4 bytes */
        /* For simplicity, output IO0 data as the main byte */
        char str[16];
        snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io0data);
        c_put(di, ss, es, s->out_ann, ANN_QUAD_DATA, str);
    } else if (current_mode == MODE_DUAL) {
        char str[16];
        snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io0data);
        c_put(di, ss, es, s->out_ann, ANN_DUAL_DATA, str);
    }

    /* Command parsing state machine */
    if (s->command == 0) {
        /* Idle: check for command byte */
        const qspi_cmd_entry *entry = find_command((uint8_t)s->io0data);
        if (entry) {
            char cmd_str[128], cmd_short[64], cmd_abbr[32];
            snprintf(cmd_str, sizeof(cmd_str), "Command : %s", entry->name);
            snprintf(cmd_short, sizeof(cmd_short), "CMD : %s", entry->abbrev);
            snprintf(cmd_abbr, sizeof(cmd_abbr), "%s", entry->abbrev);
            c_put(di, ss, es, s->out_ann, ANN_DATA_TRANSFER, cmd_str, cmd_short, cmd_abbr);

            /* Update address mode */
            if (entry->cmd_byte == 0xB7) s->ads = 1;
            if (entry->cmd_byte == 0xE9) s->ads = 0;

            if (entry->data_after_count > 0) {
                s->command = entry->cmd_byte;
                s->state_count = 0;
                s->count = 0;
                s->bits_data = 0;
                s->ss = es + 1;
            }
        } else {
            char str[16];
            snprintf(str, sizeof(str), "@%02llX", (unsigned long long)s->io0data);
            c_put(di, ss, es, s->out_ann, ANN_DATA_TRANSFER, str);
        }
    } else {
        /* Processing command data */
        const qspi_cmd_entry *entry = find_command((uint8_t)s->command);
        if (entry && s->state_count < entry->data_after_count) {
            const process_info *pi = &entry->data_after[s->state_count];
            switch (pi->proc_enum) {
            case PROCESS_ADDRESS_BY_MODE:
            case PROCESS_ADDRESS_24BIT:
            case PROCESS_ADDRESS_32BIT: {
                int addr_bytes = (pi->proc_enum == PROCESS_ADDRESS_32BIT || s->ads == 1) ? 4 : 3;
                if (s->count == 0) s->ss = ss;
                s->bits_data = (s->bits_data << 8) | s->io0data;
                s->count++;
                if (s->count >= addr_bytes) {
                    char addr_str[64], addr_short[32], addr_abbr[16];
                    snprintf(addr_str, sizeof(addr_str), "%d-Bit Address : 0x%llX", addr_bytes * 8, (unsigned long long)s->bits_data);
                    snprintf(addr_short, sizeof(addr_short), "AD : 0x%llX", (unsigned long long)s->bits_data);
                    snprintf(addr_abbr, sizeof(addr_abbr), "0x%llX", (unsigned long long)s->bits_data);
                    c_put(di, s->ss, es, s->out_ann, ANN_DATA_TRANSFER, addr_str, addr_short, addr_abbr);
                    s->state_count++;
                    s->count = 0;
                    s->bits_data = 0;
                }
                break;
            }
            case PROCESS_DUMMY_BY_MODE:
            case PROCESS_DUMMY_8BIT:
            case PROCESS_DUMMY_32BIT:
            case PROCESS_DUMMY_40BIT: {
                s->count++;
                int dummy_total;
                if (pi->proc_enum == PROCESS_DUMMY_8BIT) dummy_total = 1;
                else if (pi->proc_enum == PROCESS_DUMMY_32BIT) dummy_total = 4;
                else if (pi->proc_enum == PROCESS_DUMMY_40BIT) dummy_total = 5;
                else dummy_total = 1; /* PROCESS_DUMMY_BY_MODE */
                if (s->count >= dummy_total) {
                    c_put(di, ss, es, s->out_ann, ANN_DATA_TRANSFER, "Dummy Cycles", "Dummy", "D");
                    s->state_count++;
                    s->count = 0;
                }
                break;
            }
            case PROCESS_WRITE_BYTE:
            case PROCESS_READ_BYTE: {
                char data_str[64], data_short[32], data_abbr[16];
                const char *rw = (pi->proc_enum == PROCESS_WRITE_BYTE) ? "Write Data" : "Read Data";
                snprintf(data_str, sizeof(data_str), "%s : 0x%02llX", rw, (unsigned long long)s->io0data);
                snprintf(data_short, sizeof(data_short), "%s : 0x%02llX", (pi->proc_enum == PROCESS_WRITE_BYTE) ? "WR" : "RD", (unsigned long long)s->io0data);
                snprintf(data_abbr, sizeof(data_abbr), "0x%02llX", (unsigned long long)s->io0data);
                c_put(di, ss, es, s->out_ann, ANN_DATA_TRANSFER, data_str, data_short, data_abbr);
                s->state_count++;
                break;
            }
            case PROCESS_READ_BYTE_CONTINUOUS: {
                char data_str[64], data_short[32], data_abbr[16];
                snprintf(data_str, sizeof(data_str), "Read Data : 0x%02llX", (unsigned long long)s->io0data);
                snprintf(data_short, sizeof(data_short), "RD : 0x%02llX", (unsigned long long)s->io0data);
                snprintf(data_abbr, sizeof(data_abbr), "0x%02llX", (unsigned long long)s->io0data);
                c_put(di, ss, es, s->out_ann, ANN_DATA_TRANSFER, data_str, data_short, data_abbr);
                /* Don't increment state_count for continuous read */
                break;
            }
            case PROCESS_CONTINUOUS_READ_MODE_BITS: {
                c_put(di, ss, es, s->out_ann, ANN_DATA_TRANSFER, "Mode Bits", "M", "M");
                s->state_count++;
                break;
            }
            default:
                s->state_count++;
                break;
            }
        }
    }

    /* Store byte for transfer display */
    if (s->io0bytes_cnt < MAX_TRANSFER_BYTES) {
        s->io0bytes_ss[s->io0bytes_cnt] = ss;
        s->io0bytes_es[s->io0bytes_cnt] = es;
        s->io0bytes_val[s->io0bytes_cnt] = s->io0data;
        s->io0bytes_cnt++;
    }

    /* SPI protocol output: BITS v2 format */
    {
        int mosi_cnt = 1; /* IO0 = MOSI */
        int miso_cnt = s->have_io1 ? 1 : 0; /* IO1 = MISO */
        unsigned char bits_data[2200];
        int bpos = 0;

        bits_data[bpos++] = (1) | (s->have_io1 ? 2 : 0);
        bits_data[bpos++] = (unsigned char)mosi_cnt;

        for (int i = 0; i < mosi_cnt && bpos + 17 <= (int)sizeof(bits_data); i++) {
            bits_data[bpos++] = (unsigned char)s->io0bits_val[i];
            uint64_t ss_val = s->io0bits_ss[i];
            for (int b = 0; b < 8; b++) bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
            uint64_t es_val = s->io0bits_es[i];
            for (int b = 0; b < 8; b++) bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
        }

        bits_data[bpos++] = 0x00;
        bits_data[bpos++] = (unsigned char)miso_cnt;

        if (s->have_io1) {
            for (int i = 0; i < miso_cnt && bpos + 17 <= (int)sizeof(bits_data); i++) {
                bits_data[bpos++] = (unsigned char)s->io1bits_val[i];
                uint64_t ss_val = s->io1bits_ss[i];
                for (int b = 0; b < 8; b++) bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
                uint64_t es_val = s->io1bits_es[i];
                for (int b = 0; b < 8; b++) bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
            }
        }

        c_proto(di, ss, es, s->out_python, "BITS", C_BYTES(bits_data, bpos), C_END);
    }

    /* SPI DATA 17-byte format */
    {
        unsigned char data_data[17];
        int dpos = 0;
        data_data[dpos++] = 1 | (s->have_io1 ? 2 : 0);
        uint64_t mv = s->io0data;
        uint64_t sv = s->have_io1 ? s->io1data : 0;
        for (int i = 0; i < 8; i++) data_data[dpos++] = (unsigned char)(mv >> (8 * i));
        for (int i = 0; i < 8; i++) data_data[dpos++] = (unsigned char)(sv >> (8 * i));
        c_proto(di, ss, es, s->out_python, "DATA", C_BYTES(data_data, dpos), C_END);
    }
}

static void qspi_decode(struct srd_decoder_inst *di)
{
    qspi_state *s = (qspi_state *)c_decoder_get_private(di);
    int CLK = 0, IO0 = 1, IO1 = 2, IO2 = 3, IO3 = 4, CS = 5;

    /* Samplerate fallback */
    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
        if (s->samplerate > 0)
            s->bit_width = 1.0 / (double)s->samplerate;
    }

    if (!s->have_cs) {
        c_proto(di, 0, 0, s->out_python, "CS-CHANGE", C_END);
    }

    /* Get initial pin states */
    uint64_t cur_sample = 0;
    if (c_wait(di, CW_END) != SRD_OK)
        return;

    if (s->have_cs) {
        int cs = c_pin(di, CS);
        int cs_active = qspi_cs_asserted(s, cs);
        unsigned char cs_data[2];
        cs_data[0] = 0xFF;
        cs_data[1] = (unsigned char)cs;
        c_proto(di, cur_sample, cur_sample, s->out_python, "CS-CHANGE", C_U8(cs_data[0]), C_U8(cs_data[1]), C_END);
        if (cs_active) {
            s->ss_transfer = cur_sample;
            s->io0bytes_cnt = 0;
        }
    }

    while (1) {
        int ret;
        if (s->have_cs) {
            if (s->sample_edge_rise)
                ret = c_wait(di, CW_R(CLK), CW_OR, CW_E(CS), CW_END);
            else
                ret = c_wait(di, CW_F(CLK), CW_OR, CW_E(CS), CW_END);
        } else {
            if (s->sample_edge_rise)
                ret = c_wait(di, CW_R(CLK), CW_END);
            else
                ret = c_wait(di, CW_F(CLK), CW_END);
        }
        if (ret != SRD_OK)
            return;

        int clk = c_pin(di, CLK);
        int io0 = c_pin(di, IO0);
        int io1 = s->have_io1 ? c_pin(di, IO1) : 0;
        int io2 = s->have_io3 ? c_pin(di, IO2) : 0;
        int io3 = s->have_io3 ? c_pin(di, IO3) : 0;
        int cs = s->have_cs ? c_pin(di, CS) : 1;

        int clk_matched = (di_matched(di) & (1ULL << 0));
        int cs_matched = s->have_cs && (di_matched(di) & (1ULL << 1));

        int cs_active = s->have_cs ? qspi_cs_asserted(s, cs) : 1;

        if (cs_matched) {
            unsigned char cs_data[2];
            cs_data[0] = (unsigned char)(1 - cs);
            cs_data[1] = (unsigned char)cs;
            c_proto(di, di_samplenum(di), di_samplenum(di), s->out_python, "CS-CHANGE", C_U8(cs_data[0]), C_U8(cs_data[1]), C_END);

            if (cs_active) {
                s->ss_transfer = di_samplenum(di);
                s->io0bytes_cnt = 0;
            } else if (s->ss_transfer != (uint64_t)-1) {
                if (s->frame && s->io0bytes_cnt > 0) {
                    char transfer_str[4096];
                    int pos = 0;
                    for (int i = 0; i < s->io0bytes_cnt && pos < (int)sizeof(transfer_str) - 16; i++) {
                        if (i > 0) pos += snprintf(transfer_str + pos, sizeof(transfer_str) - pos, " ");
                        pos += snprintf(transfer_str + pos, sizeof(transfer_str) - pos, "%02llX", (unsigned long long)s->io0bytes_val[i]);
                    }
                    c_put(di, s->ss_transfer, di_samplenum(di), s->out_ann, ANN_DATA_TRANSFER, transfer_str);
                }
                c_proto(di, s->ss_transfer, di_samplenum(di), s->out_python, "TRANSFER", C_END);
                s->ss_transfer = (uint64_t)-1;
            }
            /* Reset decoder state on CS change */
            s->command = 0;
            s->state_count = 0;
            s->count = 0;
            s->bits_data = 0;
            qspi_reset_word(s);
        }

        if (s->have_cs && !cs_active)
            continue;

        if (!clk_matched)
            continue;

        /* Check correct clock edge */
        int mode;
        if (s->cpol == 0 && s->cpha == 0) mode = 0;
        else if (s->cpol == 0 && s->cpha == 1) mode = 1;
        else if (s->cpol == 1 && s->cpha == 0) mode = 2;
        else mode = 3;

        int correct_edge = 0;
        if ((mode == 0 && clk == 1) || (mode == 3 && clk == 1))
            correct_edge = 1;
        else if ((mode == 1 && clk == 0) || (mode == 2 && clk == 0))
            correct_edge = 2;
        if (!correct_edge) continue;

        /* Accumulate bits */
        if (s->bitcount == 0) {
            s->ss_block = di_samplenum(di);
            s->cs_was_deasserted = s->have_cs ? !qspi_cs_asserted(s, cs) : 0;
        }

        if (s->bitcount > 0 && s->bitcount < 8) {
            s->io0bits_es[s->bitcount - 1] = di_samplenum(di);
            if (s->have_io1) s->io1bits_es[s->bitcount - 1] = di_samplenum(di);
            if (s->have_io3) {
                s->io2bits_es[s->bitcount - 1] = di_samplenum(di);
                s->io3bits_es[s->bitcount - 1] = di_samplenum(di);
            }
        }

        if (s->bit_order == 0) { /* msb-first */
            s->io0data = (s->io0data << 1) | io0;
            if (s->have_io1) s->io1data = (s->io1data << 1) | io1;
            if (s->have_io3) {
                s->io2data = (s->io2data << 1) | io2;
                s->io3data = (s->io3data << 1) | io3;
            }
        } else {
            s->io0data |= ((uint64_t)io0 << s->bitcount);
            if (s->have_io1) s->io1data |= ((uint64_t)io1 << s->bitcount);
            if (s->have_io3) {
                s->io2data |= ((uint64_t)io2 << s->bitcount);
                s->io3data |= ((uint64_t)io3 << s->bitcount);
            }
        }

        if (s->bitcount < 8) {
            s->io0bits_ss[s->bitcount] = di_samplenum(di);
            s->io0bits_es[s->bitcount] = di_samplenum(di);
            s->io0bits_val[s->bitcount] = io0;
            if (s->have_io1) {
                s->io1bits_ss[s->bitcount] = di_samplenum(di);
                s->io1bits_es[s->bitcount] = di_samplenum(di);
                s->io1bits_val[s->bitcount] = io1;
            }
            if (s->have_io3) {
                s->io2bits_ss[s->bitcount] = di_samplenum(di);
                s->io2bits_es[s->bitcount] = di_samplenum(di);
                s->io2bits_val[s->bitcount] = io2;
                s->io3bits_ss[s->bitcount] = di_samplenum(di);
                s->io3bits_es[s->bitcount] = di_samplenum(di);
                s->io3bits_val[s->bitcount] = io3;
            }
        }

        s->bitcount++;
        if (s->bitcount != 8) continue;

        qspi_putdata(di, s);

        if (s->samplerate > 0) {
            double elapsed = 1.0 / (double)s->samplerate;
            elapsed *= (double)(di_samplenum(di) - s->ss_block + 1);
            int bitrate = (int)(1.0 / elapsed * 8);
            c_put_meta_int(di, s->ss_block, di_samplenum(di), s->out_bitrate, bitrate);
        }

        if (s->have_cs && s->cs_was_deasserted) {
            c_put(di, s->ss_block, di_samplenum(di), s->out_ann, ANN_OTHER,
                "CS# was deasserted during this data word!");
        }

        qspi_reset_word(s);
    }
}

static void qspi_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder qspi_c_decoder = {
    .id = "qspi_c",
    .name = "Smart QSPI(C)",
    .longname = "Quad Serial Peripheral Interface (C)",
    .desc = "Full-duplex, synchronous, serial bus. Compatible with Dual SPI and Quad SPI interfaces. (C implementation)",
    .license = "gplv2+",
    .channels = qspi_channels,
    .num_channels = 2,
    .optional_channels = qspi_optional_channels,
    .num_optional_channels = 4,
    .options = qspi_options,
    .num_options = 8,
    .num_annotations = NUM_ANN,
    .ann_labels = qspi_ann_labels,
    .num_annotation_rows = 8,
    .annotation_rows = qspi_ann_rows,
    .inputs = qspi_inputs,
    .num_inputs = 1,
    .outputs = qspi_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = qspi_tags,
    .num_tags = 1,
    .reset = qspi_reset,
    .start = qspi_start,
    .decode = qspi_decode,
    .destroy = qspi_destroy,
    .state_size = 0,
    .metadata = qspi_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    qspi_options[0].def = g_variant_new_string("active-low");
    qspi_options[1].def = g_variant_new_int64(0);
    qspi_options[2].def = g_variant_new_int64(0);
    qspi_options[3].def = g_variant_new_string("msb-first");
    qspi_options[4].def = g_variant_new_string("24-Bit Address");
    qspi_options[5].def = g_variant_new_string("no");
    qspi_options[6].def = g_variant_new_string("spi");
    qspi_options[7].def = g_variant_new_string("both");

    GSList *cs_pol_vals = NULL;
    cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-low"));
    cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-high"));
    qspi_options[0].values = cs_pol_vals;

    GSList *bitorder_vals = NULL;
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb-first"));
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb-first"));
    qspi_options[3].values = bitorder_vals;

    GSList *ads_vals = NULL;
    ads_vals = g_slist_append(ads_vals, g_variant_new_string("32-Bit Address"));
    ads_vals = g_slist_append(ads_vals, g_variant_new_string("24-Bit Address"));
    qspi_options[4].values = ads_vals;

    GSList *frame_vals = NULL;
    frame_vals = g_slist_append(frame_vals, g_variant_new_string("yes"));
    frame_vals = g_slist_append(frame_vals, g_variant_new_string("no"));
    qspi_options[5].values = frame_vals;

    GSList *twoln_vals = NULL;
    twoln_vals = g_slist_append(twoln_vals, g_variant_new_string("spi"));
    twoln_vals = g_slist_append(twoln_vals, g_variant_new_string("dspi"));
    twoln_vals = g_slist_append(twoln_vals, g_variant_new_string("qspi"));
    qspi_options[6].values = twoln_vals;

    GSList *inv_vals = NULL;
    inv_vals = g_slist_append(inv_vals, g_variant_new_string("both"));
    inv_vals = g_slist_append(inv_vals, g_variant_new_string("low"));
    inv_vals = g_slist_append(inv_vals, g_variant_new_string("high"));
    qspi_options[7].values = inv_vals;

    return &qspi_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}