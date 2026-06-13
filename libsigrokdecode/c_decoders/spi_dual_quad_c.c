/*
 * SPI Dual/Quad C decoder
 * Ported from Python spi-dual-quad decoder
 */

#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRANSFER_BYTES 256
#define MAX_BITS 64

enum spi_dq_ann {
    ANN_SPI_DATA = 0,
    ANN_SIO0_BIT,
    ANN_SIO1_BIT,
    ANN_SIO2_BIT,
    ANN_SIO3_BIT,
    ANN_WARNING,
    ANN_SPI_TRANSFER,
    NUM_ANN,
};

enum protocol_mode {
    PROTO_SPI = 0,
    PROTO_DUAL,
    PROTO_QUAD,
    PROTO_SQI,
};

enum current_mode {
    CUR_MODE_SPI = 0,
    CUR_MODE_DUAL,
    CUR_MODE_QUAD,
};

typedef struct {
    int cs_polarity;
    int cpol;
    int cpha;
    int bit_order;
    int wordsize;
    int twolnmd;
    int protocol;

    int have_cs;
    int have_sio2;
    int have_sio3;
    int is_quad;

    int current_mode;
    int command_phase;

    uint64_t samplerate;

    int bitcount;
    uint64_t spidata;
    int sio0bits_val[MAX_BITS]; uint64_t sio0bits_ss[MAX_BITS]; uint64_t sio0bits_es[MAX_BITS];
    int sio1bits_val[MAX_BITS]; uint64_t sio1bits_ss[MAX_BITS]; uint64_t sio1bits_es[MAX_BITS];
    int sio2bits_val[MAX_BITS]; uint64_t sio2bits_ss[MAX_BITS]; uint64_t sio2bits_es[MAX_BITS];
    int sio3bits_val[MAX_BITS]; uint64_t sio3bits_ss[MAX_BITS]; uint64_t sio3bits_es[MAX_BITS];

    uint64_t spibytes_ss[MAX_TRANSFER_BYTES]; uint64_t spibytes_es[MAX_TRANSFER_BYTES]; uint64_t spibytes_val[MAX_TRANSFER_BYTES];
    int spibytes_cnt;

    uint64_t ss_block;
    uint64_t ss_transfer;
    int cs_was_deasserted;
    int sample_edge_rise;

    int out_ann;
    int out_python;
    int out_binary;
    int out_bitrate;
    int bw;
} spi_dq_state;

static struct srd_channel spi_dq_channels[] = {
    { "clk", "CLK", "Clock", 0, SRD_CHANNEL_SCLK, "dec_spi_dual_quad_chan_clk" },
    { "sio0", "SIO0", "SPI Input/Output 0", 1, SRD_CHANNEL_SDATA, "dec_spi_dual_quad_chan_sio0" },
    { "sio1", "SIO1", "SPI Input/Output 1", 2, SRD_CHANNEL_SDATA, "dec_spi_dual_quad_chan_sio1" },
};

static struct srd_channel spi_dq_optional_channels[] = {
    { "sio2", "SIO2", "SPI Input/Output 2", 3, SRD_CHANNEL_SDATA, "dec_spi_dual_quad_opt_chan_sio2" },
    { "sio3", "SIO3", "SPI Input/Output 3", 4, SRD_CHANNEL_SDATA, "dec_spi_dual_quad_opt_chan_sio3" },
    { "cs", "CS#", "Chip-select", 5, SRD_CHANNEL_COMMON, "dec_spi_dual_quad_opt_chan_cs" },
};

static struct srd_decoder_option spi_dq_options[] = {
    { "cs_polarity", NULL, "CS# polarity", NULL, NULL },
    { "cpol", NULL, "Clock polarity", NULL, NULL },
    { "cpha", NULL, "Clock phase", NULL, NULL },
    { "bitorder", NULL, "Bit order", NULL, NULL },
    { "wordsize", NULL, "Word size", NULL, NULL },
    { "twolnmd", NULL, "Twolnmd", NULL, NULL },
    { "protocol", NULL, "protocol mode", NULL, NULL },
};

static const char *spi_dq_ann_labels[][3] = {
    { "", "spi-data", "SPI data" },
    { "", "sio0-bit", "SIO0 bit" },
    { "", "sio1-bit", "SIO1 bit" },
    { "", "sio2-bit", "SIO2 bit" },
    { "", "sio3-bit", "SIO3 bit" },
    { "", "warning", "Warning" },
    { "", "spi-transfer", "SPI transfer" },
};

static const int spi_dq_row_sio0_classes[] = { ANN_SIO0_BIT, -1 };
static const int spi_dq_row_sio1_classes[] = { ANN_SIO1_BIT, -1 };
static const int spi_dq_row_sio2_classes[] = { ANN_SIO2_BIT, -1 };
static const int spi_dq_row_sio3_classes[] = { ANN_SIO3_BIT, -1 };
static const int spi_dq_row_data_classes[] = { ANN_SPI_DATA, -1 };
static const int spi_dq_row_transfer_classes[] = { ANN_SPI_TRANSFER, -1 };
static const int spi_dq_row_other_classes[] = { ANN_WARNING, -1 };

static const struct srd_c_ann_row spi_dq_ann_rows[] = {
    { "sio0-bits", "SIO0 bits", spi_dq_row_sio0_classes, 1 },
    { "sio1-bits", "SIO1 bits", spi_dq_row_sio1_classes, 1 },
    { "sio2-bits", "SIO2 bits", spi_dq_row_sio2_classes, 1 },
    { "sio3-bits", "SIO3 bits", spi_dq_row_sio3_classes, 1 },
    { "spi-data-vals", "SPI data", spi_dq_row_data_classes, 1 },
    { "spi-transfers", "SPI transfers", spi_dq_row_transfer_classes, 1 },
    { "other", "Other", spi_dq_row_other_classes, 1 },
};

static const struct srd_decoder_binary spi_dq_binary[] = {
    { 0, "spi-data", "SPI Data" },
};

static const char *spi_dq_inputs[] = { "logic" };
static const char *spi_dq_outputs[] = { "spi" };
static const char *spi_dq_tags[] = { "Embedded/industrial" };

static int spi_dq_cs_asserted(spi_dq_state *s, int cs_val)
{
    return (s->cs_polarity == 0) ? (cs_val == 0) : (cs_val == 1);
}

static void spi_dq_reset_word(spi_dq_state *s)
{
    s->bitcount = 0;
    s->spidata = 0;
}

static void spi_dq_reset_decoder_state(spi_dq_state *s)
{
    s->spidata = 0;
    s->bitcount = 0;
    if (s->protocol == PROTO_SQI) {
        s->command_phase = 1;
        s->current_mode = CUR_MODE_SPI;
        s->is_quad = 0;
    }
}

static void spi_dq_putdata(struct srd_decoder_inst *di, spi_dq_state *s)
{
    int ws = s->wordsize;
    uint64_t ss = 0, es = 0;

    /* Get ss/es from bit records */
    if (s->current_mode == CUR_MODE_QUAD) {
        ss = s->sio0bits_ss[0];
        es = s->sio0bits_es[ws / 4 - 1];
    } else if (s->current_mode == CUR_MODE_DUAL) {
        ss = s->sio0bits_ss[0];
        es = s->sio0bits_es[ws / 2 - 1];
    } else {
        ss = s->sio0bits_ss[0];
        es = s->sio0bits_es[ws - 1];
    }

    /* Guesstimate: extend the last bit's end_sample by one bit period */
    {
        int total_bit_count = (s->current_mode == CUR_MODE_QUAD) ? ws / 4 :
                              (s->current_mode == CUR_MODE_DUAL) ? ws / 2 : ws;
        if (total_bit_count > 1) {
            uint64_t last_ss = s->sio0bits_ss[total_bit_count - 1];
            uint64_t prev_ss = s->sio0bits_ss[total_bit_count - 2];
            uint64_t guesstimate = last_ss + (last_ss - prev_ss);
            s->sio0bits_es[total_bit_count - 1] = guesstimate;
            s->sio1bits_es[total_bit_count - 1] = guesstimate;
            if (s->is_quad) {
                s->sio2bits_es[total_bit_count - 1] = guesstimate;
                s->sio3bits_es[total_bit_count - 1] = guesstimate;
            }
            es = guesstimate;
        }
    }

    /* Binary output */
    {
        unsigned char bdata[8];
        int bw = (ws + 7) / 8;
        for (int i = 0; i < bw; i++)
            bdata[i] = (unsigned char)(s->spidata >> (8 * (bw - 1 - i)));
        c_put_bin(di, ss, es, s->out_binary, 0, bw, bdata);
    }

    /* BITS v2 format */
    {
        int bit_count = (s->current_mode == CUR_MODE_QUAD) ? ws / 4 :
                        (s->current_mode == CUR_MODE_DUAL) ? ws / 2 : ws;
        int sio0_cnt = bit_count;
        int sio1_cnt = bit_count;
        int sio2_cnt = s->is_quad ? bit_count : 0;
        int sio3_cnt = s->is_quad ? bit_count : 0;
        
        unsigned char bits_buf[8800];
        int bpos = 0;

        bits_buf[bpos++] = (unsigned char)(s->is_quad ? 0x0F : 0x03); /* SIO lines present */
        bits_buf[bpos++] = (unsigned char)sio0_cnt;
        for (int i = sio0_cnt - 1; i >= 0 && bpos + 17 <= (int)sizeof(bits_buf); i--) {
            bits_buf[bpos++] = (unsigned char)s->sio0bits_val[i];
            for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio0bits_ss[i] >> (8 * b));
            for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio0bits_es[i] >> (8 * b));
        }
        bits_buf[bpos++] = 0x00;
        bits_buf[bpos++] = (unsigned char)sio1_cnt;
        for (int i = sio1_cnt - 1; i >= 0 && bpos + 17 <= (int)sizeof(bits_buf); i--) {
            bits_buf[bpos++] = (unsigned char)s->sio1bits_val[i];
            for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio1bits_ss[i] >> (8 * b));
            for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio1bits_es[i] >> (8 * b));
        }
        if (s->is_quad) {
            bits_buf[bpos++] = 0x00;
            bits_buf[bpos++] = (unsigned char)sio2_cnt;
            for (int i = sio2_cnt - 1; i >= 0 && bpos + 17 <= (int)sizeof(bits_buf); i--) {
                bits_buf[bpos++] = (unsigned char)s->sio2bits_val[i];
                for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio2bits_ss[i] >> (8 * b));
                for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio2bits_es[i] >> (8 * b));
            }
            bits_buf[bpos++] = 0x00;
            bits_buf[bpos++] = (unsigned char)sio3_cnt;
            for (int i = sio3_cnt - 1; i >= 0 && bpos + 17 <= (int)sizeof(bits_buf); i--) {
                bits_buf[bpos++] = (unsigned char)s->sio3bits_val[i];
                for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio3bits_ss[i] >> (8 * b));
                for (int b = 0; b < 8; b++) bits_buf[bpos++] = (unsigned char)(s->sio3bits_es[i] >> (8 * b));
            }
        }
        c_proto(di, ss, es, s->out_python, "BITS", C_BYTES(bits_buf, bpos), C_END);
    }

    /* DATA 17-byte format */
    {
        unsigned char data_data[17];
        int dpos = 0;
        data_data[dpos++] = 1 | 2; /* have mosi + miso */
        uint64_t mv = s->spidata;
        uint64_t sv = s->spidata; /* same data for both */
        for (int i = 0; i < 8; i++) data_data[dpos++] = (unsigned char)(mv >> (8 * i));
        for (int i = 0; i < 8; i++) data_data[dpos++] = (unsigned char)(sv >> (8 * i));
        c_proto(di, ss, es, s->out_python, "DATA", C_BYTES(data_data, dpos), C_END);
    }

    /* Store byte for transfer display */
    if (s->spibytes_cnt < MAX_TRANSFER_BYTES) {
        s->spibytes_ss[s->spibytes_cnt] = ss;
        s->spibytes_es[s->spibytes_cnt] = es;
        s->spibytes_val[s->spibytes_cnt] = s->spidata;
        s->spibytes_cnt++;
    }

    /* Bit annotations */
    int total_bit_count = (s->current_mode == CUR_MODE_QUAD) ? ws / 4 :
                          (s->current_mode == CUR_MODE_DUAL) ? ws / 2 : ws;
    for (int i = total_bit_count - 1; i >= 0 && i < MAX_BITS; i--) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->sio0bits_val[i]);
        c_put(di, s->sio0bits_ss[i], s->sio0bits_es[i], s->out_ann, ANN_SIO0_BIT, bit_str);
    }
    for (int i = total_bit_count - 1; i >= 0 && i < MAX_BITS; i--) {
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%d", s->sio1bits_val[i]);
        c_put(di, s->sio1bits_ss[i], s->sio1bits_es[i], s->out_ann, ANN_SIO1_BIT, bit_str);
    }
    if (s->is_quad) {
        for (int i = total_bit_count - 1; i >= 0 && i < MAX_BITS; i--) {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", s->sio2bits_val[i]);
            c_put(di, s->sio2bits_ss[i], s->sio2bits_es[i], s->out_ann, ANN_SIO2_BIT, bit_str);
        }
        for (int i = total_bit_count - 1; i >= 0 && i < MAX_BITS; i--) {
            char bit_str[16];
            snprintf(bit_str, sizeof(bit_str), "%d", s->sio3bits_val[i]);
            c_put(di, s->sio3bits_ss[i], s->sio3bits_es[i], s->out_ann, ANN_SIO3_BIT, bit_str);
        }
    }

    /* Data word annotation */
    {
        char data_str[16];
        snprintf(data_str, sizeof(data_str), "%02llX", (unsigned long long)s->spidata);
        c_put(di, ss, es, s->out_ann, ANN_SPI_DATA, data_str);
    }
}

static void spi_dq_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(spi_dq_state)));
    }
    spi_dq_state *s = (spi_dq_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(spi_dq_state));
    s->cs_polarity = 0;
    s->cpol = 0;
    s->cpha = 0;
    s->bit_order = 0;
    s->wordsize = 8;
    s->twolnmd = 0;
    s->protocol = PROTO_SPI;
    s->current_mode = CUR_MODE_SPI;
    s->ss_transfer = (uint64_t)-1;
    s->out_ann = -1;
    s->out_python = -1;
    s->out_binary = -1;
    s->out_bitrate = -1;
    s->bw = 1;
    s->command_phase = 1;
}

static void spi_dq_start(struct srd_decoder_inst *di)
{
    spi_dq_state *s = (spi_dq_state *)c_decoder_get_private(di);
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

    s->wordsize = (int)c_opt_int(di, "wordsize", 8);
    if (s->wordsize < 1 || s->wordsize > 64) s->wordsize = 8;
    s->bw = (s->wordsize + 7) / 8;

    const char *twoln_str = c_opt_str(di, "twolnmd", "qspi");
    if (strcmp(twoln_str, "qspi") == 0)
        s->twolnmd = 1;
    else if (strcmp(twoln_str, "dspi") == 0)
        s->twolnmd = 2;
    else
        s->twolnmd = 0;

    const char *proto_str = c_opt_str(di, "protocol", "spi");
    if (strcmp(proto_str, "dual") == 0) {
        s->protocol = PROTO_DUAL;
        s->current_mode = CUR_MODE_DUAL;
        s->is_quad = 0;
    } else if (strcmp(proto_str, "quad") == 0) {
        s->protocol = PROTO_QUAD;
        s->current_mode = CUR_MODE_QUAD;
        s->is_quad = 1;
    } else if (strcmp(proto_str, "sqi") == 0) {
        s->protocol = PROTO_SQI;
        s->current_mode = CUR_MODE_SPI;
        s->is_quad = 0;
        s->command_phase = 1;
    } else {
        s->protocol = PROTO_SPI;
        s->current_mode = CUR_MODE_SPI;
        s->is_quad = 0;
    }

    s->have_sio2 = c_has_ch(di, 3);
    s->have_sio3 = c_has_ch(di, 4);
    s->have_cs = c_has_ch(di, 5);

    if (s->twolnmd == 1)
        s->is_quad = 1;

    int mode;
    if (s->cpol == 0 && s->cpha == 0) mode = 0;
    else if (s->cpol == 0 && s->cpha == 1) mode = 1;
    else if (s->cpol == 1 && s->cpha == 0) mode = 2;
    else mode = 3;
    s->sample_edge_rise = (mode == 0 || mode == 3) ? 1 : 0;
}

static void spi_dq_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    spi_dq_state *s = (spi_dq_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void spi_dq_decode(struct srd_decoder_inst *di)
{
    spi_dq_state *s = (spi_dq_state *)c_decoder_get_private(di);
    int CLK = 0, SIO0 = 1, SIO1 = 2, SIO2 = 3, SIO3 = 4, CS = 5;

    if (s->samplerate == 0) {
        s->samplerate = c_samplerate(di);
    }

    if (!s->have_cs) {
        c_proto(di, 0, 0, s->out_python, "CS-CHANGE",
                C_I8(-1), C_I8(-1), C_END);
    }

    /* Get initial pin states — match Python: self.wait({}) */
    if (c_wait(di, CW_END) != SRD_OK)
        return;

    if (s->have_cs) {
        uint64_t sn = di_samplenum(di);
        int cs = c_pin(di, CS);
        int cs_active = spi_dq_cs_asserted(s, cs);
        c_proto(di, sn, sn, s->out_python, "CS-CHANGE",
                C_I8(-1), C_I8(cs), C_END);
        if (cs_active) {
            s->ss_transfer = sn;
            s->spibytes_cnt = 0;
        }
    }

    while (1) {
            int ret;
            if (s->have_cs)
                ret = c_wait(di, CW_E(CLK), CW_OR, CW_E(CS), CW_END);
            else
                ret = c_wait(di, CW_E(CLK), CW_END);
        if (ret != SRD_OK)
                return;

        int clk = c_pin(di, CLK);
        int sio0 = c_pin(di, SIO0);
        int sio1 = c_pin(di, SIO1);
        int sio2 = s->have_sio2 ? c_pin(di, SIO2) : 0;
        int sio3 = s->have_sio3 ? c_pin(di, SIO3) : 0;
        int cs = s->have_cs ? c_pin(di, CS) : 1;

        int clk_matched = (di_matched(di) & (1ULL << 0));
        int cs_matched = s->have_cs && (di_matched(di) & (1ULL << 1));

        int cs_active = s->have_cs ? spi_dq_cs_asserted(s, cs) : 1;

        if (cs_matched) {
            int oldcs = 1 - cs;
            c_proto(di, di_samplenum(di), di_samplenum(di), s->out_python, "CS-CHANGE",
                    C_I8(oldcs), C_I8(cs), C_END);

            if (cs_active) {
                s->ss_transfer = di_samplenum(di);
                s->spibytes_cnt = 0;
            } else if (s->ss_transfer != (uint64_t)-1) {
                /* Output transfer */
                if (s->spibytes_cnt > 0) {
                    char transfer_str[4096];
                    int pos = 0;
                    
                    if (s->protocol == PROTO_DUAL) {
                        pos += snprintf(transfer_str + pos, sizeof(transfer_str) - pos, "DUAL: ");
                    } else if (s->protocol == PROTO_QUAD) {
                        pos += snprintf(transfer_str + pos, sizeof(transfer_str) - pos, "QUAD: ");
                    }
                    for (int i = 0; i < s->spibytes_cnt && pos < (int)sizeof(transfer_str) - 16; i++) {
                        if (i > 0) pos += snprintf(transfer_str + pos, sizeof(transfer_str) - pos, " ");
                        pos += snprintf(transfer_str + pos, sizeof(transfer_str) - pos, "%02llX", (unsigned long long)s->spibytes_val[i]);
                    }
                    c_put(di, s->ss_transfer, di_samplenum(di), s->out_ann, ANN_SPI_TRANSFER, transfer_str);
                }
                c_proto(di, s->ss_transfer, di_samplenum(di), s->out_python, "TRANSFER", C_END);
                s->ss_transfer = (uint64_t)-1;
            }
            spi_dq_reset_decoder_state(s);
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

        /* SQI mode: command phase check */
        if (s->protocol == PROTO_SQI && s->command_phase) {
            if (s->bitcount >= 8) {
                s->command_phase = 0;
                s->current_mode = CUR_MODE_QUAD;
                s->is_quad = 1;
                s->bitcount = 0;
                s->spidata = 0;
            }
        }

        int ws = s->wordsize;

        /* If this is the first bit of a dataword, save its sample number.
         * Python: if self.bitcount == 0: self.ss_block = self.samplenum */
        if (s->bitcount == 0) {
            s->ss_block = di_samplenum(di);
            s->cs_was_deasserted = s->have_cs ? !spi_dq_cs_asserted(s, cs) : 0;
        }

        /* Accumulate bits based on current mode */
        if (s->current_mode == CUR_MODE_QUAD) {
            if (s->bit_order == 0) { /* msb-first */
                int shift = ws - 4 - s->bitcount;
                if (shift >= 0) {
                    s->spidata |= (uint64_t)sio3 << (shift + 3);
                    s->spidata |= (uint64_t)sio2 << (shift + 2);
                    s->spidata |= (uint64_t)sio1 << (shift + 1);
                    s->spidata |= (uint64_t)sio0 << shift;
                }
            } else {
                int shift = s->bitcount;
                s->spidata |= (uint64_t)sio0 << shift;
                s->spidata |= (uint64_t)sio1 << (shift + 1);
                s->spidata |= (uint64_t)sio2 << (shift + 2);
                s->spidata |= (uint64_t)sio3 << (shift + 3);
            }
            /* Record bits */
            {
                int idx = s->bitcount / 4;
                if (idx < MAX_BITS) {
                    s->sio0bits_val[idx] = sio0;
                    s->sio0bits_ss[idx] = di_samplenum(di);
                    s->sio0bits_es[idx] = di_samplenum(di);
                    s->sio1bits_val[idx] = sio1;
                    s->sio1bits_ss[idx] = di_samplenum(di);
                    s->sio1bits_es[idx] = di_samplenum(di);
                    s->sio2bits_val[idx] = sio2;
                    s->sio2bits_ss[idx] = di_samplenum(di);
                    s->sio2bits_es[idx] = di_samplenum(di);
                    s->sio3bits_val[idx] = sio3;
                    s->sio3bits_ss[idx] = di_samplenum(di);
                    s->sio3bits_es[idx] = di_samplenum(di);
                    if (idx > 0) {
                        s->sio0bits_es[idx - 1] = di_samplenum(di);
                        s->sio1bits_es[idx - 1] = di_samplenum(di);
                        s->sio2bits_es[idx - 1] = di_samplenum(di);
                        s->sio3bits_es[idx - 1] = di_samplenum(di);
                    }
                }
            }
            s->bitcount += 4;
        } else if (s->current_mode == CUR_MODE_DUAL) {
            if (s->bit_order == 0) {
                s->spidata |= (uint64_t)sio1 << (ws - 1 - s->bitcount);
                s->spidata |= (uint64_t)sio0 << (ws - 1 - s->bitcount - 1);
            } else {
                s->spidata |= (uint64_t)sio1 << s->bitcount;
                s->spidata |= (uint64_t)sio0 << (s->bitcount + 1);
            }
            {
                int idx = s->bitcount / 2;
                if (idx < MAX_BITS) {
                    s->sio0bits_val[idx] = sio0;
                    s->sio0bits_ss[idx] = di_samplenum(di);
                    s->sio0bits_es[idx] = di_samplenum(di);
                    s->sio1bits_val[idx] = sio1;
                    s->sio1bits_ss[idx] = di_samplenum(di);
                    s->sio1bits_es[idx] = di_samplenum(di);
                    if (idx > 0) {
                        s->sio0bits_es[idx - 1] = di_samplenum(di);
                        s->sio1bits_es[idx - 1] = di_samplenum(di);
                    }
                }
            }
            s->bitcount += 2;
        } else {
            /* SPI mode */
            if (s->bit_order == 0)
                s->spidata |= (uint64_t)sio0 << (ws - 1 - s->bitcount);
            else
                s->spidata |= (uint64_t)sio0 << s->bitcount;

            if (s->bitcount < MAX_BITS) {
                s->sio0bits_val[s->bitcount] = sio0;
                s->sio0bits_ss[s->bitcount] = di_samplenum(di);
                s->sio0bits_es[s->bitcount] = di_samplenum(di);
                s->sio1bits_val[s->bitcount] = sio1;
                s->sio1bits_ss[s->bitcount] = di_samplenum(di);
                s->sio1bits_es[s->bitcount] = di_samplenum(di);
                if (s->is_quad) {
                    s->sio2bits_val[s->bitcount] = sio2;
                    s->sio2bits_ss[s->bitcount] = di_samplenum(di);
                    s->sio2bits_es[s->bitcount] = di_samplenum(di);
                    s->sio3bits_val[s->bitcount] = sio3;
                    s->sio3bits_ss[s->bitcount] = di_samplenum(di);
                    s->sio3bits_es[s->bitcount] = di_samplenum(di);
                }
                if (s->bitcount > 0) {
                    s->sio0bits_es[s->bitcount - 1] = di_samplenum(di);
                    s->sio1bits_es[s->bitcount - 1] = di_samplenum(di);
                    if (s->is_quad) {
                        s->sio2bits_es[s->bitcount - 1] = di_samplenum(di);
                        s->sio3bits_es[s->bitcount - 1] = di_samplenum(di);
                    }
                }
            }
            s->bitcount += 1;
        }

        if (s->bitcount < ws) continue;

        spi_dq_putdata(di, s);

        if (s->samplerate > 0) {
            double elapsed = 1.0 / (double)s->samplerate;
            elapsed *= (double)(di_samplenum(di) - s->ss_block + 1);
            int bitrate = (int)(1.0 / elapsed * ws);
            c_put_meta_int(di, s->ss_block, di_samplenum(di), s->out_bitrate, bitrate);
        }

        if (s->have_cs && s->cs_was_deasserted) {
            c_put(di, s->ss_block, di_samplenum(di), s->out_ann, ANN_WARNING,
                "CS# was deasserted during this data word!");
        }

        spi_dq_reset_word(s);
    }
}

static void spi_dq_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder spi_dual_quad_c_decoder = {
    .id = "spi_dual_quad_c",
    .name = "SPI Dual/Quad(C)",
    .longname = "Dual/Quad Serial Peripheral Interface (C)",
    .desc = "Full-duplex, synchronous, serial bus. (C implementation)",
    .license = "gplv2+",
    .channels = spi_dq_channels,
    .num_channels = 3,
    .optional_channels = spi_dq_optional_channels,
    .num_optional_channels = 3,
    .options = spi_dq_options,
    .num_options = 7,
    .num_annotations = NUM_ANN,
    .ann_labels = spi_dq_ann_labels,
    .num_annotation_rows = 7,
    .annotation_rows = spi_dq_ann_rows,
    .inputs = spi_dq_inputs,
    .num_inputs = 1,
    .outputs = spi_dq_outputs,
    .num_outputs = 1,
    .binary = spi_dq_binary,
    .num_binary = 1,
    .tags = spi_dq_tags,
    .num_tags = 1,
    .reset = spi_dq_reset,
    .start = spi_dq_start,
    .decode = spi_dq_decode,
    .destroy = spi_dq_destroy,
    .state_size = 0,
    .metadata = spi_dq_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    spi_dq_options[0].def = g_variant_new_string("active-low");
    spi_dq_options[1].def = g_variant_new_int64(0);
    spi_dq_options[2].def = g_variant_new_int64(0);
    spi_dq_options[3].def = g_variant_new_string("msb-first");
    spi_dq_options[4].def = g_variant_new_int64(8);
    spi_dq_options[5].def = g_variant_new_string("qspi");
    spi_dq_options[6].def = g_variant_new_string("spi");

    GSList *cs_pol_vals = NULL;
    cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-low"));
    cs_pol_vals = g_slist_append(cs_pol_vals, g_variant_new_string("active-high"));
    spi_dq_options[0].values = cs_pol_vals;

    GSList *bitorder_vals = NULL;
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb-first"));
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb-first"));
    spi_dq_options[3].values = bitorder_vals;

    GSList *twoln_vals = NULL;
    twoln_vals = g_slist_append(twoln_vals, g_variant_new_string("spi"));
    twoln_vals = g_slist_append(twoln_vals, g_variant_new_string("qspi"));
    twoln_vals = g_slist_append(twoln_vals, g_variant_new_string("dspi"));
    spi_dq_options[5].values = twoln_vals;

    GSList *proto_vals = NULL;
    proto_vals = g_slist_append(proto_vals, g_variant_new_string("spi"));
    proto_vals = g_slist_append(proto_vals, g_variant_new_string("dual"));
    proto_vals = g_slist_append(proto_vals, g_variant_new_string("quad"));
    proto_vals = g_slist_append(proto_vals, g_variant_new_string("sqi"));
    spi_dq_options[6].values = proto_vals;

    return &spi_dual_quad_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}