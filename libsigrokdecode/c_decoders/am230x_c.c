#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANN_START = 0,
    ANN_RESPONSE,
    ANN_BIT,
    ANN_END,
    ANN_BYTE,
    ANN_HUMIDITY,
    ANN_TEMPERATURE,
    ANN_CHECKSUM,
    NUM_ANN,
};

enum am230x_state_enum {
    STATE_WAIT_START_LOW = 0,
    STATE_WAIT_START_HIGH,
    STATE_WAIT_RESPONSE_LOW,
    STATE_WAIT_RESPONSE_HIGH,
    STATE_WAIT_FIRST_BIT,
    STATE_WAIT_BIT_HIGH,
    STATE_WAIT_BIT_LOW,
    STATE_WAIT_END,
};

enum timing_index {
    TIMING_START_LOW = 0,
    TIMING_START_HIGH,
    TIMING_RESPONSE_LOW,
    TIMING_RESPONSE_HIGH,
    TIMING_BIT_LOW,
    TIMING_BIT_0_HIGH,
    TIMING_BIT_1_HIGH,
};

typedef struct {
    uint64_t min;
    uint64_t max;
} timing_range;

/* Timing constants in microseconds */
static const timing_range timing_us[7] = {
    { 750, 25000 },   /* START LOW */
    { 10,  10000 },   /* START HIGH */
    { 50,  90 },      /* RESPONSE LOW */
    { 50,  90 },      /* RESPONSE HIGH */
    { 45,  90 },      /* BIT LOW */
    { 20,  35 },      /* BIT 0 HIGH */
    { 65,  80 },      /* BIT 1 HIGH */
};

typedef struct {
    uint64_t samplerate;
    int out_ann;
    int device;         /* 0=am230x/rht, 1=dht11 */
    int state;
    uint64_t fall;
    uint64_t rise;
    uint8_t bits[40];
    int bit_count;
    uint64_t bytepos[5];
    int bytepos_count;
    timing_range cnt[7];
} am230x_state;

static struct srd_channel am230x_channels[] = {
    { "sda", "SDA", "Single wire serial data line", 0, SRD_CHANNEL_SDATA, "dec_am230x_chan_sda" },
};

static struct srd_decoder_option am230x_options[] = {
    { "device", NULL, "Device type", NULL, NULL },
};

static const char* am230x_ann_labels[][3] = {
    { "", "start", "Start" },
    { "", "response", "Response" },
    { "", "bit", "Bit" },
    { "", "end", "End" },
    { "", "byte", "Byte" },
    { "", "humidity", "Relative humidity in percent" },
    { "", "temperature", "Temperature in degrees Celsius" },
    { "", "checksum", "Checksum" },
};

static const int am230x_row_bits_classes[] = {ANN_START, ANN_RESPONSE, ANN_BIT, ANN_END};
static const int am230x_row_bytes_classes[] = {ANN_BYTE};
static const int am230x_row_results_classes[] = {ANN_HUMIDITY, ANN_TEMPERATURE, ANN_CHECKSUM};

static const struct srd_c_ann_row am230x_ann_rows[] = {
    { "bits", "Bits", am230x_row_bits_classes, 4 },
    { "bytes", "Bytes", am230x_row_bytes_classes, 1 },
    { "results", "Results", am230x_row_results_classes, 3 },
};

static const char* am230x_inputs[] = { "logic" };
static const char* am230x_tags[] = { "IC", "Sensor" };

static uint8_t bits2num(uint8_t *bits, int count)
{
    uint8_t number = 0;
    for (int i = 0; i < count; i++)
        number += bits[count - 1 - i] * (1 << i);
    return number;
}

static uint16_t bits2num16(uint8_t *bits, int count)
{
    uint16_t number = 0;
    for (int i = 0; i < count && i < 16; i++)
        number += bits[count - 1 - i] * (1 << i);
    return number;
}

static int is_valid(am230x_state *s, uint64_t samplenum, int timing_idx)
{
    uint64_t dt;
    if (timing_idx == TIMING_START_LOW || timing_idx == TIMING_RESPONSE_LOW ||
        timing_idx == TIMING_BIT_LOW) {
        dt = samplenum - s->fall;
    } else {
        dt = samplenum - s->rise;
    }
    return (dt >= s->cnt[timing_idx].min && dt <= s->cnt[timing_idx].max);
}

static void am230x_reset_state(am230x_state *s)
{
    s->state = STATE_WAIT_START_LOW;
    s->fall = 0;
    s->rise = 0;
    s->bit_count = 0;
    s->bytepos_count = 0;
}

static void am230x_calc_timing(am230x_state *s)
{
    if (!s->samplerate) return;
    for (int i = 0; i < 7; i++) {
        s->cnt[i].min = timing_us[i].min * s->samplerate / 1000000;
        s->cnt[i].max = timing_us[i].max * s->samplerate / 1000000;
    }
}

static void am230x_reset(struct srd_decoder_inst *di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(am230x_state)));
    }
    am230x_state *s = (am230x_state *)c_decoder_get_private(di);
    memset(s, 0, sizeof(am230x_state));
    s->out_ann = -1;
    s->state = STATE_WAIT_START_LOW;
}

static void am230x_start(struct srd_decoder_inst *di)
{
    am230x_state *s = (am230x_state *)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "am230x");

    const char *dev_str = c_opt_str(di, "device", "am230x");
    if (strcmp(dev_str, "dht11") == 0)
        s->device = 1;
    else
        s->device = 0;

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    am230x_calc_timing(s);
}

static void am230x_metadata(struct srd_decoder_inst *di, int key, uint64_t value)
{
    am230x_state *s = (am230x_state *)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE) {
        s->samplerate = value;
        am230x_calc_timing(s);
    }
}

static void am230x_decode(struct srd_decoder_inst *di)
{
    am230x_state *s = (am230x_state *)c_decoder_get_private(di);
    int ret;

    if (!s->samplerate)
        s->samplerate = c_samplerate(di);
    if (!s->samplerate)
        return;
    am230x_calc_timing(s);

    while (1) {
        switch (s->state) {
        case STATE_WAIT_START_LOW:
        {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->fall = di_samplenum(di);
            s->state = STATE_WAIT_START_HIGH;
            break;
        }

        case STATE_WAIT_START_HIGH:
        {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
            if (is_valid(s, di_samplenum(di), TIMING_START_LOW)) {
                s->rise = di_samplenum(di);
                s->state = STATE_WAIT_RESPONSE_LOW;
            } else {
                am230x_reset_state(s);
            }
            break;
        }

        case STATE_WAIT_RESPONSE_LOW:
        {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
            if (is_valid(s, di_samplenum(di), TIMING_START_HIGH)) {
                c_put(di, s->fall, di_samplenum(di), s->out_ann, ANN_START, "Start", "S");
                s->fall = di_samplenum(di);
                s->state = STATE_WAIT_RESPONSE_HIGH;
            } else {
                am230x_reset_state(s);
            }
            break;
        }

        case STATE_WAIT_RESPONSE_HIGH:
        {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
            if (is_valid(s, di_samplenum(di), TIMING_RESPONSE_LOW)) {
                s->rise = di_samplenum(di);
                s->state = STATE_WAIT_FIRST_BIT;
            } else {
                am230x_reset_state(s);
            }
            break;
        }

        case STATE_WAIT_FIRST_BIT:
        {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
            if (is_valid(s, di_samplenum(di), TIMING_RESPONSE_HIGH)) {
                c_put(di, s->fall, di_samplenum(di), s->out_ann, ANN_RESPONSE, "Response", "R");
                s->fall = di_samplenum(di);
                s->bytepos[s->bytepos_count++] = di_samplenum(di);
                s->state = STATE_WAIT_BIT_HIGH;
            } else {
                am230x_reset_state(s);
            }
            break;
        }

        case STATE_WAIT_BIT_HIGH:
        {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
            if (is_valid(s, di_samplenum(di), TIMING_BIT_LOW)) {
                s->rise = di_samplenum(di);
                s->state = STATE_WAIT_BIT_LOW;
            } else {
                am230x_reset_state(s);
            }
            break;
        }

        case STATE_WAIT_BIT_LOW:
        {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;

            int bit;
            if (is_valid(s, di_samplenum(di), TIMING_BIT_0_HIGH)) {
                bit = 0;
            } else if (is_valid(s, di_samplenum(di), TIMING_BIT_1_HIGH)) {
                bit = 1;
            } else {
                am230x_reset_state(s);
                break;
            }

            /* handle_byte logic */
            s->bits[s->bit_count++] = bit;
            {
                char tmp[16];
                snprintf(tmp, sizeof(tmp), "Bit: %d", bit);
                c_put(di, s->fall, di_samplenum(di), s->out_ann, ANN_BIT, tmp, bit ? "1" : "0");
            }
            s->fall = di_samplenum(di);
            s->state = STATE_WAIT_BIT_HIGH;

            if (s->bit_count % 8 == 0) {
                int byte_idx = s->bit_count / 8 - 1;
                uint8_t byte_val = bits2num(&s->bits[byte_idx * 8], 8);
                char tmp2[32], tmp3[32];
                snprintf(tmp2, sizeof(tmp2), "Byte: 0x%02x", byte_val);
                snprintf(tmp3, sizeof(tmp3), "0x%02x", byte_val);
                c_put(di, s->bytepos[s->bytepos_count - 1], di_samplenum(di), s->out_ann, ANN_BYTE, tmp2, tmp3);

                if (s->bit_count == 16) {
                    /* Humidity */
                    double h;
                    if (s->device == 1) {
                        h = bits2num(&s->bits[0], 8);
                    } else {
                        h = bits2num16(&s->bits[0], 16) / 10.0;
                    }
                    char htmp[64];
                    snprintf(htmp, sizeof(htmp), "Humidity: %.1f %%", h);
                    char hshort[32];
                    snprintf(hshort, sizeof(hshort), "RH = %.1f %%", h);
                    c_put(di, s->bytepos[s->bytepos_count - 2], di_samplenum(di), s->out_ann, ANN_HUMIDITY, htmp, hshort);
                } else if (s->bit_count == 32) {
                    /* Temperature */
                    double t;
                    if (s->device == 1) {
                        t = bits2num(&s->bits[16], 8);
                    } else {
                        t = bits2num16(&s->bits[17], 15) / 10.0;
                        if (s->bits[16] == 1) t = -t;
                    }
                    char ttmp[64];
                    snprintf(ttmp, sizeof(ttmp), "Temperature: %.1f \xc2\xb0""C", t);
                    char tshort[32];
                    snprintf(tshort, sizeof(tshort), "T = %.1f \xc2\xb0""C", t);
                    c_put(di, s->bytepos[s->bytepos_count - 2], di_samplenum(di), s->out_ann, ANN_TEMPERATURE, ttmp, tshort);
                } else if (s->bit_count == 40) {
                    /* Checksum */
                    uint8_t parity = bits2num(&s->bits[32], 8);
                    uint8_t checksum = 0;
                    for (int i = 0; i < 4; i++)
                        checksum += bits2num(&s->bits[i * 8], 8);
                    checksum %= 256;
                    if (parity == checksum) {
                        c_put(di, s->bytepos[s->bytepos_count - 1], di_samplenum(di), s->out_ann, ANN_CHECKSUM, "Checksum: OK", "OK");
                    } else {
                        c_put(di, s->bytepos[s->bytepos_count - 1], di_samplenum(di), s->out_ann, ANN_CHECKSUM, "Checksum: not OK", "NOK");
                    }
                    s->state = STATE_WAIT_END;
                }
                s->bytepos[s->bytepos_count++] = di_samplenum(di);
            }
            break;
        }

        case STATE_WAIT_END:
        {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;
            c_put(di, s->fall, di_samplenum(di), s->out_ann, ANN_END, "End", "E");
            am230x_reset_state(s);
            break;
        }
        }
    }
}

static void am230x_destroy(struct srd_decoder_inst *di)
{
    void *priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder am230x_c_decoder = {
    .id = "am230x_c",
    .name = "AM230x(C)",
    .longname = "Aosong AM230x/DHTxx/RHTxx (C)",
    .desc = "Aosong AM230x/DHTxx/RHTxx humidity/temperature sensor.",
    .license = "gplv2+",
    .channels = am230x_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = am230x_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = am230x_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = am230x_ann_rows,
    .inputs = am230x_inputs,
    .num_inputs = 1,
    .outputs = NULL,
    .num_outputs = 0,
    .binary = NULL,
    .num_binary = 0,
    .tags = am230x_tags,
    .num_tags = 2,
    .reset = am230x_reset,
    .start = am230x_start,
    .decode = am230x_decode,
    .destroy = am230x_destroy,
    .state_size = 0,
    .metadata = am230x_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    return &am230x_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}