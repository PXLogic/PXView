#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TRANSFER_BYTES 256

enum spi_fast_ann {
    ANN_MISO_DATA = 0,
    ANN_MOSI_DATA,
    ANN_ATK_DATA_POINT,
    ANN_ATK_RISING_EDGE,
    ANN_ATK_FALLING_EDGE,
    NUM_ANN,
};

typedef struct {
    uint64_t samplerate;
    int have_miso;
    int have_mosi;
    int have_cs;
    int cs_active;

    int cpol;
    int cpha;
    int bit_order;
    int wordsize;
    int format;
    int show_data_point;
    int cs_polarity;
    int bw;

    int bit_count;
    uint64_t miso_byte;
    uint64_t mosi_byte;
    uint64_t start_sample;
    uint64_t last_bit_sample;
    int cs_was_deasserted;

    uint64_t miso_bits_ss[64];
    uint64_t miso_bits_es[64];
    int miso_bits_val[64];
    uint64_t mosi_bits_ss[64];
    uint64_t mosi_bits_es[64];
    int mosi_bits_val[64];

    uint64_t misobytes_val[MAX_TRANSFER_BYTES];
    int misobytes_cnt;
    uint64_t mosibytes_val[MAX_TRANSFER_BYTES];
    int mosibytes_cnt;
    uint64_t transfer_start;

    int sample_edge_rise;

    int out_ann;
    int out_python;
    int out_binary;
    int out_bitrate;
} spi_fast_state;

static struct srd_channel spi_fast_channels[] = {
    {"clk", "CLK", "Clock(串行时钟)", 0, SRD_CHANNEL_SCLK, NULL},
};

static struct srd_channel spi_fast_optional_channels[] = {
    {"miso", "MISO", "Master in, slave out(主入从出)", 1, SRD_CHANNEL_SDATA, NULL},
    {"mosi", "MOSI", "Master out, slave in(主出从入)", 2, SRD_CHANNEL_SDATA, NULL},
    {"cs", "CS#", "Chip-select(片选信号)", 3, SRD_CHANNEL_COMMON, NULL},
};

static struct srd_decoder_option spi_fast_options[] = {
    {"cs_polarity", NULL, "CS# polarity(片选极性)", NULL, NULL},
    {"cpol", NULL, "Clock polarity(时钟极性)", NULL, NULL},
    {"cpha", NULL, "Clock phase(时钟相位)", NULL, NULL},
    {"bitorder", NULL, "Bit order(位序)", NULL, NULL},
    {"wordsize", NULL, "Word size(字长)", NULL, NULL},
    {"format", NULL, "Data format(数据格式)", NULL, NULL},
    {"show_data_point", NULL, "Show data point(数据点显示)", NULL, NULL},
};

static const char *spi_fast_ann_labels[][3] = {
    {"", "miso-data", "MISO data"},
    {"", "mosi-data", "MOSI data"},
    {"", "atk-data-point", "ATK Data point"},
    {"", "atk-rising-edge", "ATK Rising edge"},
    {"", "atk-falling-edge", "ATK Falling edge"},
};

static const int row_miso_classes[] = {ANN_MISO_DATA, -1};
static const int row_mosi_classes[] = {ANN_MOSI_DATA, -1};
static const int row_atk_classes[] = {ANN_ATK_DATA_POINT, ANN_ATK_RISING_EDGE, ANN_ATK_FALLING_EDGE, -1};

static const struct srd_c_ann_row spi_fast_ann_rows[] = {
    {"miso-data-vals", "MISO data", row_miso_classes, 1},
    {"mosi-data-vals", "MOSI data", row_mosi_classes, 1},
    {"atk-signs", "ATK signs", row_atk_classes, 3},
};

static const struct srd_decoder_binary spi_fast_binary[] = {
    {0, "miso", "MISO"},
    {1, "mosi", "MOSI"},
};

static const char *spi_fast_inputs[] = {"logic"};
static const char *spi_fast_outputs[] = {"spi"};
static const char *spi_fast_tags[] = {"Embedded/industrial"};

static void spi_fast_format_value(uint64_t val, int wordsize, int format,
                                   char *buf, int bufsize)
{
    switch (format) {
    case 0: /* ascii */
        if (val >= 32 && val <= 126)
            snprintf(buf, bufsize, "%c", (char)val);
        else
            snprintf(buf, bufsize, "%02llx", (unsigned long long)val);
        break;
    case 1: /* dec */
        snprintf(buf, bufsize, "%llu", (unsigned long long)val);
        break;
    case 2: /* hex */
        snprintf(buf, bufsize, "%02llx", (unsigned long long)val);
        break;
    case 3: /* oct */
        snprintf(buf, bufsize, "%03llo", (unsigned long long)val);
        break;
    case 4: /* bin */
        for (int i = wordsize - 1; i >= 0; i--)
            buf[wordsize - 1 - i] = ((val >> i) & 1) + '0';
        buf[wordsize] = '\0';
        break;
    default:
        snprintf(buf, bufsize, "%02llx", (unsigned long long)val);
        break;
    }
}

static void spi_fast_format_transfer(uint64_t *vals, int cnt, int format,
                                      int wordsize, char *out, int out_size)
{
    int pos = 0;
    out[0] = '\0';
    for (int i = 0; i < cnt && pos < out_size - 16; i++) {
        if (i > 0 && format != 4)
            pos += snprintf(out + pos, out_size - pos, " ");
        if (format == 0) {
            pos += snprintf(out + pos, out_size - pos, "%02llx", (unsigned long long)vals[i]);
        } else if (format == 1) {
            pos += snprintf(out + pos, out_size - pos, "%llu", (unsigned long long)vals[i]);
        } else if (format == 2) {
            pos += snprintf(out + pos, out_size - pos, "%03llo", (unsigned long long)vals[i]);
        } else if (format == 3) {
            char btmp[65];
            int bwidth = wordsize > 8 ? wordsize : 8;
            for (int b = 0; b < bwidth; b++) {
                int bit_idx = bwidth - 1 - b;
                btmp[b] = ((vals[i] >> bit_idx) & 1) ? '1' : '0';
            }
            btmp[bwidth] = '\0';
            pos += snprintf(out + pos, out_size - pos, "%s", btmp);
        } else {
            if (vals[i] >= 32 && vals[i] <= 126)
                pos += snprintf(out + pos, out_size - pos, "%c", (char)vals[i]);
            else
                pos += snprintf(out + pos, out_size - pos, "\\x%02llx", (unsigned long long)vals[i]);
        }
    }
}

static int spi_fast_cs_asserted(spi_fast_state *s, int cs_val)
{
    return (s->cs_polarity == 0) ? (cs_val == 0) : (cs_val == 1);
}

static void spi_fast_reset_word(spi_fast_state *s)
{
    s->bit_count = 0;
    s->mosi_byte = 0;
    s->miso_byte = 0;
}

static void spi_fast_put_data(struct srd_decoder_inst *di, spi_fast_state *s)
{
    uint64_t ss = s->start_sample;
    uint64_t es = s->last_bit_sample;

    if (s->have_miso) {
        uint64_t miso_ss = s->miso_bits_ss[0];
        uint64_t miso_es = s->miso_bits_es[s->wordsize - 1];
        unsigned char bdata[8];
        for (int i = 0; i < s->bw; i++)
            bdata[i] = (unsigned char)(s->miso_byte >> (8 * (s->bw - 1 - i)));
        c_put_bin(di, miso_ss, miso_es, s->out_binary, 0, s->bw, bdata);
        ss = miso_ss;
        es = miso_es;
    }
    if (s->have_mosi) {
        uint64_t mosi_ss = s->mosi_bits_ss[0];
        uint64_t mosi_es = s->mosi_bits_es[s->wordsize - 1];
        unsigned char bdata[8];
        for (int i = 0; i < s->bw; i++)
            bdata[i] = (unsigned char)(s->mosi_byte >> (8 * (s->bw - 1 - i)));
        c_put_bin(di, mosi_ss, mosi_es, s->out_binary, 1, s->bw, bdata);
        ss = mosi_ss;
        es = mosi_es;
    }

    /* BITS v2 format */
    {
        int mosi_cnt = s->have_mosi ? s->wordsize : 0;
        int miso_cnt = s->have_miso ? s->wordsize : 0;
        unsigned char bits_data[2200];
        int bpos = 0;

        bits_data[bpos++] = (s->have_mosi ? 1 : 0) | (s->have_miso ? 2 : 0);
        bits_data[bpos++] = (unsigned char)mosi_cnt;

        for (int i = 0; i < mosi_cnt && bpos + 17 <= (int)sizeof(bits_data); i++) {
            bits_data[bpos++] = (unsigned char)s->mosi_bits_val[i];
            uint64_t ss_val = s->mosi_bits_ss[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
            uint64_t es_val = s->mosi_bits_es[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
        }

        bits_data[bpos++] = 0x00;
        bits_data[bpos++] = (unsigned char)miso_cnt;

        for (int i = 0; i < miso_cnt && bpos + 17 <= (int)sizeof(bits_data); i++) {
            bits_data[bpos++] = (unsigned char)s->miso_bits_val[i];
            uint64_t ss_val = s->miso_bits_ss[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(ss_val >> (8 * b));
            uint64_t es_val = s->miso_bits_es[i];
            for (int b = 0; b < 8; b++)
                bits_data[bpos++] = (unsigned char)(es_val >> (8 * b));
        }

        c_proto(di, ss, es, s->out_python, "BITS", C_BYTES(bits_data, bpos), C_END);
    }

    /* DATA 17-byte format */
    {
        unsigned char data_data[17];
        int dpos = 0;
        data_data[dpos++] = (s->have_mosi ? 1 : 0) | (s->have_miso ? 2 : 0);
        uint64_t mv = s->have_mosi ? s->mosi_byte : 0;
        uint64_t sv = s->have_miso ? s->miso_byte : 0;
        for (int i = 0; i < 8; i++)
            data_data[dpos++] = (unsigned char)(mv >> (8 * i));
        for (int i = 0; i < 8; i++)
            data_data[dpos++] = (unsigned char)(sv >> (8 * i));
        c_proto(di, ss, es, s->out_python, "DATA", C_BYTES(data_data, dpos), C_END);
    }

    if (s->have_miso && s->misobytes_cnt < MAX_TRANSFER_BYTES) {
        s->misobytes_val[s->misobytes_cnt] = s->miso_byte;
        s->misobytes_cnt++;
    }
    if (s->have_mosi && s->mosibytes_cnt < MAX_TRANSFER_BYTES) {
        s->mosibytes_val[s->mosibytes_cnt] = s->mosi_byte;
        s->mosibytes_cnt++;
    }

    if (s->have_miso) {
        char miso_str[128];
        spi_fast_format_value(s->miso_byte, s->wordsize, s->format, miso_str, sizeof(miso_str));
        c_put(di, ss, es, s->out_ann, ANN_MISO_DATA, miso_str);
    }
    if (s->have_mosi) {
        char mosi_str[128];
        spi_fast_format_value(s->mosi_byte, s->wordsize, s->format, mosi_str, sizeof(mosi_str));
        c_put(di, ss, es, s->out_ann, ANN_MOSI_DATA, mosi_str);
    }
}

static void spi_fast_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(spi_fast_state)));
    spi_fast_state *s = (spi_fast_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(spi_fast_state));
    s->cs_polarity = 0;
    s->cs_active = 0;
    s->wordsize = 8;
    s->transfer_start = (uint64_t)-1;
    s->out_ann = -1;
    s->out_python = -1;
    s->out_binary = -1;
    s->out_bitrate = -1;
}

static void spi_fast_start(struct srd_decoder_inst *di)
{
    spi_fast_state *s = (spi_fast_state *)c_decoder_get_private(di);
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
    if (s->wordsize < 1 || s->wordsize > 64)
        s->wordsize = 8;
    s->bw = (s->wordsize + 7) / 8;

    const char *show_dp_str = c_opt_str(di, "show_data_point", "no");
    s->show_data_point = (strcmp(show_dp_str, "yes") == 0) ? 1 : 0;

    const char *format_str = c_opt_str(di, "format", "hex");
    if (strcmp(format_str, "ascii") == 0)
        s->format = 0;
    else if (strcmp(format_str, "dec") == 0)
        s->format = 1;
    else if (strcmp(format_str, "hex") == 0)
        s->format = 2;
    else if (strcmp(format_str, "oct") == 0)
        s->format = 3;
    else if (strcmp(format_str, "bin") == 0)
        s->format = 4;
    else
        s->format = 2;

    s->have_miso = c_has_ch(di, 1);
    s->have_mosi = c_has_ch(di, 2);
    s->have_cs = c_has_ch(di, 3);

    int mode;
    if (s->cpol == 0 && s->cpha == 0)
        mode = 0;
    else if (s->cpol == 0 && s->cpha == 1)
        mode = 1;
    else if (s->cpol == 1 && s->cpha == 0)
        mode = 2;
    else
        mode = 3;
    s->sample_edge_rise = (mode == 0 || mode == 3) ? 1 : 0;
}

static void spi_fast_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    spi_fast_state *s = (spi_fast_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void spi_fast_decode(struct srd_decoder_inst *di)
{
    spi_fast_state *s = (spi_fast_state *)c_decoder_get_private(di);
    int CLK = 0;
    int MISO = 1;
    int MOSI = 2;
    int CS = 3;

    c_put(di, 0, 0, s->out_ann, ANN_ATK_DATA_POINT, "color:#F32FDC");
    c_put(di, 0, 0, s->out_ann, ANN_ATK_RISING_EDGE, "color:#F32FDC");
    c_put(di, 0, 0, s->out_ann, ANN_ATK_FALLING_EDGE, "color:#F32FDC");

    if (!s->have_cs) {
        c_proto(di, 0, 0, s->out_python, "CS-CHANGE", C_END);
    }

    /* Get initial pin states */
    uint64_t cur_sample = 0;
    if (c_wait(di, CW_END) != SRD_OK)
        return;

    if (s->have_cs) {
        int cs = c_pin(di, CS);
        s->cs_active = spi_fast_cs_asserted(s, cs);

        unsigned char cs_data[2];
        cs_data[0] = 0xFF;
        cs_data[1] = (unsigned char)cs;
        c_proto(di, cur_sample, cur_sample, s->out_python, "CS-CHANGE", C_U8(cs_data[0]), C_U8(cs_data[1]), C_END);

        if (s->cs_active) {
            s->transfer_start = cur_sample;
            s->misobytes_cnt = 0;
            s->mosibytes_cnt = 0;
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
        int miso = s->have_miso ? c_pin(di, MISO) : 0;
        int mosi = s->have_mosi ? c_pin(di, MOSI) : 0;
        int cs = s->have_cs ? c_pin(di, CS) : 1;

        int clk_matched = (di_matched(di) & (1ULL << 0));
        int cs_matched = s->have_cs && (di_matched(di) & (1ULL << 1));

        s->cs_active = s->have_cs ? spi_fast_cs_asserted(s, cs) : 1;

        if (cs_matched) {
            unsigned char cs_data[2];
            cs_data[0] = (unsigned char)(1 - cs);
            cs_data[1] = (unsigned char)cs;
            c_proto(di, di_samplenum(di), di_samplenum(di), s->out_python, "CS-CHANGE", C_U8(cs_data[0]), C_U8(cs_data[1]), C_END);

            if (s->cs_active) {
                s->transfer_start = di_samplenum(di);
                s->misobytes_cnt = 0;
                s->mosibytes_cnt = 0;
            } else if (s->transfer_start != (uint64_t)-1) {
                if (s->have_miso) {
                    char transfer_str[4096];
                    spi_fast_format_transfer(s->misobytes_val, s->misobytes_cnt,
                        s->format, s->wordsize, transfer_str, sizeof(transfer_str));
                    c_put(di, s->transfer_start, di_samplenum(di), s->out_ann, ANN_MISO_DATA, transfer_str);
                }
                if (s->have_mosi) {
                    char transfer_str[4096];
                    spi_fast_format_transfer(s->mosibytes_val, s->mosibytes_cnt,
                        s->format, s->wordsize, transfer_str, sizeof(transfer_str));
                    c_put(di, s->transfer_start, di_samplenum(di), s->out_ann, ANN_MOSI_DATA, transfer_str);
                }
                c_proto(di, s->transfer_start, di_samplenum(di), s->out_python, "TRANSFER", C_END);
            }

            spi_fast_reset_word(s);
        }

        if (s->have_cs && !s->cs_active)
            continue;

        if (!clk_matched)
            continue;

        int mode;
        if (s->cpol == 0 && s->cpha == 0)
            mode = 0;
        else if (s->cpol == 0 && s->cpha == 1)
            mode = 1;
        else if (s->cpol == 1 && s->cpha == 0)
            mode = 2;
        else
            mode = 3;

        int correct_edge = 0;
        if ((mode == 0 && clk == 1) || (mode == 3 && clk == 1))
            correct_edge = 1;
        else if ((mode == 1 && clk == 0) || (mode == 2 && clk == 0))
            correct_edge = 2;

        if (!correct_edge)
            continue;

        if (s->show_data_point) {
            char dp_str[8];
            if (correct_edge == 1) {
                snprintf(dp_str, sizeof(dp_str), "%d", CLK);
                c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_ATK_RISING_EDGE, dp_str);
            } else {
                snprintf(dp_str, sizeof(dp_str), "%d", CLK);
                c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_ATK_FALLING_EDGE, dp_str);
            }
            snprintf(dp_str, sizeof(dp_str), "%d", MISO);
            c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_ATK_DATA_POINT, dp_str);
            snprintf(dp_str, sizeof(dp_str), "%d", MOSI);
            c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_ATK_DATA_POINT, dp_str);
        }

        if (s->bit_count == 0) {
            s->start_sample = di_samplenum(di);
            s->cs_was_deasserted = s->have_cs ? !spi_fast_cs_asserted(s, cs) : 0;
        }

        if (s->bit_count > 0) {
            if (s->have_miso && s->bit_count <= s->wordsize)
                s->miso_bits_es[s->bit_count - 1] = di_samplenum(di);
            if (s->have_mosi && s->bit_count <= s->wordsize)
                s->mosi_bits_es[s->bit_count - 1] = di_samplenum(di);
        }
        s->last_bit_sample = di_samplenum(di);

        if (s->have_mosi) {
            if (s->bit_order == 0)
                s->mosi_byte = (s->mosi_byte << 1) | mosi;
            else
                s->mosi_byte |= ((uint64_t)mosi << s->bit_count);

            if (s->bit_count < s->wordsize) {
                s->mosi_bits_ss[s->bit_count] = di_samplenum(di);
                s->mosi_bits_es[s->bit_count] = di_samplenum(di);
                s->mosi_bits_val[s->bit_count] = mosi;
            }
        }

        if (s->have_miso) {
            if (s->bit_order == 0)
                s->miso_byte = (s->miso_byte << 1) | miso;
            else
                s->miso_byte |= ((uint64_t)miso << s->bit_count);

            if (s->bit_count < s->wordsize) {
                s->miso_bits_ss[s->bit_count] = di_samplenum(di);
                s->miso_bits_es[s->bit_count] = di_samplenum(di);
                s->miso_bits_val[s->bit_count] = miso;
            }
        }

        s->bit_count++;

        if (s->bit_count != s->wordsize)
            continue;

        spi_fast_put_data(di, s);

        if (s->samplerate > 0) {
            double elapsed = 1.0 / (double)s->samplerate;
            elapsed *= (double)(di_samplenum(di) - s->start_sample + 1);
            int bitrate = (int)(1.0 / elapsed * s->wordsize);
            c_put_meta_int(di, s->start_sample, di_samplenum(di), s->out_bitrate, bitrate);
        }

        spi_fast_reset_word(s);
    }
}

static void spi_fast_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder spi_fast_c_decoder = {
    .id = "spi_fast_c",
    .name = "SPI-Fast(C)",
    .longname = "Serial Peripheral Interface (C)",
    .desc = "SPI protocol decoder ultra-fast version (C implementation)",
    .license = "gplv2+",
    .channels = spi_fast_channels,
    .num_channels = 1,
    .optional_channels = spi_fast_optional_channels,
    .num_optional_channels = 3,
    .options = spi_fast_options,
    .num_options = 7,
    .num_annotations = NUM_ANN,
    .ann_labels = spi_fast_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = spi_fast_ann_rows,
    .inputs = spi_fast_inputs,
    .num_inputs = 1,
    .outputs = spi_fast_outputs,
    .num_outputs = 1,
    .binary = spi_fast_binary,
    .num_binary = 2,
    .tags = spi_fast_tags,
    .num_tags = 1,
    .reset = spi_fast_reset,
    .start = spi_fast_start,
    .decode = spi_fast_decode,
    .destroy = spi_fast_destroy,
    .state_size = 0,
    .metadata = spi_fast_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    /* cs_polarity */
    spi_fast_options[0].idn = "dec_spi_fast_opt_cs_polarity";
    spi_fast_options[0].def = g_variant_new_string("active-low");
    GSList *cs_vals = NULL;
    cs_vals = g_slist_append(cs_vals, g_variant_new_string("active-low"));
    cs_vals = g_slist_append(cs_vals, g_variant_new_string("active-high"));
    spi_fast_options[0].values = cs_vals;

    /* cpol */
    spi_fast_options[1].idn = "dec_spi_fast_opt_cpol";
    spi_fast_options[1].def = g_variant_new_uint64(0);
    GSList *cpol_vals = NULL;
    cpol_vals = g_slist_append(cpol_vals, g_variant_new_uint64(0));
    cpol_vals = g_slist_append(cpol_vals, g_variant_new_uint64(1));
    spi_fast_options[1].values = cpol_vals;

    /* cpha */
    spi_fast_options[2].idn = "dec_spi_fast_opt_cpha";
    spi_fast_options[2].def = g_variant_new_uint64(0);
    GSList *cpha_vals = NULL;
    cpha_vals = g_slist_append(cpha_vals, g_variant_new_uint64(0));
    cpha_vals = g_slist_append(cpha_vals, g_variant_new_uint64(1));
    spi_fast_options[2].values = cpha_vals;

    /* bitorder */
    spi_fast_options[3].idn = "dec_spi_fast_opt_bitorder";
    spi_fast_options[3].def = g_variant_new_string("msb-first");
    GSList *bitorder_vals = NULL;
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("msb-first"));
    bitorder_vals = g_slist_append(bitorder_vals, g_variant_new_string("lsb-first"));
    spi_fast_options[3].values = bitorder_vals;

    /* wordsize */
    spi_fast_options[4].idn = "dec_spi_fast_opt_wordsize";
    spi_fast_options[4].def = g_variant_new_uint64(8);

    /* format */
    spi_fast_options[5].idn = "dec_spi_fast_opt_format";
    spi_fast_options[5].def = g_variant_new_string("hex");
    GSList *format_vals = NULL;
    format_vals = g_slist_append(format_vals, g_variant_new_string("ascii"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("dec"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("hex"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("oct"));
    format_vals = g_slist_append(format_vals, g_variant_new_string("bin"));
    spi_fast_options[5].values = format_vals;

    /* show_data_point */
    spi_fast_options[6].idn = "dec_spi_fast_opt_show_data_point";
    spi_fast_options[6].def = g_variant_new_string("no");
    GSList *sdp_vals = NULL;
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("yes"));
    sdp_vals = g_slist_append(sdp_vals, g_variant_new_string("no"));
    spi_fast_options[6].values = sdp_vals;

    return &spi_fast_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}