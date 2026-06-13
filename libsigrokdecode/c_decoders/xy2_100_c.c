#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XY2100_MAX_BITS 20
#define XY2100_MAX_STAT_BITS 19

enum xy2100_ann {
    ANN_BIT = 0,
    ANN_STAT_BIT,
    ANN_TYPE,
    ANN_COMMAND,
    ANN_PARAMETER,
    ANN_PARITY,
    ANN_POS,
    ANN_STATUS,
    ANN_WARNING,
    NUM_ANN,
};

enum xy2100_frame_type {
    FRAME_TYPE_NONE = 0,
    FRAME_TYPE_COMMAND = 1,
    FRAME_TYPE_16BIT_POS = 2,
    FRAME_TYPE_18BIT_POS = 3,
};

typedef struct {
    uint64_t samplerate;
    int out_ann;

    /* Data bits acquisition */
    int bits_count;
    uint64_t bits_ss[XY2100_MAX_BITS];
    uint64_t bits_es[XY2100_MAX_BITS];
    int bits_value[XY2100_MAX_BITS];

    /* Status bits acquisition */
    int stat_bits_count;
    uint64_t stat_bits_ss[XY2100_MAX_STAT_BITS];
    uint64_t stat_bits_es[XY2100_MAX_STAT_BITS];
    int stat_bits_value[XY2100_MAX_STAT_BITS];
    int stat_skip_bit;

    /* Current bit tracking */
    uint64_t bit_ss;
    int bit_value;
    uint64_t stat_ss;
    int stat_value;
    int sync_value;

    /* Channel presence flag */
    int has_stat;
} xy2100_state;

static struct srd_channel xy2100_channels[] = {
    {"clk", "CLK", "Clock", 0, SRD_CHANNEL_SCLK, NULL},
    {"sync", "SYNC", "Sync", 1, SRD_CHANNEL_COMMON, NULL},
    {"data", "DATA", "X, Y or Z axis data", 2, SRD_CHANNEL_SDATA, NULL},
};

static struct srd_channel xy2100_optional_channels[] = {
    {"status", "STAT", "X, Y or Z axis status", 3, SRD_CHANNEL_SDATA, NULL},
};

static const char *xy2100_ann_labels[][3] = {
    {"", "Data Bit", "Data Bit"},
    {"", "Status Bit", "Status Bit"},
    {"", "Frame Type", "Frame Type"},
    {"", "Command", "Command"},
    {"", "Parameter", "Parameter"},
    {"", "Parity", "Parity"},
    {"", "Position", "Position"},
    {"", "Status", "Status"},
    {"", "Warning", "Human-readable warnings"},
};

static const int xy2100_row_bits_classes[] = {ANN_BIT, -1};
static const int xy2100_row_stat_bits_classes[] = {ANN_STAT_BIT, -1};
static const int xy2100_row_data_classes[] = {ANN_TYPE, ANN_COMMAND, ANN_PARAMETER, ANN_PARITY, -1};
static const int xy2100_row_positions_classes[] = {ANN_POS, -1};
static const int xy2100_row_statuses_classes[] = {ANN_STATUS, -1};
static const int xy2100_row_warnings_classes[] = {ANN_WARNING, -1};

static const struct srd_c_ann_row xy2100_ann_rows[] = {
    {"bits", "Data Bits", xy2100_row_bits_classes, 1},
    {"stat_bits", "Status Bits", xy2100_row_stat_bits_classes, 1},
    {"data", "Data", xy2100_row_data_classes, 4},
    {"positions", "Positions", xy2100_row_positions_classes, 1},
    {"statuses", "Statuses", xy2100_row_statuses_classes, 1},
    {"warnings", "Warnings", xy2100_row_warnings_classes, 1},
};

static const char *xy2100_inputs[] = {"logic"};
static const char *xy2100_tags[] = {"Embedded/industrial"};

static void xy2100_reset_state(xy2100_state *s)
{
    s->bits_count = 0;
    s->stat_bits_count = 0;
    s->stat_skip_bit = 1;
}

static void xy2100_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(xy2100_state)));
    xy2100_state *s = (xy2100_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(xy2100_state));
    s->out_ann = -1;
    s->bit_ss = (uint64_t)-1;
    s->stat_ss = (uint64_t)-1;
    s->stat_skip_bit = 1;
}

static void xy2100_start(struct srd_decoder_inst *di)
{
    xy2100_state *s = (xy2100_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "xy2-100");
    s->has_stat = c_has_ch(di, 3);
}

static void xy2100_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    xy2100_state *s = (xy2100_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
    }
}

static void xy2100_process_stat_bit(struct srd_decoder_inst *di, xy2100_state *s,
                                     int sync, uint64_t bit_ss, uint64_t bit_es, int bit_value)
{
    if (s->stat_skip_bit) {
        s->stat_skip_bit = 0;
        return;
    }

    char bstr[4];
    snprintf(bstr, sizeof(bstr), "%d", bit_value);
    c_put(di, bit_ss, bit_es, s->out_ann, ANN_STAT_BIT, bstr);

    if (s->stat_bits_count < XY2100_MAX_STAT_BITS) {
        s->stat_bits_ss[s->stat_bits_count] = bit_ss;
        s->stat_bits_es[s->stat_bits_count] = bit_es;
        s->stat_bits_value[s->stat_bits_count] = bit_value;
    }
    s->stat_bits_count++;

    if (sync == 0 && s->stat_bits_count == 19) {
        uint64_t stat_ss = s->stat_bits_ss[0];
        uint64_t stat_es = s->stat_bits_es[18];

        int status = 0;
        int count = 18;
        for (int i = 0; i < 19; i++) {
            status |= s->stat_bits_value[i] << count;
            count--;
        }
        char stat_str[32];
        snprintf(stat_str, sizeof(stat_str), "Status 0x%X", status);
        char stat_short[16];
        snprintf(stat_short, sizeof(stat_short), "0x%X", status);
        c_put(di, stat_ss, stat_es, s->out_ann, ANN_STATUS, stat_str, stat_short);
    }
}

static void xy2100_process_bit(struct srd_decoder_inst *di, xy2100_state *s,
                                int sync, uint64_t bit_ss, uint64_t bit_es, int bit_value)
{
    char bstr[4];
    snprintf(bstr, sizeof(bstr), "%d", bit_value);
    c_put(di, bit_ss, bit_es, s->out_ann, ANN_BIT, bstr);

    if (s->bits_count < XY2100_MAX_BITS) {
        s->bits_ss[s->bits_count] = bit_ss;
        s->bits_es[s->bits_count] = bit_es;
        s->bits_value[s->bits_count] = bit_value;
    }
    s->bits_count++;

    if (sync == 0) {
        if (s->bits_count < 20) {
            c_put(di, s->bits_ss[0], bit_es, s->out_ann, ANN_WARNING,
                      "Not enough data bits");
            xy2100_reset_state(s);
            return;
        }

        /* Calculate parity (XOR of bits 0-18, not including parity bit) */
        int parity = 0;
        for (int i = 0; i < 19; i++)
            parity ^= s->bits_value[i];

        int par_value = s->bits_value[19];
        int parity_even = (par_value == parity) ? 1 : 0;
        int parity_odd = (par_value != parity) ? 1 : 0;

        int type_1_value = s->bits_value[0];
        int type_3_value = (s->bits_value[0] << 2) | (s->bits_value[1] << 1) | s->bits_value[2];

        enum xy2100_frame_type frame_type = FRAME_TYPE_NONE;
        const char *parity_status = "X";
        uint64_t type_ss = s->bits_ss[0];
        uint64_t type_es = s->bits_es[2];

        /* 18-bit position frame */
        if (type_1_value == 1 && parity_odd == 1) {
            frame_type = FRAME_TYPE_18BIT_POS;
            type_es = s->bits_es[0];
            c_put(di, s->bits_ss[0], bit_es, s->out_ann, ANN_WARNING,
                      "Careful: 18-bit position frames with wrong parity and command frames with wrong parity cannot be identified");
        }
        /* 16-bit position frame */
        else if (type_3_value == 1) {
            frame_type = FRAME_TYPE_16BIT_POS;
            if (parity_even == 1)
                parity_status = "OK";
            else {
                parity_status = "NOK";
                c_put(di, s->bits_ss[0], bit_es, s->out_ann, ANN_WARNING,
                          "Parity error", "PE");
            }
        }
        /* Command frame */
        else if (type_3_value == 7 && parity_even == 1) {
            frame_type = FRAME_TYPE_COMMAND;
            c_put(di, s->bits_ss[0], bit_es, s->out_ann, ANN_WARNING,
                      "Careful: 18-bit position frames with wrong parity and command frames with wrong parity cannot be identified");
        }
        /* Unknown */
        else {
            c_put(di, s->bits_ss[0], bit_es, s->out_ann, ANN_WARNING,
                      "Error", "Unknown command or parity error");
            xy2100_reset_state(s);
            return;
        }

        /* Output frame type */
        if (frame_type == FRAME_TYPE_16BIT_POS) {
            c_put(di, type_ss, type_es, s->out_ann, ANN_TYPE,
                      "16 bit Position Frame", "16 bit Pos", "Pos", "P");
        } else if (frame_type == FRAME_TYPE_18BIT_POS) {
            c_put(di, type_ss, type_es, s->out_ann, ANN_TYPE,
                      "18 bit Position Frame", "18 bit Pos", "Pos", "P");
        } else if (frame_type == FRAME_TYPE_COMMAND) {
            c_put(di, type_ss, type_es, s->out_ann, ANN_TYPE,
                      "Command Frame", "Command", "C");
        }

        /* Output parity */
        uint64_t par_ss = s->bits_ss[19];
        uint64_t par_es = s->bits_es[19];
        c_put(di, par_ss, par_es, s->out_ann, ANN_PARITY, parity_status);

        /* Output position value */
        if (frame_type == FRAME_TYPE_16BIT_POS || frame_type == FRAME_TYPE_18BIT_POS) {
            int64_t pos = 0;
            if (frame_type == FRAME_TYPE_16BIT_POS) {
                int count = 15;
                for (int i = 3; i < 19; i++) {
                    pos |= (int64_t)s->bits_value[i] << count;
                    count--;
                }
                if (pos >= 32768) pos -= 65536;
            } else {
                int count = 17;
                for (int i = 3; i < 19; i++) {
                    pos |= (int64_t)s->bits_value[i] << count;
                    count--;
                }
                if (pos >= 131072) pos -= 262144;
            }
            char pos_str[32];
            snprintf(pos_str, sizeof(pos_str), "%lld", (long long)pos);
            c_put(di, type_es, par_ss, s->out_ann, ANN_POS, pos_str);
        }

        /* Output command and parameter */
        if (frame_type == FRAME_TYPE_COMMAND) {
            int cmd = 0;
            int count = 7;
            uint64_t cmd_es = 0;
            for (int i = 3; i < 11; i++) {
                cmd |= s->bits_value[i] << count;
                count--;
                cmd_es = s->bits_es[i];
            }
            char cmd_str[32];
            snprintf(cmd_str, sizeof(cmd_str), "Command 0x%X", cmd);
            char cmd_short[16];
            snprintf(cmd_short, sizeof(cmd_short), "0x%X", cmd);
            c_put(di, type_es, cmd_es, s->out_ann, ANN_COMMAND, cmd_str, cmd_short);

            int param = 0;
            count = 7;
            for (int i = 11; i < 19; i++) {
                param |= s->bits_value[i] << count;
                count--;
            }
            char param_str[64];
            snprintf(param_str, sizeof(param_str), "Parameter 0x%X / %d", param, param);
            char param_short[32];
            snprintf(param_short, sizeof(param_short), "0x%X / %d", param, param);
            char param_tiny[16];
            snprintf(param_tiny, sizeof(param_tiny), "0x%X", param);
            c_put(di, cmd_es, par_ss, s->out_ann, ANN_PARAMETER, param_str, param_short, param_tiny);
        }

        xy2100_reset_state(s);
    }
}

static void xy2100_decode(struct srd_decoder_inst *di)
{
    xy2100_state *s = (xy2100_state *)c_decoder_get_private(di);
    if (!s->samplerate) {
        s->samplerate = c_samplerate(di);
    }

    uint64_t bit_ss = (uint64_t)-1;
    int bit_value = 0;
    uint64_t stat_ss = (uint64_t)-1;
    int stat_value = 0;
    int sync_value = 0;

    while (1) {
        int ret = c_wait(di, CW_E(0), CW_END);
        if (ret != SRD_OK)
            return;

        int clk = c_pin(di, 0);
        int sync = c_pin(di, 1);
        int data = c_pin(di, 2);
        int stat = s->has_stat ? c_pin(di, 3) : 0;

        if (clk == 1) {
            /* Rising edge: end data bit, start new bit */
            stat_value = stat;
            uint64_t bit_es = di_samplenum(di);
            if (bit_ss != (uint64_t)-1) {
                xy2100_process_bit(di, s, sync_value, bit_ss, bit_es, bit_value);
            }
            bit_ss = di_samplenum(di);
        } else {
            /* Falling edge: sample DATA and SYNC */
            bit_value = data;
            sync_value = sync;

            uint64_t stat_es = di_samplenum(di);
            if (stat_ss != (uint64_t)-1 && s->has_stat) {
                xy2100_process_stat_bit(di, s, sync_value, stat_ss, stat_es, stat_value);
            }
            stat_ss = di_samplenum(di);
        }
    }
}

static void xy2100_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder xy2100_c_decoder = {
    .id = "xy2_100_c",
    .name = "XY2-100(C)",
    .longname = "XY2-100(E) and XY-200(E) galvanometer protocol (C)",
    .desc = "Serial protocol for galvanometer positioning in laser systems (C implementation)",
    .license = "gplv2+",
    .channels = xy2100_channels,
    .num_channels = 3,
    .optional_channels = xy2100_optional_channels,
    .num_optional_channels = 1,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = xy2100_ann_labels,
    .num_annotation_rows = 6,
    .annotation_rows = xy2100_ann_rows,
    .inputs = xy2100_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = xy2100_tags,
    .num_tags = 1,
    .reset = xy2100_reset,
    .start = xy2100_start,
    .decode = xy2100_decode,
    .destroy = xy2100_destroy,
    .state_size = 0,
    .metadata = xy2100_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &xy2100_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}