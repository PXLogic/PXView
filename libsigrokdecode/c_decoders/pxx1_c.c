#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include "libsigrokdecode.h"

enum pxx1_state {
    STATE_WAIT_HEADER = 0,
    STATE_RX_MODEL_ID,
    STATE_RX_TYPE,
    STATE_RX_RANGE_CHECK,
    STATE_RX_FAIL_SAFE,
    STATE_RX_COUNTRY_CODE,
    STATE_RX_BIND,
    STATE_RX_FLAG2,
    STATE_RX_CHANNELS,
    STATE_RX_RSRV2,
    STATE_RX_EUPLUS,
    STATE_RX_DISABLE_SPORT,
    STATE_RX_POWERLEVEL,
    STATE_RX_HIGHCHAN,
    STATE_RX_TELEMETRY_OFF,
    STATE_RX_EXTERNAL_ANTENA,
    STATE_RX_CRC,
    STATE_RX_STOP,
    STATE_ERROR,
};

enum pxx1_ann {
    ANN_BYTE = 0,
    ANN_BIT,
    ANN_BIT_STUFF,
    ANN_START_HEADER,
    ANN_MODEL_ID,
    ANN_TYPE,
    ANN_RANGE_CHECK,
    ANN_FAIL_SAFE,
    ANN_COUNTRY_CODE,
    ANN_BIND,
    ANN_FLAGS2,
    ANN_CHANNELS,
    ANN_RESERVED,
    ANN_IS_EUPLUS,
    ANN_DISABLE_SPORT,
    ANN_POWER_LEVEL,
    ANN_RX_HIGHCHAN,
    ANN_TELEMETRY_OFF,
    ANN_EXTERNAL_ANTENA,
    ANN_CRC,
    NUM_ANN,
};

static const char *transmit_type[] = {"FCC", "EU", "EU+", "AU+"};

#define PXX_HEADER      0x7E
#define PXX_SEND_BIND   0x01
#define PXX_SEND_FAILSAFE  (1 << 4)
#define PXX_SEND_RANGECHECK (1 << 5)

struct pxx1_priv {
    uint64_t samplerate;
    int out_ann;
    int out_binary;

    uint8_t byte_val;
    int byte_cnt;
    int cur_bit;
    int bit_one_cnt;
    uint64_t byte_start;
    int bit_stuffing;

    uint32_t state_word;
    uint64_t state_word_start;
    int state_bit;
    int state;

    uint8_t nibbles[24];
    int nibble_cnt;
    int nibble_val;
    int nibble_bit_cnt;

    uint64_t start_samplenum;
    uint64_t ss_block;
    uint64_t es_block;
};

static struct srd_channel pxx1_channels[] = {
    {"data", "Data", "Data line", 0, SRD_CHANNEL_SDATA, "dec_pxx1_chan_data"},
};

static const char *pxx1_ann_labels[][3] = {
    {"", "byte", "Byte"},
    {"", "bit", "Bit"},
    {"", "bit_stuff", "BitStuff"},
    {"", "start_header", "Start Header"},
    {"", "model_id", "Model ID"},
    {"", "type", "Type"},
    {"", "range_check", "RangeCheck"},
    {"", "fail_safe", "FailSafe"},
    {"", "country_code", "CountryCode"},
    {"", "bind", "Bind"},
    {"", "flags2", "Flags2"},
    {"", "channels", "Channels"},
    {"", "reserved", "Reserved"},
    {"", "is_euplus", "EU-PLUS"},
    {"", "disable_sport", "Disable SPort"},
    {"", "power_level", "Power Level"},
    {"", "rx_highchan", "Receive Hight Channel"},
    {"", "telemetry_off", "Telemetry Off"},
    {"", "external_antena", "ExternalAntena"},
    {"", "CRC", "CRC"},
};

static const int pxx1_row_bytes_classes[] = {ANN_BYTE, -1};
static const int pxx1_row_bits_classes[] = {ANN_BIT, ANN_BIT_STUFF, -1};
static const int pxx1_row_desc_classes[] = {
    ANN_START_HEADER, ANN_MODEL_ID, ANN_TYPE, ANN_RANGE_CHECK,
    ANN_FAIL_SAFE, ANN_COUNTRY_CODE, ANN_BIND, ANN_FLAGS2,
    ANN_CHANNELS, ANN_RESERVED, ANN_IS_EUPLUS, ANN_DISABLE_SPORT,
    ANN_POWER_LEVEL, ANN_RX_HIGHCHAN, ANN_TELEMETRY_OFF,
    ANN_EXTERNAL_ANTENA, ANN_CRC, -1
};

static const struct srd_c_ann_row pxx1_ann_rows[] = {
    {"bytes", "Bytes", pxx1_row_bytes_classes, 1},
    {"bits", "Bits", pxx1_row_bits_classes, 2},
    {"desc", "Description", pxx1_row_desc_classes, 17},
};

static struct srd_decoder_binary pxx1_binary[] = {
    {0, "raw", "RAW file"},
};

static const char *pxx1_inputs[] = {"logic"};
static const char *pxx1_outputs[] = {};
static const char *pxx1_tags[] = {"PXX1"};

static void pxx1_reset_state(struct srd_decoder_inst *di)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);
    s->state = STATE_WAIT_HEADER;
    s->byte_val = 0;
    s->byte_cnt = 0;
    s->cur_bit = 0;
    s->bit_one_cnt = 0;
    s->byte_start = 0;
    s->bit_stuffing = 1;
    s->state_word = 0;
    s->state_word_start = 0;
    s->state_bit = 0;
    s->nibble_cnt = 0;
    s->nibble_val = 0;
    s->nibble_bit_cnt = 0;
}

static void pxx1_break_rx(struct srd_decoder_inst *di)
{
    pxx1_reset_state(di);
}

static void pxx1_add_byte(struct srd_decoder_inst *di, uint8_t byte_val)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);

    char h[8];
    snprintf(h, sizeof(h), "%02X", byte_val);
    c_put(di, s->byte_start, s->es_block, s->out_ann, ANN_BYTE, h);

    if (s->out_binary >= 0)
        c_put_bin(di, s->byte_start, s->es_block, s->out_binary, 0, 1, &byte_val);

    s->byte_cnt++;
    if (s->byte_cnt > 18)
        s->bit_stuffing = 0;
}

static void pxx1_process_state(struct srd_decoder_inst *di, int bstuff);

static void pxx1_add_bit(struct srd_decoder_inst *di, int value)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);
    int bstuff = 0;

    if (s->cur_bit == 0)
        s->byte_start = s->ss_block;

    if (s->state_bit == 0)
        s->state_word_start = s->ss_block;

    s->bit_one_cnt += 1;

    if (!s->bit_stuffing || s->bit_one_cnt < 6) {
        s->byte_val <<= 1;
        s->byte_val |= value;
        s->cur_bit += 1;
        char bit_str[16];
        snprintf(bit_str, sizeof(bit_str), "%X", value);
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_BIT, bit_str);
        s->state_word <<= 1;
        s->state_word |= value;
        s->state_bit += 1;
    } else {
        bstuff = 1;
        c_put(di, s->ss_block, s->es_block, s->out_ann, ANN_BIT_STUFF, "S");
    }

    if (value == 0)
        s->bit_one_cnt = 0;

    if (s->cur_bit == 8) {
        pxx1_add_byte(di, s->byte_val);
        s->cur_bit = 0;
        s->byte_val = 0;
    }

    pxx1_process_state(di, bstuff);
}

static void pxx1_process_state(struct srd_decoder_inst *di, int bstuff)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);

    switch (s->state) {
    case STATE_WAIT_HEADER:
        if (s->state_bit == 8) {
            if (s->state_word == PXX_HEADER) {
                c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_START_HEADER, "Start Header", "SH");
            } else {
                c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_START_HEADER, "Header error");
                pxx1_break_rx(di);
                return;
            }
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_MODEL_ID;
        }
        break;

    case STATE_RX_MODEL_ID:
        if (s->state_bit == 8) {
            char text[32];
            snprintf(text, sizeof(text), "Model ID: %u", s->state_word);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_MODEL_ID, text);
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_TYPE;
        }
        break;

    case STATE_RX_TYPE:
        if (s->state_bit == 2) {
            int idx = (s->state_word > 3) ? 3 : (int)s->state_word;
            char text[32];
            snprintf(text, sizeof(text), "Type: %s", transmit_type[idx]);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_TYPE, text);
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_RANGE_CHECK;
        }
        break;

    case STATE_RX_RANGE_CHECK:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_RANGE_CHECK,
                      s->state_word ? "Range Check: On" : "Range Check: Off",
                      s->state_word ? "RC: On" : "RC: Off");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_FAIL_SAFE;
        }
        break;

    case STATE_RX_FAIL_SAFE:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_FAIL_SAFE,
                      s->state_word ? "FailSafe: On" : "FailSafe: Off",
                      s->state_word ? "FS: On" : "FS: Off");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_COUNTRY_CODE;
        }
        break;

    case STATE_RX_COUNTRY_CODE:
        if (s->state_bit == 3) {
            char text[32];
            snprintf(text, sizeof(text), "CountryCode: %u", s->state_word);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_COUNTRY_CODE, text);
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_BIND;
        }
        break;

    case STATE_RX_BIND:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_BIND,
                      s->state_word ? "Bind: On" : "Bind: Off",
                      s->state_word ? "B: On" : "B: Off");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_FLAG2;
        }
        break;

    case STATE_RX_FLAG2:
        if (s->state_bit == 8) {
            char text[32];
            snprintf(text, sizeof(text), "Flag2: %u", s->state_word);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_FLAGS2, text);
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_CHANNELS;
        }
        break;

    case STATE_RX_CHANNELS: {
        /* Accumulate nibbles (4 bits each, excluding stuffing bits) */
        if (bstuff == 0 && s->state_bit > 0 && s->state_bit % 4 == 0) {
            if (s->nibble_cnt < 24) {
                s->nibbles[s->nibble_cnt] = s->nibble_val;
                s->nibble_cnt++;
            }
            s->nibble_val = 0;
        }
        if (bstuff == 0) {
            s->nibble_val <<= 1;
            s->nibble_val |= (s->state_word & 1);
        }

        if (s->state_bit == 96) {
            char out_buf[256] = "";
            int is_upper = 0;
            int idx = 0;
            while (idx + 5 < s->nibble_cnt) {
                int ch1 = (s->nibbles[idx+3] << 8) | (s->nibbles[idx+1] << 4) | s->nibbles[idx];
                int ch2 = (s->nibbles[idx+4] << 8) | (s->nibbles[idx+5] << 4) | s->nibbles[idx+2];
                char pair[32];
                snprintf(pair, sizeof(pair), "%s%04u %04u", (idx > 0 ? " " : ""), ch1, ch2);
                strcat(out_buf, pair);
                if (ch1 > 2048) is_upper = 1;
                idx += 6;
            }
            char ann_text[300];
            snprintf(ann_text, sizeof(ann_text), "Channels %s: [%s]",
                     is_upper ? "(9-16)" : "(1-8)", out_buf);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_CHANNELS, ann_text);
            s->state_bit = 0;
            s->state_word = 0;
            s->nibble_cnt = 0;
            s->nibble_val = 0;
            s->state = STATE_RX_RSRV2;
        }
        break;
    }

    case STATE_RX_RSRV2:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_RESERVED, "Reserved");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_EUPLUS;
        }
        break;

    case STATE_RX_EUPLUS:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_IS_EUPLUS,
                      s->state_word ? "EUPlus: Yes" : "EUPlus: No",
                      s->state_word ? "EU+: Y" : "EU+: N");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_DISABLE_SPORT;
        }
        break;

    case STATE_RX_DISABLE_SPORT:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_DISABLE_SPORT,
                      s->state_word ? "SPort: Disabled" : "SPort: Enable",
                      s->state_word ? "SP: Dis" : "SP: En");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_POWERLEVEL;
        }
        break;

    case STATE_RX_POWERLEVEL:
        if (s->state_bit == 2) {
            char text[32];
            snprintf(text, sizeof(text), "PowerLevel: %u", s->state_word);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_POWER_LEVEL, text);
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_HIGHCHAN;
        }
        break;

    case STATE_RX_HIGHCHAN:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_RX_HIGHCHAN,
                      s->state_word ? "RX HighChannel: Yes" : "RX HighChannel: No",
                      s->state_word ? "HC: Y" : "HC: N");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_TELEMETRY_OFF;
        }
        break;

    case STATE_RX_TELEMETRY_OFF:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_TELEMETRY_OFF,
                      s->state_word ? "Telemetry: Off" : "Telemetry: On",
                      s->state_word ? "Tel: Off" : "Tel: On");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_EXTERNAL_ANTENA;
        }
        break;

    case STATE_RX_EXTERNAL_ANTENA:
        if (s->state_bit == 1) {
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_EXTERNAL_ANTENA,
                      s->state_word ? "ExternalAntena: Yes" : "ExternalAntena: No",
                      s->state_word ? "EA: Y" : "EA: N");
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_CRC;
        }
        break;

    case STATE_RX_CRC:
        if (s->state_bit == 16) {
            char text[32];
            snprintf(text, sizeof(text), "CRC: 0x%04X", s->state_word);
            c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_CRC, text);
            s->state_bit = 0;
            s->state_word = 0;
            s->state = STATE_RX_STOP;
        }
        break;

    case STATE_RX_STOP:
        if (s->state_bit == 8) {
            if (s->state_word == PXX_HEADER) {
                c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_START_HEADER, "Stop Header", "SH");
            } else {
                c_put(di, s->state_word_start, s->es_block, s->out_ann, ANN_START_HEADER, "Header error");
            }
            s->state_bit = 0;
            s->state_word = 0;
            pxx1_break_rx(di);
        }
        break;

    case STATE_ERROR:
        pxx1_break_rx(di);
        break;
    }
}

static void pxx1_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di))
        c_decoder_set_private(di, g_malloc0(sizeof(struct pxx1_priv)));
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct pxx1_priv));
    s->out_ann = -1;
    s->out_binary = -1;
    s->bit_stuffing = 1;
    s->state = STATE_WAIT_HEADER;
}

static void pxx1_start(struct srd_decoder_inst *di)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "pxx1");
    s->out_binary = c_reg_out(di, SRD_OUTPUT_BINARY, "pxx1");
}

static void pxx1_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void pxx1_decode(struct srd_decoder_inst *di)
{
    struct pxx1_priv *s = (struct pxx1_priv *)c_decoder_get_private(di);
    int ret;

    if (s->samplerate == 0)
        return;

    /* Wait for first falling edge */
    {
        ret = c_wait(di, CW_F(0), CW_END);
        if (ret != SRD_OK)
            return;
    }

    while (1) {
        uint64_t start_samplenum = di_samplenum(di);

        /* Wait for rising edge */
        {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
        }
        

        /* Wait for falling edge */
        {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
        }

        s->ss_block = start_samplenum;
        s->es_block = di_samplenum(di);

        uint64_t period = di_samplenum(di) - start_samplenum;
        double period_t = (double)period / (double)s->samplerate;

        if (period_t >= 0.000023 && period_t <= 0.000025) {
            pxx1_add_bit(di, 1);
        } else if (period_t >= 0.000015 && period_t <= 0.000017) {
            pxx1_add_bit(di, 0);
        } else if (period_t >= 0.000040) {
            /* Fix es_block and break */
            s->es_block = s->ss_block + (uint64_t)((double)s->samplerate / 1000000.0 * 16.0);
            pxx1_add_bit(di, 0);
            pxx1_break_rx(di);
        }
    }
}

static void pxx1_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

static struct srd_c_decoder pxx1_c_decoder = {
    .id = "pxx1_c",
    .name = "PXX1(C)",
    .longname = "PXX1 modulation (C)",
    .desc = "FrSky PXX1(R9M) Protcol (C implementation)",
    .license = "gplv2+",
    .channels = pxx1_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = NULL,
    .num_options = 0,
    .num_annotations = NUM_ANN,
    .ann_labels = pxx1_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = pxx1_ann_rows,
    .inputs = pxx1_inputs,
    .num_inputs = 1,
    .outputs = pxx1_outputs,
    .num_outputs = 0,
    .binary = pxx1_binary,
    .num_binary = 1,
    .tags = pxx1_tags,
    .num_tags = 1,
    .reset = pxx1_reset,
    .start = pxx1_start,
    .decode = pxx1_decode,
    .destroy = pxx1_destroy,
    .state_size = 0,
    .metadata = pxx1_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder *srd_c_decoder_entry(void)
{
    return &pxx1_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}