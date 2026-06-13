#include "libsigrokdecode.h"
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum ow_state {
    STATE_INITIAL,
    STATE_IDLE,
    STATE_LOW,
    STATE_SLOT,
    STATE_WAIT_PRESENCE_FALL,  /* PRESENCE DETECT HIGH in Python */
    STATE_WAIT_PRESENCE_RISE,  /* PRESENCE DETECT LOW in Python */
    STATE_PRESENCE_DETECT,     /* After presence pulse, check RSTH */
};

/* Timing values in us: [0]=normal, [1]=overdrive */
struct ow_timing {
    double RSTL_min[2]; double RSTL_max[2];
    double RSTH_min[2];
    double PDH_min[2]; double PDH_max[2];
    double PDL_min[2]; double PDL_max[2];
    double SLOT_min[2]; double SLOT_max[2];
    double REC_min[2];
    double LOWR_min[2]; double LOWR_max[2];
};

static const struct ow_timing ow_timing = {
    .RSTL_min = {480.0, 48.0},
    .RSTL_max = {960.0, 80.0},
    .RSTH_min = {480.0, 48.0},
    .PDH_min  = {15.0,  2.0},
    .PDH_max  = {60.0,  6.0},
    .PDL_min  = {60.0,  8.0},
    .PDL_max  = {240.0, 24.0},
    .SLOT_min = {60.0,  6.0},
    .SLOT_max = {120.0, 16.0},
    .REC_min  = {1.0,   1.0},
    .LOWR_min = {1.0,   1.0},
    .LOWR_max = {15.0,  2.0},
};

static uint64_t us_to_samples(uint64_t samplerate, double us)
{
    return (uint64_t)(us * (double)samplerate / 1000000.0);
}

struct ow_priv {
    int state;
    uint8_t byte_val;
    int bit_cnt;
    int bit_val;
    uint64_t ss_rise;
    uint64_t ss_fall;
    int overdrive;
    uint64_t samplerate;
    int out_ann;
    int out_python;
};

#define ANN_BIT 0
#define ANN_WARN 1
#define ANN_RESET 2
#define ANN_PRESENCE 3
#define ANN_OVERDRIVE 4
#define NUM_ANN 5

static struct srd_channel ow_channels[] = {
    { "owr", "OWR", "1-Wire signal line", 0, SRD_CHANNEL_SDATA, "dec_onewire_link_chan_owr" },
};

static struct srd_decoder_option ow_options[] = {
    { "overdrive", NULL, "Start in overdrive speed", NULL, NULL },
};

static const char* ow_ann_labels[][3] = {
    { "", "bit", "Bit" },
    { "", "warnings", "Warnings" },
    { "", "reset", "Reset" },
    { "", "presence", "Presence" },
    { "", "overdrive", "Overdrive speed notifications" },
};

static const int ow_row_bits_classes[] = { ANN_BIT, ANN_RESET, ANN_PRESENCE };
static const int ow_row_info_classes[] = { ANN_OVERDRIVE };
static const int ow_row_warnings_classes[] = { ANN_WARN };
static const struct srd_c_ann_row ow_ann_rows[] = {
    { "bits", "Bits", ow_row_bits_classes, 3 },
    { "info", "Info", ow_row_info_classes, 1 },
    { "warnings", "Warnings", ow_row_warnings_classes, 1 },
};

static const char* ow_inputs[] = { "logic", NULL };
static const char* ow_outputs[] = { "onewire_link", NULL };
static const char* ow_tags[] = { "Embedded/industrial", NULL };

static void onewire_reset(struct srd_decoder_inst* di)
{
    if (!c_decoder_get_private(di)) {
        c_decoder_set_private(di, g_malloc0(sizeof(struct ow_priv)));
    }
    struct ow_priv* s = (struct ow_priv*)c_decoder_get_private(di);
    memset(s, 0, sizeof(struct ow_priv));
    s->state = STATE_INITIAL;
    s->bit_cnt = -1;
}

static void onewire_start(struct srd_decoder_inst* di)
{
    struct ow_priv* s = (struct ow_priv*)c_decoder_get_private(di);
    s->out_ann = c_reg_out(di, SRD_OUTPUT_ANN, "onewire_link");
    s->out_python = c_reg_out(di, SRD_OUTPUT_PROTO, "onewire_link");
    const char* od = c_opt_str(di, "overdrive", "no");
    s->overdrive = (strcmp(od, "yes") == 0) ? 1 : 0;
    s->bit_cnt = -1;
}

static void onewire_metadata(struct srd_decoder_inst* di, int key, uint64_t value)
{
    struct ow_priv* s = (struct ow_priv*)c_decoder_get_private(di);
    if (key == SRD_CONF_SAMPLERATE)
        s->samplerate = value;
}

static void onewire_checks(struct srd_decoder_inst* di, struct ow_priv* s)
{
    if (s->overdrive) {
        if (s->samplerate < 2000000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                "Sampling rate is too low. Must be above 2MHz for proper overdrive mode decoding.");
        } else if (s->samplerate < 5000000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                "Sampling rate is suggested to be above 5MHz for proper overdrive mode decoding.");
        }
    } else {
        if (s->samplerate < 400000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                "Sampling rate is too low. Must be above 400kHz for proper normal mode decoding.");
        } else if (s->samplerate < 1000000) {
            c_put(di, 0, 0, s->out_ann, ANN_WARN,
                "Sampling rate is suggested to be above 1MHz for proper normal mode decoding.");
        }
    }
}

static void onewire_decode(struct srd_decoder_inst* di)
{
    struct ow_priv* s = (struct ow_priv*)c_decoder_get_private(di);
    uint64_t samplerate = c_samplerate(di);

    if (!s->samplerate)
        s->samplerate = samplerate;
    if (!s->samplerate)
        return;

    onewire_checks(di, s);

    samplerate = s->samplerate;

    while (1) {
        int ret;
        int od = s->overdrive ? 1 : 0;

        switch (s->state) {

        case STATE_INITIAL: {
            ret = c_wait(di, CW_H(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->ss_rise = di_samplenum(di);
            s->state = STATE_IDLE;
            break;
        }

        case STATE_IDLE: {
            ret = c_wait(di, CW_F(0), CW_END);
            if (ret != SRD_OK)
                return;
            s->ss_fall = di_samplenum(di);

            /* Check REC timing */
            od = s->overdrive ? 1 : 0;
            if (s->ss_rise > 0) {
                double rec_us = (double)(s->ss_fall - s->ss_rise) / (double)samplerate * 1000000.0;
                if (rec_us < ow_timing.REC_min[od]) {
                    char txt[128];
                    snprintf(txt, sizeof(txt), "Recovery time not long enough, REC < %.0f us", ow_timing.REC_min[od]);
                    c_put(di, s->ss_rise, s->ss_fall, s->out_ann, ANN_WARN, txt);
                }
            }
            s->state = STATE_LOW;
            break;
        }

        case STATE_LOW: {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;

            s->ss_rise = di_samplenum(di);
            double time_us = (double)(di_samplenum(di) - s->ss_fall) / (double)samplerate * 1000000.0;
            od = s->overdrive ? 1 : 0;

            if (time_us >= ow_timing.RSTL_min[0]) {
                /* Normal-speed reset (>= 480us) always clears overdrive */
                if (s->overdrive) {
                    c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_OVERDRIVE,
                        "Exiting overdrive mode", "Overdrive off");
                    s->overdrive = 0;
                    od = 0;
                }
                /* Check RSTL timing */
                if (time_us < ow_timing.RSTL_min[0]) {
                    char txt[128];
                    snprintf(txt, sizeof(txt), "Reset pulse too short, RSTL < %.0f us", ow_timing.RSTL_min[0]);
                    c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                } else if (time_us > ow_timing.RSTL_max[0]) {
                    char txt[128];
                    snprintf(txt, sizeof(txt), "Reset pulse too long, RST > %.0f us", ow_timing.RSTL_max[0]);
                    c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                }
                c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_RESET,
                    "Reset", "Rst", "R");
                s->state = STATE_WAIT_PRESENCE_FALL;
            } else if (s->overdrive && time_us >= ow_timing.RSTL_min[1]) {
                /* Overdrive-speed reset */
                if (time_us > ow_timing.RSTL_max[1]) {
                    char txt[128];
                    snprintf(txt, sizeof(txt), "Reset pulse too long, RST > %.0f us", ow_timing.RSTL_max[1]);
                    c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                }
                c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_RESET,
                    "Reset", "Rst", "R");
                s->state = STATE_WAIT_PRESENCE_FALL;
            } else if (time_us < ow_timing.SLOT_max[od]) {
                /* Time slot */
                /* Check LOWR timing */
                if (time_us < ow_timing.LOWR_min[od]) {
                    char txt[128];
                    snprintf(txt, sizeof(txt), "Low signal not long enough, LOW < %.0f us", ow_timing.LOWR_min[od]);
                    c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                }

                /* Determine bit value */
                int bit_val;
                if (time_us < ow_timing.LOWR_max[od]) {
                    bit_val = 1; /* Short pulse = bit 1 */
                } else {
                    bit_val = 0; /* Long pulse = bit 0 */
                }

                uint64_t bit_end = s->ss_fall + us_to_samples(samplerate, ow_timing.SLOT_min[od]);
                char bit_long[16], bit_short[4];
                snprintf(bit_long, sizeof(bit_long), "Bit: %d", bit_val);
                snprintf(bit_short, sizeof(bit_short), "%d", bit_val);
                c_put(di, s->ss_fall, bit_end, s->out_ann, ANN_BIT,
                    bit_long, bit_short);

                unsigned char bit_byte = (unsigned char)bit_val;
                c_proto(di, s->ss_fall, bit_end, s->out_python, "BIT", C_U8(bit_byte), C_END);

                /* Handle byte assembly */
                if (s->bit_cnt >= 0) {
                    s->byte_val |= (bit_val << s->bit_cnt);
                    s->bit_cnt++;
                }
                if (s->bit_cnt == 8) {
                    if ((s->byte_val == 0x3C || s->byte_val == 0x69) && !s->overdrive) {
                        s->overdrive = 1;
                        c_put(di, di_samplenum(di), di_samplenum(di), s->out_ann, ANN_OVERDRIVE,
                            "Entering overdrive mode", "Overdrive on");
                    }
                    s->bit_cnt = -1;
                    s->byte_val = 0;
                }

                s->state = STATE_SLOT;
            } else {
                /* Unknown - too long for slot, too short for reset */
                c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN,
                    "Ambiguous pulse width");
                s->state = STATE_IDLE;
            }
            break;
        }

        case STATE_SLOT: {
            od = s->overdrive ? 1 : 0;
            uint64_t slot_min_samples = us_to_samples(samplerate, ow_timing.SLOT_min[od]);
            uint64_t skip_count = 0;
            if (s->ss_fall + slot_min_samples > di_samplenum(di))
                skip_count = s->ss_fall + slot_min_samples - di_samplenum(di);

            ret = c_wait(di, CW_F(0), CW_OR, CW_SKIP(skip_count), CW_END);
            if (ret != SRD_OK)
                return;

            if ((di_matched(di) & 0b1) && !(di_matched(di) & 0b10)) {
                /* Falling edge before SLOT min - slot too short */
                char txt[128];
                snprintf(txt, sizeof(txt), "Time slot not long enough, SLOT < %.0f us", ow_timing.SLOT_min[od]);
                c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                s->ss_fall = di_samplenum(di);
                s->state = STATE_LOW;
            } else {
                /* SLOT min timeout - normal end */
                s->state = STATE_IDLE;
            }
            break;
        }

        case STATE_WAIT_PRESENCE_FALL: {
            /* Wait for falling edge (presence signal) with timeout (PDH max) */
            uint64_t pdh_max_samples = us_to_samples(samplerate, ow_timing.PDH_max[od]);
            uint64_t skip_count = 0;
            if (s->ss_rise + pdh_max_samples > di_samplenum(di))
                skip_count = s->ss_rise + pdh_max_samples - di_samplenum(di);

            ret = c_wait(di, CW_F(0), CW_OR, CW_SKIP(skip_count), CW_END);
            if (ret != SRD_OK)
                return;

            double time_us = (double)(di_samplenum(di) - s->ss_rise) / (double)samplerate * 1000000.0;

            if ((di_matched(di) & 0b1) && !(di_matched(di) & 0b10)) {
                /* Presence detected (falling edge, not timeout) */
                if (time_us < ow_timing.PDH_min[od]) {
                    char txt[64];
                    snprintf(txt, sizeof(txt), "Presence detect too early, PDH < %.0f us", ow_timing.PDH_min[od]);
                    c_put(di, s->ss_rise, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                }
                s->ss_fall = di_samplenum(di);
                s->state = STATE_WAIT_PRESENCE_RISE;
            } else {
                /* No presence detected (timeout) */
                c_put(di, s->ss_rise, di_samplenum(di), s->out_ann, ANN_PRESENCE,
                    "Presence: false", "Presence", "Pres", "P");
                unsigned char pres_byte = 0;
                c_proto(di, s->ss_rise, di_samplenum(di), s->out_python, "RESET/PRESENCE", C_U8(pres_byte), C_END);
                s->state = STATE_IDLE;
            }
            break;
        }

        case STATE_WAIT_PRESENCE_RISE: {
            ret = c_wait(di, CW_R(0), CW_END);
            if (ret != SRD_OK)
                return;

            /* Check presence low timing */
            double pdl_us = (double)(di_samplenum(di) - s->ss_fall) / (double)samplerate * 1000000.0;
            od = s->overdrive ? 1 : 0;
            if (pdl_us < ow_timing.PDL_min[od]) {
                char txt[64];
                snprintf(txt, sizeof(txt), "Presence detect too short, PDL < %.0f us", ow_timing.PDL_min[od]);
                c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
            } else if (pdl_us > ow_timing.PDL_max[od]) {
                char txt[64];
                snprintf(txt, sizeof(txt), "Presence detect too long, PDL > %.0f us", ow_timing.PDL_max[od]);
                c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_WARN, txt);
            }

            c_put(di, s->ss_fall, di_samplenum(di), s->out_ann, ANN_PRESENCE,
                "Presence: true", "Presence", "Pres", "P");
            unsigned char pres_byte = 1;
            c_proto(di, s->ss_rise, di_samplenum(di), s->out_python, "RESET/PRESENCE", C_U8(pres_byte), C_END);

            s->ss_rise = di_samplenum(di);
            s->state = STATE_PRESENCE_DETECT;
            break;
        }

        case STATE_PRESENCE_DETECT: {
            od = s->overdrive ? 1 : 0;
            uint64_t rsth_min_samples = us_to_samples(samplerate, ow_timing.RSTH_min[od]);
            uint64_t skip_count = 0;
            if (s->ss_rise + rsth_min_samples > di_samplenum(di))
                skip_count = s->ss_rise + rsth_min_samples - di_samplenum(di);

            ret = c_wait(di, CW_F(0), CW_OR, CW_SKIP(skip_count), CW_END);
            if (ret != SRD_OK)
                return;

            if ((di_matched(di) & 0b1) && !(di_matched(di) & 0b10)) {
                /* Falling edge before RSTH min */
                char txt[128];
                snprintf(txt, sizeof(txt), "Reset high not long enough, RSTH < %.0f us", ow_timing.RSTH_min[od]);
                c_put(di, s->ss_rise, di_samplenum(di), s->out_ann, ANN_WARN, txt);
                s->ss_fall = di_samplenum(di);
                s->state = STATE_LOW;
            } else {
                /* RSTH min timeout - normal end */
                s->bit_cnt = 0;
                s->byte_val = 0;
                s->state = STATE_IDLE;
            }
            break;
        }

        }
    }
}

static void onewire_destroy(struct srd_decoder_inst* di)
{
    void* priv = c_decoder_get_private(di);
    if (priv) {
        g_free(priv);
        c_decoder_set_private(di, NULL);
    }
}

struct srd_c_decoder onewire_c_decoder = {
    .id = "onewire_c",
    .name = "OneWire link layer(C)",
    .longname = "1-Wire serial communication bus (link layer)(C)",
    .desc = "Bidirectional, half-duplex, asynchronous serial bus.(C implementation)",
    .license = "gplv2+",
    .channels = ow_channels,
    .num_channels = 1,
    .optional_channels = NULL,
    .num_optional_channels = 0,
    .options = ow_options,
    .num_options = 1,
    .num_annotations = NUM_ANN,
    .ann_labels = ow_ann_labels,
    .num_annotation_rows = 3,
    .annotation_rows = ow_ann_rows,
    .inputs = ow_inputs,
    .num_inputs = 1,
    .outputs = ow_outputs,
    .num_outputs = 1,
    .binary = NULL,
    .num_binary = 0,
    .tags = ow_tags,
    .num_tags = 1,
    .reset = onewire_reset,
    .start = onewire_start,
    .decode = onewire_decode,
    .destroy = onewire_destroy,
    .state_size = 0,
    .metadata = onewire_metadata,
};

SRD_C_DECODER_EXPORT struct srd_c_decoder* srd_c_decoder_entry(void)
{
    ow_options[0].idn = "dec_onewire_link_opt_overdrive";
    ow_options[0].def = g_variant_new_string("no");
    GSList* od_vals = NULL;
    od_vals = g_slist_append(od_vals, g_variant_new_string("yes"));
    od_vals = g_slist_append(od_vals, g_variant_new_string("no"));
    ow_options[0].values = od_vals;
    return &onewire_c_decoder;
}

SRD_C_DECODER_EXPORT int srd_c_decoder_api_version(void)
{
    return SRD_C_DECODER_API_VERSION;
}